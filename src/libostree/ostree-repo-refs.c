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
#include "ostree-deployment.h"

static gboolean
add_ref_to_set (const char       *remote,
                GFile            *base,
                GFile            *child,
                GHashTable       *refs,
                GCancellable     *cancellable,
                GError          **error)
{
  gboolean ret = FALSE;
  char *contents;
  char *relpath;
  gsize len;
  GString *refname;

  if (!g_file_load_contents (child, cancellable, &contents, &len, NULL, error))
    goto out;

  g_strchomp (contents);

  refname = g_string_new ("");
  if (remote)
    {
      g_string_append (refname, remote);
      g_string_append_c (refname, ':');
    }
  relpath = g_file_get_relative_path (base, child);
  g_string_append (refname, relpath);
  g_free (relpath);
          
  g_hash_table_insert (refs, g_string_free (refname, FALSE), contents);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_checksum_file (GFile *parentdir,
                     const char *name,
                     const char *sha256,
                     GCancellable *cancellable,
                     GError **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;
  int i;
  gs_unref_object GFile *parent = NULL;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GOutputStream *out = NULL;
  gs_unref_ptrarray GPtrArray *components = NULL;

  if (!ostree_validate_checksum_string (sha256, error))
    goto out;

  if (ostree_validate_checksum_string (name, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Rev name '%s' looks like a checksum", name);
      goto out;
    }

  if (!ot_util_path_split_validate (name, &components, error))
    goto out;

  if (components->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty ref name");
      goto out;
    }

  parent = g_object_ref (parentdir);
  for (i = 0; i+1 < components->len; i++)
    {
      child = g_file_get_child (parent, (char*)components->pdata[i]);

      if (!gs_file_ensure_directory (child, FALSE, cancellable, error))
        goto out;

      g_clear_object (&parent);
      parent = child;
      child = NULL;
    }

  child = g_file_get_child (parent, components->pdata[components->len - 1]);
  if ((out = (GOutputStream*)g_file_replace (child, NULL, FALSE, 0, cancellable, error)) == NULL)
    goto out;
  if (!g_output_stream_write_all (out, sha256, strlen (sha256), &bytes_written, cancellable, error))
    goto out;
  if (!g_output_stream_write_all (out, "\n", 1, &bytes_written, cancellable, error))
    goto out;
  if (!g_output_stream_close (out, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error) G_GNUC_UNUSED;

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gs_free char *rev = NULL;

  if ((rev = gs_file_load_contents_utf8 (f, NULL, &temp_error)) == NULL)
    goto out;

  if (rev == NULL)
    {
      if (g_error_matches (temp_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    {
      g_strchomp (rev);
    }

  if (g_str_has_prefix (rev, "ref: "))
    {
      gs_unref_object GFile *ref = NULL;
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (self->local_heads_dir, rev + 5);
      subret = parse_rev_file (self, ref, &ref_sha256, error);
        
      if (!subret)
        {
          g_free (ref_sha256);
          goto out;
        }
      
      g_free (rev);
      rev = ref_sha256;
    }
  else 
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;
    }

  ot_transfer_out_value(sha256, &rev);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
find_ref_in_remotes (OstreeRepo         *self,
                     const char         *rev,
                     GFile             **out_file,
                     GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *ret_file = NULL;

  dir_enum = g_file_enumerate_children (self->remote_heads_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
                                       NULL, error))
        goto out;
      if (file_info == NULL)
        break;
      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        continue;

      g_clear_object (&ret_file);
      ret_file = g_file_resolve_relative_path (child, rev);
      if (!g_file_query_exists (ret_file, NULL))
        g_clear_object (&ret_file);
      else
        break;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

static gboolean
resolve_refspec (OstreeRepo     *self,
                 const char     *remote,
                 const char     *ref,
                 gboolean        allow_noent,
                 char          **out_rev,
                 GError        **error);

static gboolean
resolve_refspec_fallback (OstreeRepo     *self,
                          const char     *remote,
                          const char     *ref,
                          gboolean        allow_noent,
                          char          **out_rev,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_rev = NULL;

  if (self->parent_repo)
    {
      if (!resolve_refspec (self->parent_repo, remote, ref,
                            allow_noent, &ret_rev, error))
        goto out;
    }
  else if (!allow_noent)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Refspec '%s%s%s' not found",
                   remote ? remote : "",
                   remote ? ":" : "",
                   ref);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_rev, &ret_rev);
 out:
  return ret;
}

static gboolean
resolve_refspec (OstreeRepo     *self,
                 const char     *remote,
                 const char     *ref,
                 gboolean        allow_noent,
                 char          **out_rev,
                 GError        **error)
{
  gboolean ret = FALSE;
  __attribute__((unused)) GCancellable *cancellable = NULL;
  GError *temp_error = NULL;
  gs_free char *ret_rev = NULL;
  gs_unref_object GFile *child = NULL;
  
  g_return_val_if_fail (ref != NULL, FALSE);

  /* We intentionally don't allow a ref that looks like a checksum */
  if (ostree_validate_checksum_string (ref, NULL))
    {
      ret_rev = g_strdup (ref);
    }
  else if (remote != NULL)
    {
      child = ot_gfile_resolve_path_printf (self->remote_heads_dir, "%s/%s",
                                            remote, ref);
      if (!g_file_query_exists (child, NULL))
        g_clear_object (&child);
    }
  else
    {
      child = g_file_resolve_relative_path (self->local_heads_dir, ref);

      if (!g_file_query_exists (child, NULL))
        {
          g_clear_object (&child);

          child = g_file_resolve_relative_path (self->remote_heads_dir, ref);

          if (!g_file_query_exists (child, NULL))
            {
              g_clear_object (&child);
              
              if (!find_ref_in_remotes (self, ref, &child, error))
                goto out;
            }
        }
    }

  if (child)
    {
      if ((ret_rev = gs_file_load_contents_utf8 (child, NULL, &temp_error)) == NULL)
        {
          g_propagate_error (error, temp_error);
          g_prefix_error (error, "Couldn't open ref '%s': ", gs_file_get_path_cached (child));
          goto out;
        }

      g_strchomp (ret_rev);
      if (!ostree_validate_checksum_string (ret_rev, error))
        goto out;
    }
  else
    {
      if (!resolve_refspec_fallback (self, remote, ref, allow_noent,
                                     &ret_rev, cancellable, error))
        goto out;
    }

  ot_transfer_out_value (out_rev, &ret_rev);
  ret = TRUE;
 out:
  return ret;
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
  gboolean ret = FALSE;
  static const char hexchars[] = "0123456789abcdef";
  gsize off;
  gs_unref_hashtable GHashTable *ref_list = NULL;
  gs_free char *ret_rev = NULL;
  guint length;
  const char *checksum = NULL;
  OstreeObjectType objtype;
  GHashTableIter hashiter;
  gpointer key, value;
  GVariant *first_commit;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* If the input is longer than 64 chars or contains non-hex chars,
     don't bother looking for it as an object */
  off = strspn (refspec, hexchars);
  if (off > 64 || refspec[off] != '\0')
    return TRUE;

  /* this looks through all objects and adds them to the ref_list if:
     a) they are a commit object AND
     b) the obj checksum starts with the partual checksum defined by "refspec" */
  if (!ostree_repo_list_commit_objects_starting_with (self, refspec, &ref_list, NULL, error))
    goto out;

  length = g_hash_table_size (ref_list);

  g_hash_table_iter_init (&hashiter, ref_list);
  if (g_hash_table_iter_next (&hashiter, &key, &value))
    first_commit = (GVariant*) key;
  else
    first_commit = NULL;

  if (first_commit) 
    ostree_object_name_deserialize (first_commit, &checksum, &objtype);

  /* length more than one - multiple commits match partial refspec: is not unique */
  if (length > 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Refspec %s not unique", refspec);
      goto out;
    }
    
  /* length is 1 - a single matching commit gives us our revision */
  else if (length == 1)
    {
      ret_rev = g_strdup (checksum);
    }

  /* Note: if length is 0, then code will return TRUE
     because there is no error, but it will return full_checksum = NULL
     to signal to continue parsing */

  ret = TRUE;
  ot_transfer_out_value (full_checksum, &ret_rev);
 out:
  return ret;
}

/**
 * ostree_repo_resolve_partial_checksum:
 * @self: Repo
 * @refspec: A refspec
 * @full_checksum (out) (transfer full): A full checksum corresponding to the name given
 * @error: Error
 *
 * Look up the existing deployments.  If the given ref is a deployment name
 * it will return that deployment's checksum in the parameter @full_checksum
 */
static gboolean
ostree_repo_resolve_name (OstreeRepo   *self,
                         const char   *refspec,
                         char        **full_checksum,
                         GError      **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_rev = NULL;
  gs_unref_hashtable GHashTable *ref_list = NULL;
  const char *checksum = NULL;
  OstreeObjectType objtype;
  GHashTableIter hashiter;
  gpointer key, value;
  gs_free char *deployment_name = NULL;
  gs_free char *csum = NULL;
  gs_unref_object GFile *path_to_customs = ot_gfile_resolve_path_printf (self->repodir, "state/custom_names");

  /* returns all commit objects */
  if (!ostree_repo_list_commit_objects_starting_with (self, "", &ref_list, NULL, error))
    goto out;

  g_hash_table_iter_init (&hashiter, ref_list);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      ostree_object_name_deserialize ((GVariant*) key, &checksum, &objtype);
      csum = g_strdup (checksum);
      if (!ostree_deployment_get_name (csum, path_to_customs, &deployment_name, error))
        goto out;
      if (g_strcmp0 (deployment_name, refspec) == 0)
        {
          ret_rev = g_strdup (checksum);
          break;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (full_checksum, &ret_rev);
 out:
  return ret;
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
 * the parameter @out_rev.
 */
gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *refspec,
                         gboolean        allow_noent,
                         char          **out_rev,
                         GError        **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_rev = NULL;

  g_return_val_if_fail (refspec != NULL, FALSE);

  if (ostree_validate_checksum_string (refspec, NULL))
    {
      ret_rev = g_strdup (refspec);
    }

  else if (!ostree_repo_resolve_partial_checksum (self, refspec, &ret_rev, error))
    goto out;

  else if (!ret_rev && !ostree_repo_resolve_name (self, refspec, &ret_rev, error))
    goto out;

  if (!ret_rev)
    {
      if (error != NULL && *error != NULL)
        goto out;

      if (g_str_has_suffix (refspec, "^"))
        {
          gs_free char *parent_refspec = NULL;
          gs_free char *parent_rev = NULL;
          gs_unref_variant GVariant *commit = NULL;

          parent_refspec = g_strdup (refspec);
          parent_refspec[strlen(parent_refspec) - 1] = '\0';

          if (!ostree_repo_resolve_rev (self, parent_refspec, allow_noent, &parent_rev, error))
            goto out;
          
          if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, parent_rev,
                                         &commit, error))
            goto out;
      
          if (!(ret_rev = ostree_commit_get_parent (commit)))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Commit %s has no parent", parent_rev);
              goto out;
            }
        }
      else
        {
          gs_free char *remote = NULL;
          gs_free char *ref = NULL;

          if (!ostree_parse_refspec (refspec, &remote, &ref, error))
            goto out;
          
          if (!resolve_refspec (self, remote, ref, allow_noent,
                                &ret_rev, error))
            goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_rev, &ret_rev);
 out:
  return ret;
}

static gboolean
enumerate_refs_recurse (OstreeRepo    *repo,
                        const char    *remote,
                        GFile         *base,
                        GFile         *dir,
                        GHashTable    *refs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *enumerator = NULL;

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info = NULL;
      GFile *child = NULL;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &child,
                                       NULL, error))
        goto out;
      if (file_info == NULL)
        break;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!enumerate_refs_recurse (repo, remote, base, child, refs, cancellable, error))
            goto out;
        }
      else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          if (!add_ref_to_set (remote, base, child, refs,
                               cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
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
  gboolean ret = FALSE;
  gs_unref_hashtable GHashTable *ret_all_refs = NULL;
  gs_free char *remote = NULL;
  gs_free char *ref_prefix = NULL;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (refspec_prefix)
    {
      gs_unref_object GFile *dir = NULL;
      gs_unref_object GFile *child = NULL;
      gs_unref_object GFileInfo *info = NULL;

      if (!ostree_parse_refspec (refspec_prefix, &remote, &ref_prefix, error))
        goto out;

      if (remote)
        dir = g_file_get_child (self->remote_heads_dir, remote);
      else
        dir = g_object_ref (self->local_heads_dir);

      child = g_file_resolve_relative_path (dir, ref_prefix);
      if (!ot_gfile_query_info_allow_noent (child, OSTREE_GIO_FAST_QUERYINFO, 0,
                                            &info, cancellable, error))
        goto out;

      if (info)
        {
          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
            {
              if (!enumerate_refs_recurse (self, remote, child, child,
                                           ret_all_refs,
                                           cancellable, error))
                goto out;
            }
          else
            {
              if (!add_ref_to_set (remote, dir, child, ret_all_refs,
                                   cancellable, error))
                goto out;
            }
        }
    }
  else
    {
      gs_unref_object GFileEnumerator *remote_enumerator = NULL;

      if (!enumerate_refs_recurse (self, NULL, self->local_heads_dir, self->local_heads_dir,
                                   ret_all_refs,
                                   cancellable, error))
        goto out;

      remote_enumerator = g_file_enumerate_children (self->remote_heads_dir, OSTREE_GIO_FAST_QUERYINFO,
                                                     0,
                                                     cancellable, error);

      while (TRUE)
        {
          GFileInfo *info;
          GFile *child;
          const char *name;

          if (!gs_file_enumerator_iterate (remote_enumerator, &info, &child,
                                           cancellable, error))
            goto out;
          if (!info)
            break;

          name = g_file_info_get_name (info);
          if (!enumerate_refs_recurse (self, name, child, child,
                                       ret_all_refs,
                                       cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_all_refs, &ret_all_refs);
 out:
  return ret;
}

gboolean      
_ostree_repo_write_ref (OstreeRepo    *self,
                        const char    *remote,
                        const char    *ref,
                        const char    *rev,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *dir = NULL;

  if (remote == NULL)
    dir = g_object_ref (self->local_heads_dir);
  else
    {
      dir = g_file_get_child (self->remote_heads_dir, remote);
      
      if (rev != NULL)
        {
          if (!gs_file_ensure_directory (dir, FALSE, cancellable, error))
            goto out;
        }
    }

  if (rev == NULL)
    {
      gs_unref_object GFile *child = g_file_resolve_relative_path (dir, ref);

      if (g_file_query_exists (child, cancellable))
        {
          if (!gs_file_unlink (child, cancellable, error))
            goto out;
        }
    }
  else
    {
      if (!write_checksum_file (dir, ref, rev, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_repo_update_refs (OstreeRepo        *self,
                          GHashTable        *refs,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;

  g_hash_table_iter_init (&hash_iter, refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *refspec = key;
      const char *rev = value;
      gs_free char *remote = NULL;
      gs_free char *ref = NULL;

      if (!ostree_parse_refspec (refspec, &remote, &ref, error))
        goto out;

      if (!_ostree_repo_write_ref (self, remote, ref, rev,
                                   cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
