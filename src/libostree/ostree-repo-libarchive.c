/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file.h"
#include "ostree-mutable-tree.h"

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include "ostree-libarchive-input-stream.h"
#endif

#include "otutil.h"

#ifdef HAVE_LIBARCHIVE

#define DEFAULT_DIRMODE (0755 | S_IFDIR)

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static const char *
path_relative (const char *src,
               GError    **error)
{
  /* One issue here is that some archives almost record the pathname as just a
   * string and don't need to actually encode parent/child relationships in the
   * archive. For us however, this will be important. So we do our best to deal
   * with non-conventional paths. We also validate the path at the end to make
   * sure there are no illegal components. Also important, we relativize the
   * path. */

  /* relativize first (and make /../../ --> /) */
  while (src[0] == '/')
    {
      src += 1;
      if (src[0] == '.' && src[1] == '.' && src[2] == '/')
        src += 2; /* keep trailing / so we continue */
    }

  /* now let's skip . and empty components */
  while (TRUE)
    {
      if (src[0] == '.' && src[1] == '/')
        src += 2;
      else if (src[0] == '/')
        src += 1;
      else
        break;
    }

  /* assume a single '.' means the root dir itself, which we handle as the empty
   * string in our code */
  if (src[0] == '.' && src[1] == '\0')
    src += 1;

  /* make sure that the final path is valid (no . or ..) */
  if (!ot_util_path_split_validate (src, NULL, error))
    {
      g_prefix_error (error, "While making relative path \"%s\":", src);
      return NULL;
    }

  return src;
}

static char *
path_relative_ostree (const char *path,
                      GError    **error)
{
  path = path_relative (path, error);
  if (path == NULL)
    return NULL;
  if (g_str_has_prefix (path, "etc/"))
    return g_strconcat ("usr/", path, NULL);
  else if (strcmp (path, "etc") == 0)
    return g_strdup ("usr/etc");
  return g_strdup (path);
}

static void
append_path_component (char       **path_builder,
                       const char  *component)
{
  g_autofree char *s = g_steal_pointer (path_builder);
  *path_builder = g_build_filename (s ?: "/", component, NULL);
}

/* inplace trailing slash squashing  */
static void
squash_trailing_slashes (char *path)
{
  char *endp = path + strlen (path) - 1;
  for (; endp > path && *endp == '/'; endp--)
    *endp = '\0';
}

static GFileInfo *
file_info_from_archive_entry (struct archive_entry *entry)
{
  g_autoptr(GFileInfo) info = NULL;
  const struct stat *st = NULL;
  guint32 file_type;
  mode_t mode;

  st = archive_entry_stat (entry);
  mode = st->st_mode;

  /* Some archives only store the permission mode bits in hardlink entries, so
   * let's just make it into a regular file. Yes, this hack will work even if
   * it's a hardlink to a symlink. */
  if (archive_entry_hardlink (entry))
    mode |= S_IFREG;

  info = _ostree_header_gfile_info_new (mode, st->st_uid, st->st_gid);

  file_type = ot_gfile_type_for_mode (mode);
  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", st->st_size);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_file_info_set_attribute_byte_string (info, "standard::symlink-target",
                                             archive_entry_symlink (entry));
    }

  return g_steal_pointer (&info);
}

static gboolean
builder_add_label (GVariantBuilder  *builder,
                   OstreeSePolicy   *sepolicy,
                   const char       *path,
                   mode_t            mode,
                   GCancellable     *cancellable,
                   GError          **error)
{
  g_autofree char *label = NULL;

  if (!sepolicy)
    return TRUE;

  if (!ostree_sepolicy_get_label (sepolicy, path, mode, &label,
                                  cancellable, error))
    return FALSE;

  if (label)
    g_variant_builder_add (builder, "(@ay@ay)",
                           g_variant_new_bytestring ("security.selinux"),
                           g_variant_new_bytestring (label));
  return TRUE;
}


/* Like ostree_mutable_tree_ensure_dir(), but also creates and sets dirmeta if
 * the dir has to be created. */
