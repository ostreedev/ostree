/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>

#include "otutil.h"

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>

static const char *
find_first_file (GFileTest test, const char *name, ...) G_GNUC_NULL_TERMINATED;

static const char *
find_first_file (GFileTest test, const char *name, ...)
{
  va_list args;

  va_start (args, name);

  do
    {
      if (g_file_test (name, test))
        break;
      name = va_arg (args, const char *);
    }
  while (name != NULL);

  va_end (args);

  return name;
}

static void
split_configure_make_args (int argc,
                           char **argv,
                           GPtrArray **out_configure_args,
                           GPtrArray **out_make_args,
                           GPtrArray **out_makeinstall_args)
{
  int i;

  *out_configure_args = g_ptr_array_new ();
  *out_make_args = g_ptr_array_new ();
  *out_makeinstall_args = g_ptr_array_new ();

  for (i = 1; i < argc; i++)
    {
      if (g_str_has_prefix (argv[i], "--"))
        g_ptr_array_add (*out_configure_args, argv[i]);
      else if (g_str_has_prefix (argv[i], "DESTDIR="))
        g_ptr_array_add (*out_makeinstall_args, argv[i]);
      else
        g_ptr_array_add (*out_make_args, argv[i]);
    }
}

static void
spawn_sync_or_fatal (char **args, char **env, GSpawnFlags flags)
{
  GError *error = NULL;
  int estatus;
  char **iter;

  g_print ("osbuild: running: ");
  for (iter = args; *iter; iter++)
    g_print ("%s ", *iter);
  g_print ("\n");
  if (g_spawn_sync (NULL, args, env, flags, NULL, NULL, NULL, NULL, &estatus, &error))
    { 
      if (WIFEXITED (estatus) && WEXITSTATUS (estatus) == 0)
        {
          g_message ("Subprocess %s exited successfully\n", args[0]);
        }
      else
        {
          if (WIFEXITED (estatus))
            g_error ("Subprocess %s exited with code %d\n", args[0], WEXITSTATUS (estatus));
          else if (WIFSIGNALED (estatus))
            g_error ("Subprocess %s killed by signal %d\n", args[0], WTERMSIG (estatus));
          else
            g_error ("Subprocess %s terminated with status %d\n", args[0], estatus);
          exit (1);
        }
    }
  else
    {
      g_error ("Failed to execute %s: %s\n", args[0], error->message);
      exit (1);
    }
}

static void
ptr_array_extend (GPtrArray *dest, GPtrArray *to_append)
{
  int i;

  for (i = 0; i < to_append->len; i++)
    g_ptr_array_add (dest, to_append->pdata[i]);
}

int
main (int    argc,
      char **argv)
{
  GPtrArray *config_args;
  GPtrArray *make_args;
  GPtrArray *makeinstall_args;
  GPtrArray *args;
  char **subprocess_env;

  g_type_init ();

  g_set_prgname (argv[0]);

  args = g_ptr_array_new ();

  subprocess_env = g_get_environ ();
  ot_g_environ_setenv (subprocess_env, "LANG", "C", TRUE);
  ot_g_environ_unsetenv (subprocess_env, "LC_ALL");

  split_configure_make_args (argc, argv, &config_args, &make_args, &makeinstall_args);

  if (!g_file_test ("./configure", G_FILE_TEST_IS_EXECUTABLE))
    {
      const char *autogen;
      char **autogen_env;
      
      autogen = find_first_file (G_FILE_TEST_IS_EXECUTABLE, "./autogen", "./autogen.sh", NULL);
      if (!autogen)
        ot_util_fatal_literal ("No executable configure or autogen script found"); 

      autogen_env = g_strdupv (subprocess_env);
      ot_g_environ_setenv (autogen_env, "NOCONFIGURE", "1", TRUE);
      
      g_ptr_array_set_size (args, 0);
      g_ptr_array_add (args, (char*) autogen);
      g_ptr_array_add (args, NULL);
      spawn_sync_or_fatal ((char**)args->pdata, autogen_env, 0);
    }

  if (!g_file_test ("./configure", G_FILE_TEST_IS_EXECUTABLE))
    ot_util_fatal_literal ("autogen script failed to generate a configure script");

  g_ptr_array_set_size (args, 0);
  g_ptr_array_add (args, "./configure");
  ptr_array_extend (args, config_args);
  g_ptr_array_add (args, NULL);
  spawn_sync_or_fatal ((char**)args->pdata, subprocess_env, 0);
    
  g_ptr_array_set_size (args, 0);
  g_ptr_array_add (args, "make");
  ptr_array_extend (args, make_args);
  g_ptr_array_add (args, NULL);
  spawn_sync_or_fatal ((char**)args->pdata, subprocess_env, G_SPAWN_SEARCH_PATH);
  
  g_ptr_array_set_size (args, 0);
  g_ptr_array_add (args, "make");
  g_ptr_array_add (args, "install");
  ptr_array_extend (args, makeinstall_args);
  g_ptr_array_add (args, NULL);
  spawn_sync_or_fatal ((char**)args->pdata, subprocess_env, G_SPAWN_SEARCH_PATH);

  return 0;
}
