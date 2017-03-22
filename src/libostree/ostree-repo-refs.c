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
 */

#include "config.h"

#include "ostree-repo-private.h"
#include "otutil.h"
#include "ot-fs-utils.h"

static gboolean
add_ref_to_set (const char       *remote,
                int               base_fd,
                const char       *path,
                GHashTable       *refs,
                GCancellable     *cancellable,
                GError          **error)
{
  gsize len;
  char *contents = glnx_file_get_contents_utf8_at (base_fd, path, &len, cancellable, error);
  if (!contents)
    return FALSE;

  g_strchomp (contents);

  g_autoptr(GString) refname = g_string_new ("");
  if (remote)
    {
      g_string_append (refname, remote);
      g_string_append_c (refname, ':');
    }
  g_string_append (refname, path);
  g_hash_table_insert (refs, g_string_free (g_steal_pointer (&refname), FALSE), contents);

  return TRUE;
}

static gboolean
write_checksum_file_at (OstreeRepo   *self,
                        int dfd,
                        const char *name,
                        const char *sha256,
                        GCancellable *cancellable,
                        GError **error)
{
  if (!ostree_validate_checksum_string (sha256, error))
    return FALSE;

  if (ostree_validate_checksum_string (name, NULL))
    return glnx_throw (error, "Rev name '%s' looks like a checksum", name);

  if (!*name)
    return glnx_throw (error, "Invalid empty ref name");

  const char *lastslash = strrchr (name, '/');

  if (lastslash)
    {
      char *parent = strdupa (name);
      parent[lastslash - name] = '\0';

      if (!glnx_shutil_mkdir_p_at (dfd, parent, 0777, cancellable, error))
        return FALSE;
    }

  {
    size_t l = strlen (sha256);
    char *bufnl = alloca (l + 2);
    g_autoptr(GError) temp_error = NULL;

    memcpy (bufnl, sha256, l);
    bufnl[l] = '\n';
    bufnl[l+1] = '\0';

    if (!_ostree_repo_file_replace_contents (self, dfd, name, (guint8*)bufnl, l + 1,
                                             cancellable, &temp_error))
      {
        if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
          {
            g_autoptr(GHashTable) refs = NULL;
            GHashTableIter hashiter;
            gpointer hashkey, hashvalue;

            g_clear_error (&temp_error);

            if (!ostree_repo_list_refs (self, name, &refs, cancellable, error))
              return FALSE;

            g_hash_table_iter_init (&hashiter, refs);

            while ((g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue)))
              {
                if (strcmp (name, (char *)hashkey) != 0)
                  return glnx_throw (error, "Conflict: %s exists under %s when attempting write", (char*)hashkey, name);
              }

            if (!glnx_shutil_rm_rf_at (dfd, name, cancellable, error))
              return FALSE;

            if (!_ostree_repo_file_replace_contents (self, dfd, name, (guint8*)bufnl, l + 1,
                                                     cancellable, error))
              return FALSE;
          }
        else
          {
            g_propagate_error (error, g_steal_pointer (&temp_error));
            return FALSE;
          }
      }
  }

  return TRUE;
}

