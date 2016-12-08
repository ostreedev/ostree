/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
 */

#include "config.h"

#include <libglnx.h>
#include <gio/gunixinputstream.h>

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

/* XXX This belongs in libotutil. */
#include "ostree-chain-input-stream.h"

static gboolean opt_stdin;
static char **opt_keyrings;

static GOptionEntry option_entries[] = {
  { "keyring", 'k', 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_keyrings, "Import keys from a keyring file (repeatable)", "FILE" },
  { "stdin", 0, 0, G_OPTION_ARG_NONE, &opt_stdin, "Import keys from standard input", NULL },
  { NULL }
};

static gboolean
open_source_stream (GInputStream **out_source_stream,
                    GCancellable *cancellable,
                    GError **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  gboolean ret = FALSE;

  if (opt_keyrings != NULL)
    n_keyrings = g_strv_length (opt_keyrings);

  if (opt_stdin)
    {
      source_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
    }
  else
    {
      g_autoptr(GPtrArray) streams = NULL;
      guint ii;

      streams = g_ptr_array_new_with_free_func (g_object_unref);

      for (ii = 0; ii < n_keyrings; ii++)
        {
          g_autoptr(GFile) file = NULL;
          GFileInputStream *input_stream = NULL;

          file = g_file_new_for_path (opt_keyrings[ii]);
          input_stream = g_file_read (file, cancellable, error);

          if (input_stream == NULL)
            goto out;

          /* Takes ownership. */
          g_ptr_array_add (streams, input_stream);
        }

       /* Chain together all the --keyring options as one long stream. */
       source_stream = (GInputStream *) ostree_chain_input_stream_new (streams);
    }

  *out_source_stream = g_steal_pointer (&source_stream);

  ret = TRUE;

out:
  return ret;
}

gboolean
ot_remote_builtin_gpg_import (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autoptr(GInputStream) source_stream = NULL;
  const char *remote_name;
  const char * const *key_ids;
  guint imported = 0;
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME [KEY-ID...] - Import GPG keys");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  if (opt_stdin && opt_keyrings != NULL)
    {
      ot_util_usage_error (context, "--keyring and --stdin are mutually exclusive", error);
      goto out;
    }

  remote_name = argv[1];
  key_ids = (argc > 2) ? (const char * const *) argv + 2 : NULL;

  if (!open_source_stream (&source_stream, cancellable, error))
    goto out;

  if (!ostree_repo_remote_gpg_import (repo, remote_name, source_stream,
                                      key_ids, &imported, cancellable, error))
    goto out;

  /* XXX If we ever add internationalization, use ngettext() here. */
  g_print ("Imported %u GPG key%s to remote \"%s\"\n",
           imported, (imported == 1) ? "" : "s", remote_name);

  ret = TRUE;

 out:
  return ret;
}
