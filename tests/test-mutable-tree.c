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
#include "libglnx.h"
#include "ostree-mutable-tree.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ot-unix-utils.h"

static void
test_metadata_checksum (void)
{
  const char *checksum = "12345678901234567890123456789012";
  glnx_unref_object OstreeMutableTree *tree = ostree_mutable_tree_new ();

  g_assert_null (ostree_mutable_tree_get_metadata_checksum (tree));

  ostree_mutable_tree_set_metadata_checksum (tree, checksum);

  g_assert_cmpstr (checksum, ==, ostree_mutable_tree_get_metadata_checksum (tree));
}

static void
test_mutable_tree_walk (void)
{
  glnx_unref_object OstreeMutableTree *tree = ostree_mutable_tree_new ();
  glnx_unref_object OstreeMutableTree *parent = NULL;
  g_autoptr(GPtrArray) split_path = NULL;
  GError *error = NULL;
  const char *pathname = "a/b/c/d/e/f/g/i";
  const char *checksum = "01234567890123456789012345678901";

  g_assert (ot_util_path_split_validate (pathname, &split_path, &error));

  g_assert (ostree_mutable_tree_ensure_parent_dirs (tree, split_path,
                                                    checksum, &parent,
                                                    &error));
  {
    glnx_unref_object OstreeMutableTree *subdir = NULL;
    g_assert (ostree_mutable_tree_walk (tree, split_path, 0, &subdir, &error));
    g_assert_nonnull (subdir);
  }

  {
    glnx_unref_object OstreeMutableTree *subdir = NULL;
    g_assert_false (ostree_mutable_tree_walk (tree, split_path, 1, &subdir, &error));
    g_assert_null (subdir);
    g_clear_error (&error);
  }

  {
    glnx_unref_object OstreeMutableTree *subdir = NULL;
    glnx_unref_object OstreeMutableTree *a = NULL;
    g_autofree char *source_checksum = NULL;
    ostree_mutable_tree_lookup (tree, "a", &source_checksum, &a, &error);
    g_assert (ostree_mutable_tree_walk (a, split_path, 1, &subdir, &error));
    g_assert (subdir);
  }
}

static void
test_ensure_parent_dirs (void)
{
  glnx_unref_object OstreeMutableTree *tree = ostree_mutable_tree_new ();
  glnx_unref_object OstreeMutableTree *parent = NULL;
  g_autoptr(GPtrArray) split_path = NULL;
  g_autoptr(GError) error = NULL;
  const char *pathname = "/foo/bar/baz";
  const char *checksum = "01234567890123456789012345678901";
  g_autofree char *source_checksum = NULL;
  glnx_unref_object OstreeMutableTree *source_subdir = NULL;
  g_autofree char *source_checksum2 = NULL;
  glnx_unref_object OstreeMutableTree *source_subdir2 = NULL;

  g_assert (ot_util_path_split_validate (pathname, &split_path, &error));

  g_assert (ostree_mutable_tree_ensure_parent_dirs (tree, split_path,
                                                    checksum, &parent,
                                                    &error));

  g_assert (ostree_mutable_tree_lookup (tree, "foo", &source_checksum,
                                        &source_subdir, &error));

  g_assert_false (ostree_mutable_tree_lookup (tree, "bar", &source_checksum2,
                                              &source_subdir2, &error));
  g_clear_error (&error);
}

static void
test_ensure_dir (void)
{
  glnx_unref_object OstreeMutableTree *tree = ostree_mutable_tree_new ();
  glnx_unref_object OstreeMutableTree *parent = NULL;
  g_autoptr(GError) error = NULL;
  const char *dirname = "foo";
  const char *filename = "bar";
  const char *checksum = "01234567890123456789012345678901";
  g_autofree char *source_checksum = NULL;
  glnx_unref_object OstreeMutableTree *source_subdir = NULL;

  g_assert (ostree_mutable_tree_ensure_dir (tree, dirname, &parent, &error));
  g_assert (ostree_mutable_tree_lookup (tree, dirname, &source_checksum, &source_subdir, &error));

  g_assert (ostree_mutable_tree_replace_file (tree, filename, checksum, &error));
  g_assert_false (ostree_mutable_tree_ensure_dir (tree, filename, &parent, &error));
  g_clear_error (&error);
}

static void
test_replace_file (void)
{
  glnx_unref_object OstreeMutableTree *tree = ostree_mutable_tree_new ();
  g_autoptr(GError) error = NULL;
  const char *filename = "bar";
  const char *checksum = "01234567890123456789012345678901";
  const char *checksum2 = "ABCDEF01234567890123456789012345";

  g_assert (ostree_mutable_tree_replace_file (tree, filename, checksum, &error));
  {
    g_autofree char *out_checksum = NULL;
    glnx_unref_object OstreeMutableTree *subdir = NULL;
    g_assert (ostree_mutable_tree_lookup (tree, filename, &out_checksum, &subdir, &error));
    g_assert_cmpstr (checksum, ==, out_checksum);
  }

  g_assert (ostree_mutable_tree_replace_file (tree, filename, checksum2, &error));
  {
    g_autofree char *out_checksum = NULL;
    glnx_unref_object OstreeMutableTree *subdir = NULL;
    g_assert (ostree_mutable_tree_lookup (tree, filename, &out_checksum, &subdir, &error));
    g_assert_cmpstr (checksum2, ==, out_checksum);
  }
}

static void
test_contents_checksum (void)
{
  const char *checksum = "01234567890123456789012345678901";
  const char *subdir_checksum = "ABCD0123456789012345678901234567";
  glnx_unref_object OstreeMutableTree *tree = ostree_mutable_tree_new ();
  glnx_unref_object OstreeMutableTree *subdir = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_null (ostree_mutable_tree_get_contents_checksum (tree));

  ostree_mutable_tree_set_contents_checksum (tree, checksum);
  g_assert_cmpstr (checksum, ==, ostree_mutable_tree_get_contents_checksum (tree));

  g_assert (ostree_mutable_tree_ensure_dir (tree, "subdir", &subdir, &error));
  g_assert_nonnull (subdir);

  ostree_mutable_tree_set_contents_checksum (subdir, subdir_checksum);
  g_assert_cmpstr (subdir_checksum, ==, ostree_mutable_tree_get_contents_checksum (subdir));
  g_assert_null (ostree_mutable_tree_get_contents_checksum (tree));
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mutable-tree/metadata-checksum", test_metadata_checksum);
  g_test_add_func ("/mutable-tree/contents-checksum", test_contents_checksum);
  g_test_add_func ("/mutable-tree/parent-dirs", test_ensure_parent_dirs);
  g_test_add_func ("/mutable-tree/walk", test_mutable_tree_walk);
  g_test_add_func ("/mutable-tree/ensure-dir", test_ensure_dir);
  g_test_add_func ("/mutable-tree/replace-file", test_replace_file);
  return g_test_run();
}