static gboolean
mtree_ensure_dir_with_meta (OstreeRepo          *repo,
                            OstreeMutableTree   *parent,
                            const char          *name,
                            GFileInfo           *file_info,
                            GVariant            *xattrs,
                            gboolean             error_if_exist, /* XXX: remove if not needed */
                            OstreeMutableTree  **out_dir,
                            GCancellable        *cancellable,
                            GError             **error)
{
  glnx_unref_object OstreeMutableTree *dir = NULL;
  g_autofree guchar *csum_raw = NULL;
  g_autofree char *csum = NULL;

  if (name[0] == '\0') /* root? */
    dir = g_object_ref (parent);
  else if (ostree_mutable_tree_lookup (parent, name, NULL, &dir, error))
    {
      if (error_if_exist)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Directory \"%s\" already exists", name);
          return FALSE;
        }
    }

  if (dir == NULL)
    {
      if (!g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return FALSE;

      g_clear_error (error);

      if (!ostree_mutable_tree_ensure_dir (parent, name, &dir, error))
        return FALSE;
    }

  if (!_ostree_repo_write_directory_meta (repo, file_info, xattrs,
                                          &csum_raw, cancellable, error))
    return FALSE;

  csum = ostree_checksum_from_bytes (csum_raw);

  ostree_mutable_tree_set_metadata_checksum (dir, csum);

  if (out_dir)
    *out_dir = g_steal_pointer (&dir);

  return TRUE;
}

typedef struct {
  OstreeRepo                     *repo;
  OstreeRepoImportArchiveOptions *opts;
  OstreeMutableTree              *root;
  struct archive                 *archive;
  struct archive_entry           *entry;
  GHashTable                     *deferred_hardlinks;
  OstreeRepoCommitModifier       *modifier;
} OstreeRepoArchiveImportContext;

typedef struct {
  OstreeMutableTree  *parent;
  char               *path;
  guint64             size;
} DeferredHardlink;

static inline char*
aic_get_final_path (OstreeRepoArchiveImportContext *ctx,
                    const char  *path,
                    GError     **error)
{
  if (ctx->opts->use_ostree_convention)
    return path_relative_ostree (path, error);
  return g_strdup (path_relative (path, error));
}

static inline char*
aic_get_final_entry_pathname (OstreeRepoArchiveImportContext *ctx,
                              GError  **error)
{
  const char *pathname = archive_entry_pathname (ctx->entry);
  g_autofree char *final = aic_get_final_path (ctx, pathname, error);

  if (final == NULL)
    return NULL;

  /* get rid of trailing slashes some archives put on dirs */
  squash_trailing_slashes (final);
  return g_steal_pointer (&final);
}

static inline char*
aic_get_final_entry_hardlink (OstreeRepoArchiveImportContext *ctx)
{
  GError *local_error = NULL;
  const char *hardlink = archive_entry_hardlink (ctx->entry);
  g_autofree char *final = NULL;

  if (hardlink != NULL)
    {
      final = aic_get_final_path (ctx, hardlink, &local_error);

      /* hardlinks always point to a preceding entry, so if there were an error
       * it would have failed then */
      g_assert_no_error (local_error);
    }

  return g_steal_pointer (&final);
}

static OstreeRepoCommitFilterResult
aic_apply_modifier_filter (OstreeRepoArchiveImportContext *ctx,
                           const char  *relpath,
                           GFileInfo  **out_file_info)
{
  g_autofree char *hardlink = aic_get_final_entry_hardlink (ctx);
  g_autoptr(GFileInfo) file_info = NULL;
  g_autofree char *abspath = NULL;
  const char *cb_path = NULL;

  if (ctx->opts->callback_with_entry_pathname)
    cb_path = archive_entry_pathname (ctx->entry);
  else
    {
      /* the user expects an abspath (where the dir to commit represents /) */
      abspath = g_build_filename ("/", relpath, NULL);
      cb_path = abspath;
    }

  file_info = file_info_from_archive_entry (ctx->entry);

  return _ostree_repo_commit_modifier_apply (ctx->repo, ctx->modifier, cb_path,
                                             file_info, out_file_info);
}