static gboolean
find_ref_in_remotes (OstreeRepo         *self,
                     const char         *rev,
                     int                *out_fd,
                     GError            **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_fd_close int ret_fd = -1;

  if (!glnx_dirfd_iterator_init_at (self->repo_dir_fd, "refs/remotes", TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      glnx_fd_close int remote_dfd = -1;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      if (!glnx_opendirat (dfd_iter.fd, dent->d_name, TRUE, &remote_dfd, error))
        return FALSE;

      if (!ot_openat_ignore_enoent (remote_dfd, rev, &ret_fd, error))
        return FALSE;

      if (ret_fd != -1)
        break;
    }

  *out_fd = ret_fd; ret_fd = -1;
  return TRUE;
}

static gboolean
resolve_refspec (OstreeRepo     *self,
                 const char     *remote,
                 const char     *ref,
                 gboolean        allow_noent,
                 gboolean        fallback_remote,
                 char          **out_rev,
                 GError        **error);

static gboolean
resolve_refspec_fallback (OstreeRepo     *self,
                          const char     *remote,
                          const char     *ref,
                          gboolean        allow_noent,
                          gboolean        fallback_remote,
                          char          **out_rev,
                          GCancellable   *cancellable,
                          GError        **error)
{
  g_autofree char *ret_rev = NULL;

  if (self->parent_repo)
    {
      if (!resolve_refspec (self->parent_repo, remote, ref, allow_noent,
                            fallback_remote, &ret_rev, error))
        return FALSE;
    }
  else if (!allow_noent)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Refspec '%s%s%s' not found",
                   remote ? remote : "",
                   remote ? ":" : "",
                   ref);
      return FALSE;
    }

  ot_transfer_out_value (out_rev, &ret_rev);
  return TRUE;
}

static gboolean
resolve_refspec (OstreeRepo     *self,
                 const char     *remote,
                 const char     *ref,
                 gboolean        allow_noent,
                 gboolean        fallback_remote,
                 char          **out_rev,
                 GError        **error)
{
  __attribute__((unused)) GCancellable *cancellable = NULL;
  g_autofree char *ret_rev = NULL;
  glnx_fd_close int target_fd = -1;

  g_return_val_if_fail (ref != NULL, FALSE);

  /* We intentionally don't allow a ref that looks like a checksum */
  if (ostree_validate_checksum_string (ref, NULL))
    {
      ret_rev = g_strdup (ref);
    }
  else if (remote != NULL)
    {
      const char *remote_ref = glnx_strjoina ("refs/remotes/", remote, "/", ref);

      if (!ot_openat_ignore_enoent (self->repo_dir_fd, remote_ref, &target_fd, error))
        return FALSE;
    }
  else
    {
      const char *local_ref = glnx_strjoina ("refs/heads/", ref);

      if (!ot_openat_ignore_enoent (self->repo_dir_fd, local_ref, &target_fd, error))
        return FALSE;

      if (target_fd == -1 && fallback_remote)
        {
          local_ref = glnx_strjoina ("refs/remotes/", ref);

          if (!ot_openat_ignore_enoent (self->repo_dir_fd, local_ref, &target_fd, error))
            return FALSE;

          if (target_fd == -1)
            {
              if (!find_ref_in_remotes (self, ref, &target_fd, error))
                return FALSE;
            }
        }
    }

  if (target_fd != -1)
    {
      ret_rev = glnx_fd_readall_utf8 (target_fd, NULL, NULL, error);
      if (!ret_rev)
        {
          g_prefix_error (error, "Couldn't open ref '%s': ", ref);
          return FALSE;
        }

      g_strchomp (ret_rev);
      if (!ostree_validate_checksum_string (ret_rev, error))
        return FALSE;
    }
  else
    {
      if (!resolve_refspec_fallback (self, remote, ref, allow_noent, fallback_remote,
                                     &ret_rev, cancellable, error))
        return FALSE;
    }

  ot_transfer_out_value (out_rev, &ret_rev);
  return TRUE;
}

/**
 * ostree_repo_resolve_partial_checksum:
 * @self: Repo
 * @refspec: A refspec
 * @full_checksum (out) (transfer full): A full checksum corresponding to the truncated ref given
 * @error: Error
 *
 * Look up the existing refspec checksums.  If the given ref is a unique truncated beginning
 * of a valid checksum it will return that checksum in the parameter @full_checksum
 */
