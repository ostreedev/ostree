/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"

#include <string.h>

static GOptionEntry options[] = {
  { NULL }
};

typedef struct {
  GError **error;
  GMainLoop *loop;
} AsyncChecksumData;

static void
on_checksum_received (GObject    *obj,
                      GAsyncResult  *result,
                      gpointer       user_data)
{
  g_autofree guchar *csum = NULL;
  g_autofree char *checksum = NULL;
  AsyncChecksumData *data = user_data;

  if (ostree_checksum_file_async_finish ((GFile*)obj, result, &csum, data->error))
    {
      checksum = ostree_checksum_from_bytes (csum);
      g_print ("%s\n", checksum);
    }
  
  g_main_loop_quit (data->loop);
}

gboolean
ostree_builtin_checksum (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  gboolean ret = FALSE;
  g_autoptr(GFile) f = NULL;
  AsyncChecksumData data = { 0, };

  context = g_option_context_new ("PATH - Checksum a file or directory");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NO_REPO, NULL, cancellable, error))
    goto out;

  if (argc > 1)
    f = g_file_new_for_path (argv[1]);
  else
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A filename must be given");
      goto out;
    }

  data.loop = g_main_loop_new (NULL, FALSE);
  data.error = error;
  ostree_checksum_file_async (f, OSTREE_OBJECT_TYPE_FILE, G_PRIORITY_DEFAULT, cancellable, on_checksum_received, &data);
  
  g_main_loop_run (data.loop);

  ret = TRUE;
 out:
  if (data.loop)
    g_main_loop_unref (data.loop);
  return ret;
}