static gboolean
aic_ensure_parent_dir_with_file_info (OstreeRepoArchiveImportContext *ctx,
                                      OstreeMutableTree   *parent,
                                      const char          *fullpath,
                                      GFileInfo           *file_info,
                                      OstreeMutableTree  **out_dir,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  const char *name = glnx_basename (fullpath);
  g_auto(GVariantBuilder) xattrs_builder;
  g_autoptr(GVariant) xattrs = NULL;

  /* is this the root directory itself? transform into empty string */
  if (name[0] == '/' && name[1] == '\0')
    name++;

  g_variant_builder_init (&xattrs_builder, (GVariantType*)"a(ayay)");

  if (ctx->modifier && ctx->modifier->sepolicy)
    if (!builder_add_label (&xattrs_builder, ctx->modifier->sepolicy, fullpath,
                            DEFAULT_DIRMODE, cancellable, error))
      return FALSE;

  xattrs = g_variant_ref_sink (g_variant_builder_end (&xattrs_builder));
  return mtree_ensure_dir_with_meta (ctx->repo, parent, name, file_info,
                                     xattrs,
                                     FALSE /* error_if_exist */, out_dir,
                                     cancellable, error);
}

static gboolean
aic_ensure_parent_dir (OstreeRepoArchiveImportContext *ctx,
                       OstreeMutableTree   *parent,
                       const char          *fullpath,
                       OstreeMutableTree  **out_dir,
                       GCancellable        *cancellable,
                       GError             **error)
{
  /* Who should own the parent dir? Since it's not in the archive, it's up to
   * us. Here, we use the heuristic of simply creating it as the same user as
   * the owner of the archive entry for which we're creating the dir. This is OK
   * since any nontrivial dir perms should have explicit archive entries. */

  guint32 uid = archive_entry_uid (ctx->entry);
  guint32 gid = archive_entry_gid (ctx->entry);
  glnx_unref_object GFileInfo *file_info = g_file_info_new ();

  g_file_info_set_attribute_uint32 (file_info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", DEFAULT_DIRMODE);

  return aic_ensure_parent_dir_with_file_info (ctx, parent, fullpath, file_info,
                                               out_dir, cancellable, error);
}

static gboolean
aic_create_parent_dirs (OstreeRepoArchiveImportContext *ctx,
                        GPtrArray           *components,
                        OstreeMutableTree  **out_subdir,
                        GCancellable        *cancellable,
                        GError             **error)
{
  g_autofree char *fullpath = NULL;
  glnx_unref_object OstreeMutableTree *dir = NULL;

  /* start with the root itself */
  if (!aic_ensure_parent_dir (ctx, ctx->root, "/", &dir, cancellable, error))
    return FALSE;

  for (guint i = 0; i < components->len-1; i++)
    {
      glnx_unref_object OstreeMutableTree  *subdir = NULL;
      append_path_component (&fullpath, components->pdata[i]);

      if (!aic_ensure_parent_dir (ctx, dir, fullpath, &subdir,
                                  cancellable, error))
        return FALSE;

      g_set_object (&dir, subdir);
    }

  *out_subdir = g_steal_pointer (&dir);
  return TRUE;
}

static gboolean
aic_get_parent_dir (OstreeRepoArchiveImportContext *ctx,
                    const char          *path,
                    OstreeMutableTree  **out_dir,
                    GCancellable        *cancellable,
                    GError             **error)
{
  g_autoptr(GPtrArray) components = NULL;
  if (!ot_util_path_split_validate (path, &components, error))
    return FALSE;

  if (components->len == 0) /* root dir? */
    {
      *out_dir = g_object_ref (ctx->root);
      return TRUE;
    }

  if (ostree_mutable_tree_walk (ctx->root, components, 0, out_dir, error))
    return TRUE; /* already exists, nice! */

  if (!g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return FALSE; /* some other error occurred */

  if (ctx->opts->autocreate_parents)
    {
      g_clear_error (error);
      return aic_create_parent_dirs (ctx, components, out_dir,
                                     cancellable, error);
    }

  return FALSE;
}

static gboolean
aic_get_xattrs (OstreeRepoArchiveImportContext *ctx,
                const char         *path,
                GFileInfo          *file_info,
                GVariant          **out_xattrs,
                GCancellable       *cancellable,
                GError            **error)
{
  g_autofree char *abspath = g_build_filename ("/", path, NULL);
  g_autoptr(GVariant) xattrs = NULL;
  const char *cb_path = abspath;

  if (ctx->opts->callback_with_entry_pathname)
    cb_path = archive_entry_pathname (ctx->entry);

  if (ctx->modifier && ctx->modifier->xattr_callback)
    xattrs = ctx->modifier->xattr_callback (ctx->repo, cb_path, file_info,
                                            ctx->modifier->xattr_user_data);

  if (ctx->modifier && ctx->modifier->sepolicy)
    {
      mode_t mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      g_autoptr(GVariantBuilder) builder =
        ot_util_variant_builder_from_variant (xattrs, G_VARIANT_TYPE
                                                        ("a(ayay)"));

      if (!builder_add_label (builder, ctx->modifier->sepolicy, abspath, mode,
                              cancellable, error))
        return FALSE;

      if (xattrs)
        g_variant_unref (xattrs);

      xattrs = g_variant_builder_end (builder);
      g_variant_ref_sink (xattrs);
    }

  *out_xattrs = g_steal_pointer (&xattrs);
  return TRUE;
}

/* XXX: add option in ctx->opts to disallow already existing dirs? see
 * error_if_exist */
static gboolean
aic_handle_dir (OstreeRepoArchiveImportContext *ctx,
                OstreeMutableTree  *parent,
                const char         *path,
                GFileInfo          *fi,
                GCancellable       *cancellable,
                GError            **error)
{
  const char *name = glnx_basename (path);
  g_autoptr(GVariant) xattrs = NULL;

  if (!aic_get_xattrs (ctx, path, fi, &xattrs, cancellable, error))
    return FALSE;

  return mtree_ensure_dir_with_meta (ctx->repo, parent, name, fi, xattrs,
                                     FALSE /* error_if_exist */, NULL,
                                     cancellable, error);
}

static gboolean
aic_write_file (OstreeRepoArchiveImportContext *ctx,
                GFileInfo          *fi,
                GVariant           *xattrs,
                char              **out_csum,
                GCancellable       *cancellable,
                GError            **error)
{
  g_autoptr(GInputStream) archive_stream = NULL;
  g_autoptr(GInputStream) file_object_input = NULL;
  guint64 length;

  g_autofree guchar *csum_raw = NULL;

  if (g_file_info_get_file_type (fi) == G_FILE_TYPE_REGULAR)
    archive_stream = _ostree_libarchive_input_stream_new (ctx->archive);

  if (!ostree_raw_file_to_content_stream (archive_stream, fi, xattrs,
                                          &file_object_input, &length,
                                          cancellable, error))
    return FALSE;

  if (!ostree_repo_write_content (ctx->repo, NULL, file_object_input, length,
                                  &csum_raw, cancellable, error))
    return FALSE;

  *out_csum = ostree_checksum_from_bytes (csum_raw);
  return TRUE;
}

static gboolean
aic_import_file (OstreeRepoArchiveImportContext *ctx,
                 OstreeMutableTree  *parent,
                 const char         *path,
                 GFileInfo          *fi,
                 GCancellable       *cancellable,
                 GError            **error)
{
  const char *name = glnx_basename (path);
  g_autoptr(GVariant) xattrs = NULL;
  g_autofree char *csum = NULL;

  if (!aic_get_xattrs (ctx, path, fi, &xattrs, cancellable, error))
    return FALSE;

  if (!aic_write_file (ctx, fi, xattrs, &csum, cancellable, error))
    return FALSE;

  if (!ostree_mutable_tree_replace_file (parent, name, csum, error))
    return FALSE;

  return TRUE;
}

static void
aic_add_deferred_hardlink (OstreeRepoArchiveImportContext *ctx,
                           const char        *hardlink,
                           DeferredHardlink  *dh)
{
  gboolean new_slist;
  GSList *slist;

  slist = g_hash_table_lookup (ctx->deferred_hardlinks, hardlink);
  new_slist = (slist == NULL);

  slist = g_slist_append (slist, dh);

  if (new_slist)
    g_hash_table_insert (ctx->deferred_hardlinks, g_strdup (hardlink), slist);
}

static void
aic_defer_hardlink (OstreeRepoArchiveImportContext *ctx,
                    OstreeMutableTree  *parent,
                    const char         *path,
                    guint64             size,
                    const char         *hardlink)
{
  DeferredHardlink *dh = g_slice_new (DeferredHardlink);
  dh->parent = g_object_ref (parent);
  dh->path = g_strdup (path);
  dh->size = size;

  aic_add_deferred_hardlink (ctx, hardlink, dh);
}

static gboolean
aic_handle_file (OstreeRepoArchiveImportContext *ctx,
                 OstreeMutableTree  *parent,
                 const char         *path,
                 GFileInfo          *fi,
                 GCancellable       *cancellable,
                 GError            **error)
{
  /* The wonderful world of hardlinks and archives. We have to be very careful
   * here. Do not assume that if a file is a hardlink, it will have size 0 (e.g.
   * cpio). Do not assume that if a file will have hardlinks to it, it will have
   * size > 0. Also do not assume that its nlink param is present (tar) or even
   * accurate (cpio). Also do not assume that hardlinks follow each other in
   * order of entries.
   *
   * These archives were made to be extracted onto a filesystem, not directly
   * hashed into an object store. So to be careful, we defer all hardlink
   * imports until the very end. Nonzero files have to be imported, hardlink or
   * not, since we can't easily seek back to this position later on.
   * */

  g_autofree char *hardlink = aic_get_final_entry_hardlink (ctx);
  guint64 size = g_file_info_get_attribute_uint64 (fi, "standard::size");

  if (hardlink == NULL || size > 0)
    if (!aic_import_file (ctx, parent, path, fi, cancellable, error))
      return FALSE;

  if (hardlink)
    aic_defer_hardlink (ctx, parent, path, size, hardlink);

  return TRUE;
}

static gboolean
aic_handle_entry (OstreeRepoArchiveImportContext *ctx,
                  OstreeMutableTree  *parent,
                  const char         *path,
                  GFileInfo          *fi,
                  GCancellable       *cancellable,
                  GError            **error)
{
  switch (g_file_info_get_file_type (fi))
    {
    case G_FILE_TYPE_DIRECTORY:
      return aic_handle_dir (ctx, parent, path, fi, cancellable, error);
    case G_FILE_TYPE_REGULAR:
    case G_FILE_TYPE_SYMBOLIC_LINK:
      return aic_handle_file (ctx, parent, path, fi, cancellable, error);
    default:
      if (ctx->opts->ignore_unsupported_content)
        return TRUE;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file type for path \"%s\"", path);
          return FALSE;
        }
    }
}