static gboolean
ostree_repo_resolve_partial_checksum (OstreeRepo   *self,
                                      const char   *refspec,
                                      char        **full_checksum,
                                      GError      **error)
{
  static const char hexchars[] = "0123456789abcdef";
  g_autofree char *ret_rev = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* If the input is longer than OSTREE_SHA256_STRING_LEN chars or contains non-hex chars,
     don't bother looking for it as an object */
  const gsize off = strspn (refspec, hexchars);
  if (off > OSTREE_SHA256_STRING_LEN || refspec[off] != '\0')
    return TRUE;

  /* this looks through all objects and adds them to the ref_list if:
     a) they are a commit object AND
     b) the obj checksum starts with the partual checksum defined by "refspec" */
  g_autoptr(GHashTable) ref_list = NULL;
  if (!ostree_repo_list_commit_objects_starting_with (self, refspec, &ref_list, NULL, error))
    return FALSE;

  guint length = g_hash_table_size (ref_list);

  GHashTableIter hashiter;
  gpointer key, value;
  GVariant *first_commit = NULL;
  g_hash_table_iter_init (&hashiter, ref_list);
  if (g_hash_table_iter_next (&hashiter, &key, &value))
    first_commit = (GVariant*) key;

  OstreeObjectType objtype;
  const char *checksum = NULL;
  if (first_commit)
    ostree_object_name_deserialize (first_commit, &checksum, &objtype);

  /* length more than one - multiple commits match partial refspec: is not unique */
  if (length > 1)
    return glnx_throw (error, "Refspec %s not unique", refspec);
  /* length is 1 - a single matching commit gives us our revision */
  else if (length == 1)
    ret_rev = g_strdup (checksum);

  /* Note: if length is 0, then code will return TRUE
     because there is no error, but it will return full_checksum = NULL
     to signal to continue parsing */

  ot_transfer_out_value (full_checksum, &ret_rev);
  return TRUE;
}

static gboolean
_ostree_repo_resolve_rev_internal (OstreeRepo     *self,
                                   const char     *refspec,
                                   gboolean        allow_noent,
                                   gboolean        fallback_remote,
                                   char          **out_rev,
                                   GError        **error)
{
  g_autofree char *ret_rev = NULL;

  g_return_val_if_fail (refspec != NULL, FALSE);

  if (ostree_validate_checksum_string (refspec, NULL))
    {
      ret_rev = g_strdup (refspec);
    }

  else if (!ostree_repo_resolve_partial_checksum (self, refspec, &ret_rev, error))
    return FALSE;

  if (!ret_rev)
    {
      if (error != NULL && *error != NULL)
        return FALSE;

      if (g_str_has_suffix (refspec, "^"))
        {
          g_autofree char *parent_refspec = NULL;
          g_autofree char *parent_rev = NULL;
          g_autoptr(GVariant) commit = NULL;

          parent_refspec = g_strdup (refspec);
          parent_refspec[strlen(parent_refspec) - 1] = '\0';

          if (!ostree_repo_resolve_rev (self, parent_refspec, allow_noent, &parent_rev, error))
            return FALSE;

          if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, parent_rev,
                                         &commit, error))
            return FALSE;

          if (!(ret_rev = ostree_commit_get_parent (commit)))
            return glnx_throw (error, "Commit %s has no parent", parent_rev);
        }
      else
        {
          g_autofree char *remote = NULL;
          g_autofree char *ref = NULL;

          if (!ostree_parse_refspec (refspec, &remote, &ref, error))
            return FALSE;

          if (!resolve_refspec (self, remote, ref, allow_noent,
                                fallback_remote, &ret_rev, error))
            return FALSE;
        }
    }

  ot_transfer_out_value (out_rev, &ret_rev);
  return TRUE;
}

/**
 * ostree_repo_resolve_rev:
 * @self: Repo
 * @refspec: A refspec
 * @allow_noent: Do not throw an error if refspec does not exist
 * @out_rev: (out) (transfer full): A checksum,or %NULL if @allow_noent is true and it does not exist
 * @error: Error
 *
 * Look up the given refspec, returning the checksum it references in
 * the parameter @out_rev. Will fall back on remote directory if cannot
 * find the given refspec in local.
 */
gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *refspec,
                         gboolean        allow_noent,
                         char          **out_rev,
                         GError        **error)
{
  return _ostree_repo_resolve_rev_internal (self, refspec, allow_noent, TRUE, out_rev, error);
}

/**
 * ostree_repo_resolve_rev_ext:
 * @self: Repo
 * @refspec: A refspec
 * @allow_noent: Do not throw an error if refspec does not exist
 * @flags: Options controlling behavior
 * @out_rev: (out) (transfer full): A checksum,or %NULL if @allow_noent is true and it does not exist
 * @error: Error
 *
 * Look up the given refspec, returning the checksum it references in
 * the parameter @out_rev. Differently from ostree_repo_resolve_rev(),
 * this will not fall back to searching through remote repos if a
 * local ref is specified but not found.
 */
gboolean
ostree_repo_resolve_rev_ext (OstreeRepo                    *self,
                             const char                    *refspec,
                             gboolean                       allow_noent,
                             OstreeRepoResolveRevExtFlags   flags,
                             char                         **out_rev,
                             GError                       **error)
{
  return _ostree_repo_resolve_rev_internal (self, refspec, allow_noent, FALSE, out_rev, error);
}

static gboolean
enumerate_refs_recurse (OstreeRepo    *repo,
                        const char    *remote,
                        int            base_dfd,
                        GString       *base_path,
                        int            child_dfd,
                        const char    *path,
                        GHashTable    *refs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  if (!glnx_dirfd_iterator_init_at (child_dfd, path, FALSE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      guint len = base_path->len;
      struct dirent *dent = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      g_string_append (base_path, dent->d_name);

      if (dent->d_type == DT_DIR)
        {
          g_string_append_c (base_path, '/');

          if (!enumerate_refs_recurse (repo, remote, base_dfd, base_path,
                                       dfd_iter.fd, dent->d_name,
                                       refs, cancellable, error))
            return FALSE;
        }
      else if (dent->d_type == DT_REG)
        {
          if (!add_ref_to_set (remote, base_dfd, base_path->str, refs,
                               cancellable, error))
            return FALSE;
        }

      g_string_truncate (base_path, len);
    }

  return TRUE;
}

static gboolean
_ostree_repo_list_refs_internal (OstreeRepo       *self,
                                 gboolean         cut_prefix,
                                 const char       *refspec_prefix,
                                 GHashTable      **out_all_refs,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  g_autoptr(GHashTable) ret_all_refs = NULL;
  g_autofree char *remote = NULL;
  g_autofree char *ref_prefix = NULL;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (refspec_prefix)
    {
      struct stat stbuf;
      const char *prefix_path;
      const char *path;

      if (!ostree_parse_refspec (refspec_prefix, &remote, &ref_prefix, error))
        return FALSE;

      if (remote)
        {
          prefix_path = glnx_strjoina ("refs/remotes/", remote, "/");
          path = glnx_strjoina (prefix_path, ref_prefix);
        }
      else
        {
          prefix_path = "refs/heads/";
          path = glnx_strjoina (prefix_path, ref_prefix);
        }

      if (fstatat (self->repo_dir_fd, path, &stbuf, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno (error);
        }
      else
        {
          if (S_ISDIR (stbuf.st_mode))
            {
              glnx_fd_close int base_fd = -1;
              g_autoptr(GString) base_path = g_string_new ("");
              if (!cut_prefix)
                g_string_printf (base_path, "%s/", ref_prefix);

              if (!glnx_opendirat (self->repo_dir_fd, cut_prefix ? path : prefix_path, TRUE, &base_fd, error))
                return FALSE;

              if (!enumerate_refs_recurse (self, remote, base_fd, base_path,
                                           base_fd, cut_prefix ? "." : ref_prefix,
                                           ret_all_refs, cancellable, error))
                return FALSE;
            }
          else
            {
              glnx_fd_close int prefix_dfd = -1;

              if (!glnx_opendirat (self->repo_dir_fd, prefix_path, TRUE, &prefix_dfd, error))
                return FALSE;

              if (!add_ref_to_set (remote, prefix_dfd, ref_prefix, ret_all_refs,
                                   cancellable, error))
                return FALSE;
            }
        }
    }
  else
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
      g_autoptr(GString) base_path = g_string_new ("");
      glnx_fd_close int refs_heads_dfd = -1;

      if (!glnx_opendirat (self->repo_dir_fd, "refs/heads", TRUE, &refs_heads_dfd, error))
        return FALSE;

      if (!enumerate_refs_recurse (self, NULL, refs_heads_dfd, base_path,
                                   refs_heads_dfd, ".",
                                   ret_all_refs, cancellable, error))
        return FALSE;

      g_string_truncate (base_path, 0);

      if (!glnx_dirfd_iterator_init_at (self->repo_dir_fd, "refs/remotes", TRUE, &dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *dent;
          glnx_fd_close int remote_dfd = -1;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
            return FALSE;
          if (!dent)
            break;

          if (dent->d_type != DT_DIR)
            continue;

          if (!glnx_opendirat (dfd_iter.fd, dent->d_name, TRUE, &remote_dfd, error))
            return FALSE;

          if (!enumerate_refs_recurse (self, dent->d_name, remote_dfd, base_path,
                                       remote_dfd, ".",
                                       ret_all_refs,
                                       cancellable, error))
            return FALSE;
        }
    }

  ot_transfer_out_value (out_all_refs, &ret_all_refs);
  return TRUE;
}

/**
 * ostree_repo_list_refs:
 * @self: Repo
 * @refspec_prefix: (allow-none): Only list refs which match this prefix
 * @out_all_refs: (out) (element-type utf8 utf8): Mapping from ref to checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * If @refspec_prefix is %NULL, list all local and remote refspecs,
 * with their current values in @out_all_refs.  Otherwise, only list
 * refspecs which have @refspec_prefix as a prefix.
 */
gboolean
ostree_repo_list_refs (OstreeRepo       *self,
                       const char       *refspec_prefix,
                       GHashTable      **out_all_refs,
                       GCancellable     *cancellable,
                       GError          **error)
{
  return _ostree_repo_list_refs_internal (self, TRUE, refspec_prefix, out_all_refs, cancellable, error);
}

/**
 * ostree_repo_list_refs_ext:
 * @self: Repo
 * @refspec_prefix: (allow-none): Only list refs which match this prefix
 * @out_all_refs: (out) (element-type utf8 utf8): Mapping from ref to checksum
 * @flags: Options controlling listing behavior
 * @cancellable: Cancellable
 * @error: Error
 *
 * If @refspec_prefix is %NULL, list all local and remote refspecs,
 * with their current values in @out_all_refs.  Otherwise, only list
 * refspecs which have @refspec_prefix as a prefix.  Differently from
 * ostree_repo_list_refs(), the prefix will not be removed from the ref
 * name.
 */
gboolean
ostree_repo_list_refs_ext (OstreeRepo                 *self,
                           const char                 *refspec_prefix,
                           GHashTable                 **out_all_refs,
                           OstreeRepoListRefsExtFlags flags,
                           GCancellable               *cancellable,
                           GError                     **error)
{
  return _ostree_repo_list_refs_internal (self, FALSE, refspec_prefix, out_all_refs, cancellable, error);
}

/**
 * ostree_repo_remote_list_refs:
 * @self: Repo
 * @remote_name: Name of the remote.
 * @out_all_refs: (out) (element-type utf8 utf8): Mapping from ref to checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 */
gboolean
ostree_repo_remote_list_refs (OstreeRepo       *self,
                              const char       *remote_name,
                              GHashTable      **out_all_refs,
                              GCancellable     *cancellable,
                              GError          **error)
{
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GHashTable) ret_all_refs = NULL;

  if (!ostree_repo_remote_fetch_summary (self, remote_name,
                                         &summary_bytes, NULL,
                                         cancellable, error))
    return FALSE;

  if (summary_bytes == NULL)
    {
      return glnx_throw (error, "Remote refs not available; server has no summary file");
    }
  else
    {
      g_autoptr(GVariant) summary = NULL;
      g_autoptr(GVariant) ref_map = NULL;
      GVariantIter iter;
      GVariant *child;
      ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                          summary_bytes, FALSE);

      ref_map = g_variant_get_child_value (summary, 0);

      g_variant_iter_init (&iter, ref_map);
      while ((child = g_variant_iter_next_value (&iter)) != NULL)
        {
          const char *ref_name = NULL;
          g_autoptr(GVariant) csum_v = NULL;
          char tmp_checksum[OSTREE_SHA256_STRING_LEN+1];

          g_variant_get_child (child, 0, "&s", &ref_name);

          if (ref_name != NULL)
            {
              g_variant_get_child (child, 1, "(t@aya{sv})", NULL, &csum_v, NULL);

              const guchar *csum_bytes = ostree_checksum_bytes_peek_validate (csum_v, error);
              if (csum_bytes == NULL)
                return FALSE;

              ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

              g_hash_table_insert (ret_all_refs,
                                   g_strdup (ref_name),
                                   g_strdup (tmp_checksum));
            }

          g_variant_unref (child);
        }
    }

  ot_transfer_out_value (out_all_refs, &ret_all_refs);
  return TRUE;
}

