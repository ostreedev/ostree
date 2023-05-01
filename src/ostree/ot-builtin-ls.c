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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree-repo-file.h"
#include "ostree.h"
#include "ot-builtins.h"
#include "ot-main.h"
#include "otutil.h"

static gboolean opt_dironly;
static gboolean opt_recursive;
static gboolean opt_checksum;
static gboolean opt_xattrs;
static gboolean opt_nul_filenames_only;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-ls.xml) when changing the option list.
 */

static GOptionEntry options[]
    = { { "dironly", 'd', 0, G_OPTION_ARG_NONE, &opt_dironly,
          "Do not recurse into directory arguments", NULL },
        { "recursive", 'R', 0, G_OPTION_ARG_NONE, &opt_recursive, "Print directories recursively",
          NULL },
        { "checksum", 'C', 0, G_OPTION_ARG_NONE, &opt_checksum, "Print checksum", NULL },
        { "xattrs", 'X', 0, G_OPTION_ARG_NONE, &opt_xattrs, "Print extended attributes", NULL },
        { "nul-filenames-only", 0, 0, G_OPTION_ARG_NONE, &opt_nul_filenames_only,
          "Print only filenames, NUL separated", NULL },
        { NULL } };

static gboolean
print_one_file_text (GFile *f, GFileInfo *file_info, GCancellable *cancellable, GError **error)
{
  g_autoptr (GString) buf = g_string_new ("");
  char type_c;
  guint32 mode;
  guint32 type;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile *)f, error))
    return FALSE;

  type_c = '?';
  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  type = g_file_info_get_file_type (file_info);
  switch (type)
    {
    case G_FILE_TYPE_REGULAR:
      type_c = '-';
      break;
    case G_FILE_TYPE_DIRECTORY:
      type_c = 'd';
      break;
    case G_FILE_TYPE_SYMBOLIC_LINK:
      type_c = 'l';
      break;
    case G_FILE_TYPE_SPECIAL:
      if (S_ISCHR (mode))
        type_c = 'c';
      else if (S_ISBLK (mode))
        type_c = 'b';
      break;
    case G_FILE_TYPE_UNKNOWN:
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE:
      return glnx_throw (error, "Invalid file type");
    }
  g_string_append_c (buf, type_c);
  g_string_append_printf (buf, "0%04o %u %u %6" G_GUINT64_FORMAT " ", mode & ~S_IFMT,
                          g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                          g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                          g_file_info_get_attribute_uint64 (file_info, "standard::size"));

  if (opt_checksum)
    {
      if (type == G_FILE_TYPE_DIRECTORY)
        g_string_append_printf (buf, "%s ",
                                ostree_repo_file_tree_get_contents_checksum ((OstreeRepoFile *)f));
      g_string_append_printf (buf, "%s ", ostree_repo_file_get_checksum ((OstreeRepoFile *)f));
    }

  if (opt_xattrs)
    {
      GVariant *xattrs;
      char *formatted;

      if (!ostree_repo_file_get_xattrs ((OstreeRepoFile *)f, &xattrs, cancellable, error))
        return FALSE;

      formatted = g_variant_print (xattrs, TRUE);
      g_string_append (buf, "{ ");
      g_string_append (buf, formatted);
      g_string_append (buf, " } ");
      g_free (formatted);
      g_variant_unref (xattrs);
    }

  g_string_append (buf, gs_file_get_path_cached (f));

  if (type == G_FILE_TYPE_SYMBOLIC_LINK)
    g_string_append_printf (
        buf, " -> %s",
        g_file_info_get_attribute_byte_string (file_info, "standard::symlink-target"));

  g_print ("%s\n", buf->str);

  return TRUE;
}

static gboolean
print_one_file_binary (GFile *f, GFileInfo *file_info, GCancellable *cancellable, GError **error)
{
  const char *path;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile *)f, error))
    return FALSE;

  path = gs_file_get_path_cached (f);

  fwrite (path, 1, strlen (path), stdout);
  fwrite ("\0", 1, 1, stdout);

  return TRUE;
}

static gboolean
print_one_file (GFile *f, GFileInfo *file_info, GCancellable *cancellable, GError **error)
{
  if (opt_nul_filenames_only)
    return print_one_file_binary (f, file_info, cancellable, error);
  else
    return print_one_file_text (f, file_info, cancellable, error);
}

static gboolean
print_directory_recurse (GFile *f, int depth, GCancellable *cancellable, GError **error)
{
  g_autoptr (GFileEnumerator) dir_enum = NULL;
  g_autoptr (GFile) child = NULL;
  g_autoptr (GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  if (depth > 0)
    depth--;
  else if (depth == 0)
    return TRUE;
  else
    g_assert (depth == -1);

  dir_enum = g_file_enumerate_children (f, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (dir_enum == NULL)
    return FALSE;

  while ((child_info = g_file_enumerator_next_file (dir_enum, NULL, &temp_error)) != NULL)
    {
      g_clear_object (&child);
      child = g_file_get_child (f, g_file_info_get_name (child_info));

      if (!print_one_file (child, child_info, cancellable, error))
        return FALSE;

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!print_directory_recurse (child, depth, cancellable, error))
            return FALSE;
        }

      g_clear_object (&child_info);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  return TRUE;
}

static gboolean
print_one_argument (OstreeRepo *repo, GFile *root, const char *arg, GCancellable *cancellable,
                    GError **error)
{
  g_assert (root != NULL);
  g_assert (arg != NULL);

  g_autoptr (GFile) f = g_file_resolve_relative_path (root, arg);
  if (f == NULL)
    return glnx_throw (error, "Failed to resolve path '%s'", arg);

  g_autoptr (GFileInfo) file_info = NULL;
  file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (file_info == NULL)
    return FALSE;

  if (!print_one_file (f, file_info, cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      if (opt_recursive)
        {
          if (!print_directory_recurse (f, -1, cancellable, error))
            return FALSE;
        }
      else if (!opt_dironly)
        {
          if (!print_directory_recurse (f, 1, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
ostree_builtin_ls (int argc, char **argv, OstreeCommandInvocation *invocation,
                   GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeRepo) repo = NULL;
  const char *rev;
  int i;
  g_autoptr (GFile) root = NULL;

  context = g_option_context_new ("COMMIT [PATH...]");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
    return FALSE;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "An COMMIT argument is required", error);
      return FALSE;
    }
  rev = argv[1];

  if (!ostree_repo_read_commit (repo, rev, &root, NULL, cancellable, error))
    return FALSE;

  if (argc > 2)
    {
      for (i = 2; i < argc; i++)
        {
          if (!print_one_argument (repo, root, argv[i], cancellable, error))
            return glnx_prefix_error (error, "Inspecting path '%s'", argv[i]);
        }
    }
  else
    {
      if (!print_one_argument (repo, root, "/", cancellable, error))
        return FALSE;
    }

  return TRUE;
}
