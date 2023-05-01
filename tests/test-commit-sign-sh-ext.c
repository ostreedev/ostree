/*
 * Copyright (C) 2021 Red Hat, Inc.
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "libglnx.h"
#include <ostree.h>

static void
assert_error_contains (GError **error, const char *msg)
{
  g_assert (error != NULL);
  GError *actual = *error;
  g_assert (actual != NULL);
  if (strstr (actual->message, msg) == NULL)
    g_error ("%s does not contain %s", actual->message, msg);
  g_clear_error (error);
}

// Perhaps in the future we hook this up to a fuzzer
static GBytes *
corrupt (GBytes *input)
{
  gsize len = 0;
  const guint8 *buf = g_bytes_get_data (input, &len);
  g_assert_cmpint (len, >, 0);
  g_assert_cmpint (len, <, G_MAXINT);
  g_autofree char *newbuf = g_memdup2 (buf, len);
  g_assert (newbuf != NULL);
  int o = g_random_int_range (0, len);
  newbuf[o] = (newbuf[0] + 1);

  return g_bytes_new_take (g_steal_pointer (&newbuf), len);
}

static gboolean
run (GError **error)
{
  g_autoptr (OstreeRepo) repo = ostree_repo_open_at (AT_FDCWD, "repo", NULL, error);
  if (!repo)
    return FALSE;

  g_autofree char *rev = NULL;
  if (!ostree_repo_resolve_rev (repo, "origin:main", FALSE, &rev, error))
    return FALSE;
  g_assert (rev);
  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &commit, error))
    return FALSE;
  g_assert (commit);

  g_autoptr (GVariant) detached_meta = NULL;
  if (!ostree_repo_read_commit_detached_metadata (repo, rev, &detached_meta, NULL, error))
    return FALSE;
  g_assert (detached_meta);

  g_autoptr (GBytes) commit_bytes = g_variant_get_data_as_bytes (commit);
  g_autoptr (GBytes) detached_meta_bytes = g_variant_get_data_as_bytes (detached_meta);
  g_autofree char *verify_report = NULL;
  if (!ostree_repo_signature_verify_commit_data (repo, "origin", commit_bytes, detached_meta_bytes,
                                                 0, &verify_report, error))
    return FALSE;

  if (ostree_repo_signature_verify_commit_data (repo, "origin", commit_bytes, detached_meta_bytes,
                                                OSTREE_REPO_VERIFY_FLAGS_NO_GPG
                                                    | OSTREE_REPO_VERIFY_FLAGS_NO_SIGNAPI,
                                                &verify_report, error))
    return glnx_throw (error, "Should not have validated");
  assert_error_contains (error, "No commit verification types enabled");

  // No signatures
  g_autoptr (GBytes) empty = g_bytes_new_static ("", 0);
  if (ostree_repo_signature_verify_commit_data (repo, "origin", commit_bytes, empty, 0,
                                                &verify_report, error))
    return glnx_throw (error, "Should not have validated");
  assert_error_contains (error, "no signatures found");
  // No such remote
  if (ostree_repo_signature_verify_commit_data (repo, "nosuchremote", commit_bytes,
                                                detached_meta_bytes, 0, &verify_report, error))
    return glnx_throw (error, "Should not have validated");
  assert_error_contains (error, "Remote \"nosuchremote\" not found");

  // Corrupted commit
  g_autoptr (GBytes) corrupted_commit = corrupt (commit_bytes);
  if (ostree_repo_signature_verify_commit_data (repo, "origin", corrupted_commit,
                                                detached_meta_bytes, 0, &verify_report, error))
    return glnx_throw (error, "Should not have validated");
  assert_error_contains (error, "BAD signature");

  return TRUE;
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  if (!run (&error))
    {
      g_printerr ("error: %s\n", error->message);
      exit (1);
    }
}