static gboolean
aic_import_entry (OstreeRepoArchiveImportContext *ctx,
                  GCancellable  *cancellable,
                  GError       **error)
{
  g_autoptr(GFileInfo) fi = NULL;
  glnx_unref_object OstreeMutableTree *parent = NULL;
  g_autofree char *path = aic_get_final_entry_pathname (ctx, error);

  if (path == NULL)
    return FALSE;

  if (aic_apply_modifier_filter (ctx, path, &fi)
        == OSTREE_REPO_COMMIT_FILTER_SKIP)
    return TRUE;

  if (!aic_get_parent_dir (ctx, path, &parent, cancellable, error))
    return FALSE;

  return aic_handle_entry (ctx, parent, path, fi, cancellable, error);
}

static gboolean
aic_import_from_hardlink (OstreeRepoArchiveImportContext *ctx,
                          const char        *target,
                          DeferredHardlink  *dh,
                          GError           **error)
{
  g_autofree char *csum = NULL;
  const char *name = glnx_basename (target);
  const char *name_dh = glnx_basename (dh->path);
  g_autoptr(GPtrArray) components = NULL;
  glnx_unref_object OstreeMutableTree *parent = NULL;

  if (!ostree_mutable_tree_lookup (dh->parent, name_dh, &csum, NULL, error))
    return FALSE;

  g_assert (csum);

  if (!ot_util_path_split_validate (target, &components, error))
    return FALSE;

  if (!ostree_mutable_tree_walk (ctx->root, components, 0, &parent, error))
    return FALSE;

  if (!ostree_mutable_tree_replace_file (parent, name, csum, error))
    return FALSE;

  return TRUE;
}

