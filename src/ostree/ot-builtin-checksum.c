/*
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

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-checksum.xml) when changing the option list.
 */

static gboolean opt_ignore_xattrs;

static GOptionEntry options[] = {
  { "ignore-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_ignore_xattrs, "Don't include xattrs in checksum", NULL },
  { NULL }
};

typedef struct {
  GError **error;
  gboolean success;
  GMainLoop *loop;
} AsyncChecksumData;

static void
on_checksum_received (GObject    *obj,
                      GAsyncResult  *result,
                      gpointer       user_data)
{
  AsyncChecksumData *data = user_data;

  g_autofree guchar *csum_bytes = NULL;
  data->success =
    ostree_checksum_file_async_finish ((GFile*)obj, result, &csum_bytes, data->error);
  if (data->success)
    {
      char csum[OSTREE_SHA256_STRING_LEN+1];
      ostree_checksum_inplace_from_bytes (csum_bytes, csum);
      g_print ("%s\n", csum);
    }

  g_main_loop_quit (data->loop);
}

gboolean
ostree_builtin_checksum (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context =
    g_option_context_new ("PATH");
  if (!ostree_option_context_parse (context, options, &argc, &argv,
                                    invocation, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return glnx_throw (error, "A filename must be given");
  const char *path = argv[1];

  /* for test coverage, use the async API if no flags are needed */
  if (!opt_ignore_xattrs)
    {
      g_autoptr(GFile) f = g_file_new_for_path (path);
      g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);

      AsyncChecksumData data = { 0, };

      data.loop = loop;
      data.error = error;
      ostree_checksum_file_async (f, OSTREE_OBJECT_TYPE_FILE, G_PRIORITY_DEFAULT,
                                  cancellable, on_checksum_received, &data);
      g_main_loop_run (data.loop);
      return data.success;
    }

  g_autofree char *checksum = NULL;
  if (!ostree_checksum_file_at (AT_FDCWD, path, NULL, OSTREE_OBJECT_TYPE_FILE,
                                OSTREE_CHECKSUM_FLAGS_IGNORE_XATTRS, &checksum,
                                cancellable, error))
    return FALSE;

  g_print ("%s\n", checksum);
  return TRUE;
}
