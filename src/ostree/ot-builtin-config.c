/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
#include "otutil.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-config.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

static gboolean
split_key_string (const char   *k,
                  char       **out_section,
                  char       **out_value,
                  GError     **error)
{
  const char *dot = strchr (k, '.');
  
  if (!dot)
    {
      return glnx_throw (error,
                        "Key must be of the form \"sectionname.keyname\"");
    }

  *out_section = g_strndup (k, dot - k);
  *out_value = g_strdup (dot + 1);

  return TRUE;
}

gboolean
ostree_builtin_config (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  const char *op;
  const char *section_key;
  const char *value;
  g_autofree char *section = NULL;
  g_autofree char *key = NULL;
  GKeyFile *config = NULL;

  context = g_option_context_new ("(get KEY|set KEY VALUE)");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OPERATION must be specified", error);
      goto out;
    }

  op = argv[1];

  if (!strcmp (op, "set"))
    {
      if (argc < 4)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "KEY and VALUE must be specified");
          goto out;
        }

      section_key = argv[2];
      value = argv[3];

      if (!split_key_string (section_key, &section, &key, error))
        goto out;

      config = ostree_repo_copy_config (repo);
      g_key_file_set_string (config, section, key, value);

      if (!ostree_repo_write_config (repo, config, error))
        goto out;
    }
  else if (!strcmp (op, "get"))
    {
      GKeyFile *readonly_config = NULL;
      g_autofree char *value = NULL;
      if (argc < 3)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "KEY must be specified");
          goto out;
        }

      section_key = argv[2];

      if (!split_key_string (section_key, &section, &key, error))
        goto out;

      readonly_config = ostree_repo_get_config (repo);
      value = g_key_file_get_string (readonly_config, section, key, error);
      if (value == NULL)
        goto out;

      g_print ("%s\n", value);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown operation %s", op);
      goto out;
    }
  
  ret = TRUE;
 out:
  if (config)
    g_key_file_free (config);
  return ret;
}
