/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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

#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

#include "libglnx.h"
#include "libostreetest.h"

static void
test_repo_is_not_system (gconstpointer data)
{
  OstreeRepo *repo = (void*)data;
  g_assert (!ostree_repo_is_system (repo));
}

static GBytes *
input_stream_to_bytes (GInputStream *input)
{
  g_autoptr(GOutputStream) mem_out_stream = NULL;
  g_autoptr(GError) error = NULL;

  if (input == NULL)
    return g_bytes_new (NULL, 0);

  mem_out_stream = g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  g_output_stream_splice (mem_out_stream,
                          input,
                          G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                          NULL,
                          &error);
  g_assert_no_error (error);

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem_out_stream));
}

static void
test_raw_file_to_archive_z2_stream (gconstpointer data)
{
  OstreeRepo *repo = OSTREE_REPO (data);
  g_autofree gchar *commit_checksum = NULL;
  g_autoptr(GHashTable) reachable = NULL;
  g_autoptr(GError) error = NULL;
  /* branch name of the test repository, see setup_test_repository in libtest.sh */
  const gchar *rev = "test2";
  GHashTableIter iter;
  GVariant *serialized_object;
  guint checks = 0;

  ostree_repo_resolve_rev (repo,
                           rev,
                           FALSE,
                           &commit_checksum,
                           &error);
  g_assert_no_error (error);
  ostree_repo_traverse_commit (repo,
                               commit_checksum,
                               -1,
                               &reachable,
                               NULL,
                               &error);
  g_assert_no_error (error);
  g_hash_table_iter_init (&iter, reachable);
  while (g_hash_table_iter_next (&iter, (gpointer*)&serialized_object, NULL))
    {
      const gchar *object_checksum;
      OstreeObjectType object_type;
      g_autoptr(GInputStream) input = NULL;
      g_autoptr(GFileInfo) info = NULL;
      g_autoptr(GVariant) xattrs = NULL;
      g_autoptr(GBytes) input_bytes = NULL;
      g_autoptr(GInputStream) mem_input = NULL;
      g_autoptr(GInputStream) zlib_stream = NULL;
      g_autoptr(GBytes) zlib_bytes = NULL;
      g_autoptr(GInputStream) mem_zlib = NULL;
      g_autoptr(GInputStream) input2 = NULL;
      g_autoptr(GFileInfo) info2 = NULL;
      g_autoptr(GVariant) xattrs2 = NULL;
      g_autoptr(GBytes) input2_bytes = NULL;

      ostree_object_name_deserialize (serialized_object, &object_checksum, &object_type);
      if (object_type != OSTREE_OBJECT_TYPE_FILE)
        continue;

      ostree_repo_load_file (repo,
                             object_checksum,
                             &input,
                             &info,
                             &xattrs,
                             NULL,
                             &error);
      g_assert_no_error (error);

      input_bytes = input_stream_to_bytes (input);
      /* This is to simulate NULL input received from
       * ostree_repo_load_file. Instead of creating the mem_input
       * variable, I could also rewind the input stream and pass it to
       * the function below, but this would assume that the input
       * stream implements either the GSeekable or
       * GFileDescriptorBased interface. */
      if (input != NULL)
        mem_input = g_memory_input_stream_new_from_bytes (input_bytes);
      ostree_raw_file_to_archive_z2_stream (mem_input,
                                            info,
                                            xattrs,
                                            &zlib_stream,
                                            NULL,
                                            &error);
      g_assert_no_error (error);

      zlib_bytes = input_stream_to_bytes (zlib_stream);
      mem_zlib = g_memory_input_stream_new_from_bytes (zlib_bytes);
      ostree_content_stream_parse (FALSE,
                                   mem_zlib,
                                   g_bytes_get_size (zlib_bytes),
                                   FALSE,
                                   &input2,
                                   &info2,
                                   &xattrs2,
                                   NULL,
                                   &error);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_clear_error (&error);

      g_seekable_seek (G_SEEKABLE (mem_zlib),
                       0,
                       G_SEEK_SET,
                       NULL,
                       &error);
      g_assert_no_error (error);

      ostree_content_stream_parse (TRUE,
                                   mem_zlib,
                                   g_bytes_get_size (zlib_bytes),
                                   FALSE,
                                   &input2,
                                   &info2,
                                   &xattrs2,
                                   NULL,
                                   &error);
      g_assert_no_error (error);

      input2_bytes = input_stream_to_bytes (input2);
      g_assert_true (g_bytes_equal (input_bytes, input2_bytes));
      g_assert_true (g_variant_equal (xattrs, xattrs2));
      /* TODO: Not sure how to compare fileinfos */
      ++checks;
    }
  /* to make sure we really tested the function */
  g_assert_cmpint (checks, >, 0);
}

