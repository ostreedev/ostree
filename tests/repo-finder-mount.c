/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <locale.h>

#include "ostree-autocleanups.h"
#include "ostree-remote-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-mount.h"
#include "ostree-types.h"
#include "test-mock-gio.h"

static void
result_cb (GObject      *source_object,
           GAsyncResult *result,
           gpointer      user_data)
{
  GAsyncResult **result_out = user_data;
  *result_out = g_object_ref (result);
}

static void
collection_ref_free0 (OstreeCollectionRef *ref)
{
  g_clear_pointer (&ref, (GDestroyNotify) ostree_collection_ref_free);
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");

  if (argc < 5 || (argc % 2) != 1)
    {
      g_printerr ("Usage: %s REPO MOUNT-ROOT COLLECTION-ID REF-NAME [COLLECTION-ID REF-NAME …]\n", argv[0]);
      return 1;
    }

  g_autoptr(GMainContext) context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  g_autoptr(OstreeRepo) parent_repo = ostree_repo_open_at (AT_FDCWD, argv[1], NULL, &error);
  g_assert_no_error (error);

  /* Set up a mock volume. */
  g_autoptr(GFile) mount_root = g_file_new_for_commandline_arg (argv[2]);
  g_autoptr(GMount) mount = G_MOUNT (ostree_mock_mount_new ("mount", mount_root));

  g_autoptr(GList) mounts = g_list_prepend (NULL, mount);

  g_autoptr(GVolumeMonitor) monitor = ostree_mock_volume_monitor_new (mounts, NULL);
  g_autoptr(OstreeRepoFinderMount) finder = ostree_repo_finder_mount_new (monitor);

  /* Resolve the refs. */
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func ((GDestroyNotify) collection_ref_free0);

  for (gsize i = 3; i < argc; i += 2)
    {
      const char *collection_id = argv[i];
      const char *ref_name = argv[i + 1];

      g_ptr_array_add (refs, ostree_collection_ref_new (collection_id, ref_name));
    }

  g_ptr_array_add (refs, NULL);  /* NULL terminator */

  g_autoptr(GAsyncResult) result = NULL;
  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder),
                                    (const OstreeCollectionRef * const *) refs->pdata,
                                    parent_repo, NULL, result_cb, &result);

  while (result == NULL)
    g_main_context_iteration (context, TRUE);

  g_autoptr(GPtrArray) results = ostree_repo_finder_resolve_finish (OSTREE_REPO_FINDER (finder),
                                                                    result, &error);
  g_assert_no_error (error);

  /* Check that the results are correct: the invalid refs should have been
   * ignored, and the valid results canonicalised and deduplicated. */
  for (gsize i = 0; i < results->len; i++)
    {
      const OstreeRepoFinderResult *result = g_ptr_array_index (results, i);
      GHashTableIter iter;
      OstreeCollectionRef *ref;
      const gchar *checksum;

      g_hash_table_iter_init (&iter, result->ref_to_checksum);

      while (g_hash_table_iter_next (&iter, (gpointer *) &ref, (gpointer *) &checksum))
        g_print ("%" G_GSIZE_FORMAT " %s %s %s %s\n",
                 i, ostree_remote_get_name (result->remote),
                 ref->collection_id, ref->ref_name,
                 checksum);
    }

  g_main_context_pop_thread_default (context);

  return 0;
}
