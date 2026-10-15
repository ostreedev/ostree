/*
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

/* Helper for tests/test-pull-s3-faults.sh: run several pulls of "main" in one
 * process, as a long-lived library user would, iterating the main context
 * between them so anything a pull left running gets to finish. Checks that
 * every pull failed (the server is set up to make them fail, or each pull is
 * cancelled after CANCEL_MS), that no temporary file is still held open
 * afterwards, and that nothing logged a critical warning.
 *
 *   s3-pull-repeat WORKDIR URL COUNT [CANCEL_MS]
 */

#include "config.h"

#include <dirent.h>
#include <ostree.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Open file descriptors that point at deleted files, i.e. temporary files. */
static guint
count_deleted_fds (void)
{
  guint n = 0;
  DIR *d = opendir ("/proc/self/fd");
  struct dirent *e;
  g_assert (d != NULL);
  while ((e = readdir (d)) != NULL)
    {
      char target[512];
      g_autofree char *path = g_strdup_printf ("/proc/self/fd/%s", e->d_name);
      ssize_t len = readlink (path, target, sizeof target - 1);
      if (len < 0)
        continue;
      target[len] = '\0';
      if (strstr (target, "(deleted)") != NULL)
        n++;
    }
  closedir (d);
  return n;
}

static gboolean
cancel_cb (gpointer user_data)
{
  g_cancellable_cancel (user_data);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  if (argc != 4 && argc != 5)
    {
      g_printerr ("usage: %s WORKDIR URL COUNT [CANCEL_MS]\n", argv[0]);
      return 2;
    }
  const char *workdir = argv[1];
  const char *url = argv[2];
  const int count = atoi (argv[3]);
  const int cancel_ms = argc == 5 ? atoi (argv[4]) : 0;

  /* A failed precondition (g_return_if_fail and the like) is a test failure. */
  g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL);

  for (int i = 0; i < count; i++)
    {
      g_autoptr (GError) error = NULL;
      g_autofree char *path = g_strdup_printf ("%s/repeat-%d", workdir, i);
      g_autoptr (GFile) file = g_file_new_for_path (path);
      g_autoptr (OstreeRepo) repo = ostree_repo_new (file);
      if (!ostree_repo_create (repo, OSTREE_REPO_MODE_ARCHIVE, NULL, &error))
        g_error ("%s", error->message);

      g_auto (GVariantBuilder) remote_opts = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&remote_opts, "{sv}", "gpg-verify", g_variant_new_boolean (FALSE));
      if (!ostree_repo_remote_add (repo, "origin", url, g_variant_builder_end (&remote_opts), NULL,
                                   &error))
        g_error ("%s", error->message);

      const char *refs[] = { "main", NULL };
      g_auto (GVariantBuilder) pull_opts = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&pull_opts, "{sv}", "refs", g_variant_new_strv (refs, -1));
      g_variant_builder_add (&pull_opts, "{sv}", "disable-static-deltas",
                             g_variant_new_boolean (TRUE));
      g_variant_builder_add (&pull_opts, "{sv}", "n-network-retries", g_variant_new_uint32 (0));
      g_autoptr (GCancellable) cancellable = g_cancellable_new ();
      if (cancel_ms > 0)
        g_timeout_add_full (G_PRIORITY_DEFAULT, cancel_ms, cancel_cb, g_object_ref (cancellable),
                            g_object_unref);
      if (ostree_repo_pull_with_options (repo, "origin", g_variant_builder_end (&pull_opts), NULL,
                                         cancellable, &error))
        {
          g_printerr ("pull %d unexpectedly succeeded\n", i);
          return 1;
        }
      g_print ("pull %d failed as expected: %s\n", i, error->message);

      /* Give anything the pull left behind time to run. */
      const gint64 until = g_get_monotonic_time () + 8 * G_TIME_SPAN_SECOND;
      while (g_get_monotonic_time () < until)
        {
          while (g_main_context_iteration (NULL, FALSE))
            ;
          g_usleep (50 * 1000);
        }
    }

  guint leftover = count_deleted_fds ();
  if (leftover != 0)
    {
      g_printerr ("%u temporary file(s) still open after the pulls\n", leftover);
      return 1;
    }
  g_print ("no temporary files left open\n");
  return 0;
}
