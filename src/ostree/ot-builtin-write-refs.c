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

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ostree_builtin_write_refs (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gsize len;
  gs_unref_object GInputStream *instream = NULL;
  gs_unref_object GDataInputStream *datastream = NULL;
  gs_free char *line = NULL;

  context = g_option_context_new ("Import newline-separated pairs of REF REVISION");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  instream = (GInputStream*)g_unix_input_stream_new (0, FALSE);
  datastream = g_data_input_stream_new (instream);

  while ((line = g_data_input_stream_read_line (datastream, &len,
                                                cancellable, &temp_error)) != NULL)
    {
      const char *spc = strchr (line, ' ');
      gs_free char *ref = NULL;

      if (!spc || spc == line)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid ref input");
          goto out;
        }

      ref = g_strndup (line, spc - line);
      if (!ostree_validate_structureof_checksum_string (spc + 1, error))
        goto out;

      if (!ostree_repo_write_ref (repo, NULL, ref, spc + 1, error))
        goto out;

      g_free (line);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