static gboolean hi_content_stream_new (GInputStream **out_stream,
                                       guint64       *out_length,
                                       GError **error)
{
  static const char hi[] = "hi";
  g_autoptr(GMemoryInputStream) hi_memstream = (GMemoryInputStream*)g_memory_input_stream_new_from_data (hi, sizeof(hi)-1, NULL);
  g_autoptr(GFileInfo) finfo = g_file_info_new ();
  g_file_info_set_attribute_uint32 (finfo, "standard::type", G_FILE_TYPE_REGULAR);
  g_file_info_set_attribute_boolean (finfo, "standard::is-symlink", FALSE);
  g_file_info_set_attribute_uint32 (finfo, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (finfo, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (finfo, "unix::mode", S_IFREG|0644);
  return ostree_raw_file_to_content_stream ((GInputStream*)hi_memstream, finfo, NULL, out_stream, out_length, NULL, error);
}

static void
test_validate_remotename (void)
{
  const char *valid[] = {"foo", "hello-world"};
  const char *invalid[] = {"foo/bar", ""};
  for (guint i = 0; i < G_N_ELEMENTS(valid); i++)
    {
      g_autoptr(GError) error = NULL;
      g_assert (ostree_validate_remote_name (valid[i], &error));
      g_assert_no_error (error);
    }
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++)
    {
      g_autoptr(GError) error = NULL;
      g_assert (!ostree_validate_remote_name (invalid[i], &error));
      g_assert (error != NULL);
    }
}

static void
test_object_writes (gconstpointer data)
{
  OstreeRepo *repo = OSTREE_REPO (data);
  g_autoptr(GError) error = NULL;

  static const char hi_sha256[] = "2301b5923720c3edc1f0467addb5c287fd5559e3e0cd1396e7f1edb6b01be9f0";

  /* Successful content write */
  { g_autoptr(GInputStream) hi_memstream = NULL;
    guint64 len;
    hi_content_stream_new (&hi_memstream, &len, &error);
    g_assert_no_error (error);
    g_autofree guchar *csum = NULL;
    (void)ostree_repo_write_content (repo, hi_sha256, hi_memstream, len, &csum,
                                     NULL, &error);
    g_assert_no_error (error);
  }

  /* Invalid content write */
  { g_autoptr(GInputStream) hi_memstream = NULL;
    guint64 len;
    hi_content_stream_new (&hi_memstream, &len, &error);
    g_assert_no_error (error);
    g_autofree guchar *csum = NULL;
    static const char invalid_hi_sha256[] = "cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe";
    g_assert (!ostree_repo_write_content (repo, invalid_hi_sha256, hi_memstream, len, &csum,
                                          NULL, &error));
    g_assert (error);
    g_assert (strstr (error->message, "Corrupted file object"));
  }
}

int main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;

  g_test_init (&argc, &argv, NULL);

  repo = ot_test_setup_repo (NULL, &error); 
  if (!repo)
    goto out;

  g_test_add_data_func ("/repo-not-system", repo, test_repo_is_not_system);
  g_test_add_data_func ("/raw-file-to-archive-z2-stream", repo, test_raw_file_to_archive_z2_stream);
  g_test_add_data_func ("/objectwrites", repo, test_object_writes);
  g_test_add_func ("/remotename", test_validate_remotename);

  return g_test_run();
 out:
  if (error)
    g_error ("%s", error->message);
  return 1;
}
