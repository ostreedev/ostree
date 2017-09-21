/*
 * Copyright (C) 2016 Red Hat, Inc.
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
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

#include "libostreetest.h"

typedef struct {
  OstreeRepo *repo;
} TestData;

static void
test_data_init (TestData *td)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  g_autofree char *http_address = NULL;
  g_autofree char *repo_url = NULL;

  td->repo = ot_test_setup_repo (NULL, error);
  if (!td->repo)
    goto out;

  if (!ot_test_run_libtest ("setup_fake_remote_repo1 archive", error))
    goto out;

  if (!g_file_get_contents ("httpd-address", &http_address, NULL, error))
    goto out;

  g_strstrip (http_address);

  repo_url = g_strconcat (http_address, "/ostree/gnomerepo", NULL);

  { g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
    g_autoptr(GVariant) opts = NULL;

    g_variant_builder_add (builder, "{s@v}", "gpg-verify", g_variant_new_variant (g_variant_new_boolean (FALSE)));
    opts = g_variant_ref_sink (g_variant_builder_end (builder));

    if (!ostree_repo_remote_change (td->repo, NULL, OSTREE_REPO_REMOTE_CHANGE_ADD,
                                    "origin", repo_url, opts, NULL, error))
      goto out;
  }

 out:
  g_assert_no_error (local_error);
}

static void
test_pull_multi_nochange (gconstpointer data)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  TestData *td = (void*)data;
  char *refs[] = { "main", NULL };

  if (!ostree_repo_pull (td->repo, "origin", (char**)&refs, 0, NULL, NULL, error))
    goto out;
  if (!ostree_repo_pull (td->repo, "origin", (char**)&refs, 0, NULL, NULL, error))
    goto out;
  if (!ostree_repo_pull (td->repo, "origin", (char**)&refs, 0, NULL, NULL, error))
    goto out;
  
 out:
  g_assert_no_error (local_error);
}

static void
test_pull_multi_error_then_ok (gconstpointer data)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  
  TestData *td = (void*)data;
  char *ok_refs[] = { "main", NULL };
  char *bad_refs[] = { "nosuchbranch", NULL };

  for (guint i = 0; i < 3; i++)
    {
      g_autoptr(GError) tmp_error = NULL;
      if (!ostree_repo_pull (td->repo, "origin", (char**)&ok_refs, 0, NULL, NULL, error))
        goto out;
      if (ostree_repo_pull (td->repo, "origin", (char**)&bad_refs, 0, NULL, NULL, &tmp_error))
        g_assert_not_reached ();
      g_clear_error (&tmp_error);
      if (ostree_repo_pull (td->repo, "origin", (char**)&bad_refs, 0, NULL, NULL, &tmp_error))
        g_assert_not_reached ();
      g_clear_error (&tmp_error);
      if (!ostree_repo_pull (td->repo, "origin", (char**)&ok_refs, 0, NULL, NULL, error))
        goto out;
    }
  
 out:
  g_assert_no_error (local_error);
}

int main (int argc, char **argv)
{
  TestData td = {NULL,};
  int r;

  test_data_init (&td);

  g_test_init (&argc, &argv, NULL);

  g_test_add_data_func ("/test-pull-c/multi-nochange", &td, test_pull_multi_nochange);
  g_test_add_data_func ("/test-pull-c/multi-ok-error-repeat", &td, test_pull_multi_error_then_ok);

  r = g_test_run();
  g_clear_object (&td.repo);

  return r;
}