gboolean
_ostree_repo_write_ref (OstreeRepo    *self,
                        const char    *remote,
                        const char    *ref,
                        const char    *rev,
                        GCancellable  *cancellable,
                        GError       **error)
{
  glnx_fd_close int dfd = -1;

  if (remote == NULL)
    {
      if (!glnx_opendirat (self->repo_dir_fd, "refs/heads", TRUE,
                           &dfd, error))
        {
          g_prefix_error (error, "Opening %s: ", "refs/heads");
          return FALSE;
        }
    }
  else
    {
      glnx_fd_close int refs_remotes_dfd = -1;

      if (!glnx_opendirat (self->repo_dir_fd, "refs/remotes", TRUE,
                           &refs_remotes_dfd, error))
        {
          g_prefix_error (error, "Opening %s: ", "refs/remotes");
          return FALSE;
        }

      if (rev != NULL)
        {
          /* Ensure we have a dir for the remote */
          if (!glnx_shutil_mkdir_p_at (refs_remotes_dfd, remote, 0777, cancellable, error))
            return FALSE;
        }

      dfd = glnx_opendirat_with_errno (refs_remotes_dfd, remote, TRUE);
      if (dfd < 0 && (errno != ENOENT || rev != NULL))
        return glnx_throw_errno_prefix (error, "Opening remotes/ dir %s", remote);
    }

  if (rev == NULL)
    {
      if (dfd >= 0)
        {
          if (unlinkat (dfd, ref, 0) != 0)
            {
              if (errno != ENOENT)
                return glnx_throw_errno (error);
            }
        }
    }
  else
    {
      if (!write_checksum_file_at (self, dfd, ref, rev, cancellable, error))
        return FALSE;
    }

  if (!_ostree_repo_update_mtime (self, error))
    return FALSE;

  return TRUE;
}

gboolean
_ostree_repo_update_refs (OstreeRepo        *self,
                          GHashTable        *refs,
                          GCancellable      *cancellable,
                          GError           **error)
{
  GHashTableIter hash_iter;
  gpointer key, value;

  g_hash_table_iter_init (&hash_iter, refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *refspec = key;
      const char *rev = value;
      g_autofree char *remote = NULL;
      g_autofree char *ref = NULL;

      if (!ostree_parse_refspec (refspec, &remote, &ref, error))
        return FALSE;

      if (!_ostree_repo_write_ref (self, remote, ref, rev,
                                   cancellable, error))
        return FALSE;
    }

  return TRUE;
}
