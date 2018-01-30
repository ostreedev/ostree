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
#include "otutil.h"

static gboolean opt_stats;
static gboolean opt_fs_diff;
static gboolean opt_no_xattrs;
static gint opt_owner_uid = -1;
static gint opt_owner_gid = -1;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-diff.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "stats", 0, 0, G_OPTION_ARG_NONE, &opt_stats, "Print various statistics", NULL },
  { "fs-diff", 0, 0, G_OPTION_ARG_NONE, &opt_fs_diff, "Print filesystem diff", NULL },
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Skip output of extended attributes", NULL },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &opt_owner_uid, "Use file ownership user id for local files", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &opt_owner_gid, "Use file ownership group id for local files", "GID" },
  { NULL }
};

static gboolean
parse_file_or_commit (OstreeRepo  *repo,
                      const char  *arg,
                      GFile      **out_file,
                      GCancellable *cancellable,
                      GError     **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) ret_file = NULL;

  if (g_str_has_prefix (arg, "/")
      || g_str_has_prefix (arg, "./")
      )
    {
      ret_file = g_file_new_for_path (arg);
    }
  else
    {
      if (!ostree_repo_read_commit (repo, arg, &ret_file, NULL, cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

static GHashTable *
reachable_set_intersect (GHashTable *a, GHashTable *b)
{
  GHashTable *ret = ostree_repo_traverse_new_reachable ();
  GHashTableIter hashiter;
  gpointer key, value;

  g_hash_table_iter_init (&hashiter, a);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *v = key;
      if (g_hash_table_contains (b, v))
        g_hash_table_insert (ret, g_variant_ref (v), v);
    }

  return ret;
}

static gboolean
object_set_total_size (OstreeRepo    *repo,
                       GHashTable    *reachable,
                       guint64       *out_total,
                       GCancellable  *cancellable,
                       GError       **error)
{
  gboolean ret = FALSE;
  GHashTableIter hashiter;
  gpointer key, value;

  *out_total = 0;

  g_hash_table_iter_init (&hashiter, reachable);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *v = key;
      const char *csum;
      OstreeObjectType objtype;
      guint64 size;

      ostree_object_name_deserialize (v, &csum, &objtype);
      if (!ostree_repo_query_object_storage_size (repo, objtype, csum, &size,
                                                  cancellable, error))
        goto out;
      *out_total += size;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_diff (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *src;
  const char *target;
  g_autofree char *src_prev = NULL;
  g_autoptr(GFile) srcf = NULL;
  g_autoptr(GFile) targetf = NULL;
  g_autoptr(GPtrArray) modified = NULL;
  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;

  context = g_option_context_new ("REV TARGETDIR");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "REV must be specified");
      goto out;
    }

  if (argc == 2)
    {
      src_prev = g_strconcat (argv[1], "^", NULL);
      src = src_prev;
      target = argv[1];
    }
  else
    {
      src = argv[1];
      target = argv[2];
    }

  if (!opt_stats && !opt_fs_diff)
    opt_fs_diff = TRUE;

  if (opt_fs_diff)
    {
      OstreeDiffFlags diff_flags = OSTREE_DIFF_FLAGS_NONE; 

      if (opt_no_xattrs)
        diff_flags |= OSTREE_DIFF_FLAGS_IGNORE_XATTRS;
      
      if (!parse_file_or_commit (repo, src, &srcf, cancellable, error))
        goto out;
      if (!parse_file_or_commit (repo, target, &targetf, cancellable, error))
        goto out;

      modified = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_diff_item_unref);
      removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
      added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

      OstreeDiffDirsOptions diff_opts = { opt_owner_uid, opt_owner_gid };
      if (!ostree_diff_dirs_with_options (diff_flags, srcf, targetf, modified, removed,
                                          added, &diff_opts, cancellable, error))
        goto out;

      ostree_diff_print (srcf, targetf, modified, removed, added);
    }

  if (opt_stats)
    {
      g_autoptr(GHashTable) reachable_a = NULL;
      g_autoptr(GHashTable) reachable_b = NULL;
      g_autoptr(GHashTable) reachable_intersection = NULL;
      g_autofree char *rev_a = NULL;
      g_autofree char *rev_b = NULL;
      g_autofree char *size = NULL;
      guint a_size;
      guint b_size;
      guint64 total_common;

      if (!ostree_repo_resolve_rev (repo, src, FALSE, &rev_a, error))
        goto out;
      if (!ostree_repo_resolve_rev (repo, target, FALSE, &rev_b, error))
        goto out;

      if (!ostree_repo_traverse_commit (repo, rev_a, 0, &reachable_a, cancellable, error))
        goto out;
      if (!ostree_repo_traverse_commit (repo, rev_b, 0, &reachable_b, cancellable, error))
        goto out;

      a_size = g_hash_table_size (reachable_a);
      b_size = g_hash_table_size (reachable_b);
      g_print ("[A] Object Count: %u\n", a_size);
      g_print ("[B] Object Count: %u\n", b_size);

      reachable_intersection = reachable_set_intersect (reachable_a, reachable_b);

      g_print ("Common Object Count: %u\n", g_hash_table_size (reachable_intersection));

      if (!object_set_total_size (repo, reachable_intersection, &total_common,
                                  cancellable, error))
        goto out;
      size = g_format_size_full (total_common, 0);
      g_print ("Common Object Size: %s\n", size);
    }

  ret = TRUE;
 out:
  return ret;
}