static gboolean
aic_lookup_file_csum (OstreeRepoArchiveImportContext *ctx,
                      const char    *target,
                      char         **out_csum,
                      GError       **error)
{
  g_autofree char *csum = NULL;
  const char *name = glnx_basename (target);
  glnx_unref_object OstreeMutableTree *parent = NULL;
  glnx_unref_object OstreeMutableTree *subdir = NULL;
  g_autoptr(GPtrArray) components = NULL;

  if (!ot_util_path_split_validate (target, &components, error))
    return FALSE;

  if (!ostree_mutable_tree_walk (ctx->root, components, 0, &parent, error))
    return FALSE;

  if (!ostree_mutable_tree_lookup (parent, name, &csum, &subdir, error))
    return FALSE;

  if (subdir != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Expected hardlink file target at \"%s\" but found a "
                   "directory", target);
      return FALSE;
    }

  *out_csum = g_steal_pointer (&csum);
  return TRUE;
}

static gboolean
aic_import_deferred_hardlinks_for (OstreeRepoArchiveImportContext *ctx,
                                   const char    *target,
                                   GSList        *hardlinks,
                                   GError       **error)
{
  GSList *payload = hardlinks;
  g_autofree char *csum = NULL;

  /* find node with the payload, if any (if none, then they're all hardlinks to
   * a zero sized target, and there's no rewrite required) */
  while (payload && ((DeferredHardlink*)payload->data)->size == 0)
    payload = g_slist_next (payload);

  /* rewrite the target so it points to the csum of the payload hardlink */
  if (payload)
    if (!aic_import_from_hardlink (ctx, target, payload->data, error))
      return FALSE;

  if (!aic_lookup_file_csum (ctx, target, &csum, error))
    return FALSE;

  /* import all the hardlinks */
  for (GSList *hl = hardlinks; hl != NULL; hl = g_slist_next (hl))
    {
      DeferredHardlink *df = hl->data;
      const char *name = glnx_basename (df->path);

      if (hl == payload)
        continue; /* small optimization; no need to redo this one */

      if (!ostree_mutable_tree_replace_file (df->parent, name, csum, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
aic_import_deferred_hardlinks (OstreeRepoArchiveImportContext *ctx,
                               GCancellable  *cancellable,
                               GError       **error)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, ctx->deferred_hardlinks);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!aic_import_deferred_hardlinks_for (ctx, key, value, error))
      return FALSE;

  return TRUE;
}

static void
deferred_hardlink_free (void *data)
{
  DeferredHardlink *dh = data;
  g_object_unref (dh->parent);
  g_free (dh->path);
  g_slice_free (DeferredHardlink, dh);
}

static void
deferred_hardlinks_list_free (void *data)
{
  GSList *slist = data;
  g_slist_free_full (slist, deferred_hardlink_free);
}
#endif /* HAVE_LIBARCHIVE */

/**
 * ostree_repo_import_archive_to_mtree:
 * @self: An #OstreeRepo
 * @opts: Options structure, ensure this is zeroed, then set specific variables
 * @archive: Really this is "struct archive*"
 * @mtree: The #OstreeMutableTree to write to
 * @modifier: (allow-none): Optional commit modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Import an archive file @archive into the repository, and write its
 * file structure to @mtree.
 */
gboolean
ostree_repo_import_archive_to_mtree (OstreeRepo                   *self,
                                     OstreeRepoImportArchiveOptions  *opts,
                                     void                         *archive,
                                     OstreeMutableTree            *mtree,
                                     OstreeRepoCommitModifier     *modifier,
                                     GCancellable                 *cancellable,
                                     GError                      **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = archive;
  g_autoptr(GHashTable) deferred_hardlinks =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                           deferred_hardlinks_list_free);

  OstreeRepoArchiveImportContext aictx = {
    .repo = self,
    .opts = opts,
    .root = mtree,
    .archive = archive,
    .deferred_hardlinks = deferred_hardlinks,
    .modifier = modifier
  };

  while (TRUE)
    {
      int r = archive_read_next_header (a, &aictx.entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto out;
        }

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      if (!aic_import_entry (&aictx, cancellable, error))
        goto out;
    }

  if (!aic_import_deferred_hardlinks (&aictx, cancellable, error))
    goto out;

  /* If we didn't import anything at all, and autocreation of parents
   * is enabled, automatically create a root directory.  This is
   * useful primarily when importing Docker image layers, which can
   * just be metadata.
   */
  if (opts->autocreate_parents &&
      ostree_mutable_tree_get_metadata_checksum (mtree) == NULL)
    {
      glnx_unref_object GFileInfo *fi = g_file_info_new ();
      g_file_info_set_attribute_uint32 (fi, "unix::uid", 0);
      g_file_info_set_attribute_uint32 (fi, "unix::gid", 0);
      g_file_info_set_attribute_uint32 (fi, "unix::mode", DEFAULT_DIRMODE);

      if (!aic_ensure_parent_dir_with_file_info (&aictx, mtree, "/", fi, NULL,
                                                 cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}

/**
 * ostree_repo_write_archive_to_mtree:
 * @self: An #OstreeRepo
 * @archive: A path to an archive file
 * @mtree: The #OstreeMutableTree to write to
 * @modifier: (allow-none): Optional commit modifier
 * @autocreate_parents: Autocreate parent directories
 * @cancellable: Cancellable
 * @error: Error
 *
 * Import an archive file @archive into the repository, and write its
 * file structure to @mtree.
 */
gboolean
ostree_repo_write_archive_to_mtree (OstreeRepo                *self,
                                    GFile                     *archive,
                                    OstreeMutableTree         *mtree,
                                    OstreeRepoCommitModifier  *modifier,
                                    gboolean                   autocreate_parents,
                                    GCancellable              *cancellable,
                                    GError                   **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = NULL;
  g_autoptr(GFileInfo) tmp_dir_info = NULL;
  OstreeRepoImportArchiveOptions opts = { 0, };

  a = archive_read_new ();
#ifdef HAVE_ARCHIVE_READ_SUPPORT_FILTER_ALL
  archive_read_support_filter_all (a);
#else
  archive_read_support_compression_all (a);
#endif
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, gs_file_get_path_cached (archive), 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  opts.autocreate_parents = !!autocreate_parents;

  if (!ostree_repo_import_archive_to_mtree (self, &opts, a, mtree, modifier, cancellable, error))
    goto out;

  if (archive_read_close (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  if (a)
    (void)archive_read_close (a);
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}

#ifdef HAVE_LIBARCHIVE

static gboolean
file_to_archive_entry_common (GFile         *root,
                              OstreeRepoExportArchiveOptions *opts,
                              GFile         *path,
                              GFileInfo  *file_info,
                              struct archive_entry *entry,
                              GError            **error)
{
  gboolean ret = FALSE;
  g_autofree char *pathstr = g_file_get_relative_path (root, path);
  g_autoptr(GVariant) xattrs = NULL;
  time_t ts = (time_t) opts->timestamp_secs;

  if (opts->path_prefix && opts->path_prefix[0])
    {
      g_autofree char *old_pathstr = pathstr;
      pathstr = g_strconcat (opts->path_prefix, old_pathstr, NULL);
    }

  if (pathstr == NULL || !pathstr[0])
    {
      g_free (pathstr);
      pathstr = g_strdup (".");
    }

  archive_entry_update_pathname_utf8 (entry, pathstr);
  archive_entry_set_ctime (entry, ts, OSTREE_TIMESTAMP);
  archive_entry_set_mtime (entry, ts, OSTREE_TIMESTAMP);
  archive_entry_set_atime (entry, ts, OSTREE_TIMESTAMP);
  archive_entry_set_uid (entry, g_file_info_get_attribute_uint32 (file_info, "unix::uid"));
  archive_entry_set_gid (entry, g_file_info_get_attribute_uint32 (file_info, "unix::gid"));
  archive_entry_set_mode (entry, g_file_info_get_attribute_uint32 (file_info, "unix::mode"));

  if (!ostree_repo_file_get_xattrs ((OstreeRepoFile*)path, &xattrs, NULL, error))
    goto out;

  if (!opts->disable_xattrs)
    {
      int i, n;
      
      n = g_variant_n_children (xattrs);
      for (i = 0; i < n; i++)
        {
          const guint8* name;
          g_autoptr(GVariant) value = NULL;
          const guint8* value_data;
          gsize value_len;

          g_variant_get_child (xattrs, i, "(^&ay@ay)", &name, &value);
          value_data = g_variant_get_fixed_array (value, &value_len, 1);

          archive_entry_xattr_add_entry (entry, (char*)name,
                                         (char*) value_data, value_len);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_header_free_entry (struct archive *a,
                         struct archive_entry **entryp,
                         GError **error)
{
  struct archive_entry *entry = *entryp;
  gboolean ret = FALSE;

  if (archive_write_header (a, entry) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  archive_entry_free (entry);
  *entryp = NULL;
  return ret;
}

static gboolean
write_directory_to_libarchive_recurse (OstreeRepo               *self,
                                       OstreeRepoExportArchiveOptions *opts,
                                       GFile                    *root,
                                       GFile                    *dir,
                                       struct archive           *a,
                                       GCancellable             *cancellable,
                                       GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileInfo) dir_info = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  struct archive_entry *entry = NULL;

  dir_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!dir_info)
    goto out;

  entry = archive_entry_new2 (a);
  if (!file_to_archive_entry_common (root, opts, dir, dir_info, entry, error))
    goto out;
  if (!write_header_free_entry (a, &entry, error))
    goto out;

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &path,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      /* First, handle directories recursively */
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!write_directory_to_libarchive_recurse (self, opts, root, path, a,
                                                      cancellable, error))
            goto out;

          /* Go to the next entry */
          continue;
        }

      /* Past here, should be a regular file or a symlink */

      entry = archive_entry_new2 (a);
      if (!file_to_archive_entry_common (root, opts, path, file_info, entry, error))
        goto out;

      switch (g_file_info_get_file_type (file_info))
        {
        case G_FILE_TYPE_SYMBOLIC_LINK:
          {
            archive_entry_set_symlink (entry, g_file_info_get_symlink_target (file_info));
            if (!write_header_free_entry (a, &entry, error))
              goto out;
          }
          break;
        case G_FILE_TYPE_REGULAR:
          {
            guint8 buf[8192];
            g_autoptr(GInputStream) file_in = NULL;
            g_autoptr(GFileInfo) file_info = NULL;
            const char *checksum;

            checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)path);

            if (!ostree_repo_load_file (self, checksum, &file_in, &file_info, NULL,
                                        cancellable, error))
              goto out;

            archive_entry_set_size (entry, g_file_info_get_size (file_info));

            if (archive_write_header (a, entry) != ARCHIVE_OK)
              {
                propagate_libarchive_error (error, a);
                goto out;
              }

            while (TRUE)
              {
                gssize bytes_read = g_input_stream_read (file_in, buf, sizeof (buf),
                                                         cancellable, error);
                if (bytes_read < 0)
                  goto out;
                if (bytes_read == 0)
                  break;

                { ssize_t r = archive_write_data (a, buf, bytes_read);
                  if (r != bytes_read)
                    {
                      propagate_libarchive_error (error, a);
                      g_prefix_error (error, "Failed to write %" G_GUINT64_FORMAT " bytes (code %" G_GUINT64_FORMAT"): ", (guint64)bytes_read, (guint64)r);
                      goto out;
                    }
                }
              }

            if (archive_write_finish_entry (a) != ARCHIVE_OK)
              {
                propagate_libarchive_error (error, a);
                goto out;
              }

            archive_entry_free (entry);
            entry = NULL;
          }
          break;
        default:
          g_assert_not_reached ();
        }
    }

  ret = TRUE;
 out:
  if (entry)
    archive_entry_free (entry);
  return ret;
}
#endif

/**
 * ostree_repo_export_tree_to_archive:
 * @self: An #OstreeRepo
 * @opts: Options controlling conversion
 * @root: An #OstreeRepoFile for the base directory
 * @archive: A `struct archive`, but specified as void to avoid a dependency on the libarchive headers
 * @cancellable: Cancellable
 * @error: Error
 *
 * Import an archive file @archive into the repository, and write its
 * file structure to @mtree.
 */
gboolean
ostree_repo_export_tree_to_archive (OstreeRepo                *self,
                                    OstreeRepoExportArchiveOptions *opts,
                                    OstreeRepoFile            *root,
                                    void                      *archive,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = archive;

  if (!write_directory_to_libarchive_recurse (self, opts, (GFile*)root, (GFile*)root,
                                              a, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}
