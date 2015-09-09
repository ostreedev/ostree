/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "ostree-cmdprivate.h"
#include "otutil.h"

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

static GOptionEntry options[] = {
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  GHashTable *seen_objects;
  guint64 commit_timestamp;
} ArchiveState;

#ifdef HAVE_LIBARCHIVE

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static gboolean
write_object_to_zipfile (ArchiveState          *self,
                         struct archive        *zipfile,
                         OstreeObjectType       objtype,
                         const char            *checksum,
                         GCancellable          *cancellable,
                         GError               **error)
{
  gboolean ret = FALSE;
  struct archive_entry *entry = NULL;
  g_autoptr (GInputStream) instream = NULL;
  guint64 size;
  guint8 buf[8192];
  g_autoptr(GVariant) object_name = ostree_object_name_serialize (checksum, objtype);

  if (g_hash_table_contains (self->seen_objects, object_name))
    {
      ret = TRUE;
      goto out;
    }

  g_hash_table_add (self->seen_objects, g_variant_ref (object_name));

  if (!ostree_repo_load_object_stream (self->repo, objtype, checksum, &instream, &size,
                                       cancellable, error))
    goto out;

  entry = archive_entry_new ();
  if (!entry)
    g_error ("OOM");
  archive_entry_set_filetype (entry, AE_IFREG);
  archive_entry_set_mode (entry, S_IFREG | 0655);
  archive_entry_set_mtime (entry, self->commit_timestamp, 0);
  archive_entry_set_size (entry, size);

  {
    g_autofree char *pathname = g_strconcat (checksum, ".", ostree_object_type_to_string (objtype), NULL);
    archive_entry_set_pathname (entry, pathname);

    if (archive_write_header (zipfile, entry) != ARCHIVE_OK)
      {
        propagate_libarchive_error (error, zipfile);
        goto out;
      }
  }

  g_clear_pointer (&entry, archive_entry_free);

  while (TRUE)
    {
      gsize bytes_read;
      if (!g_input_stream_read_all (instream, buf, sizeof (buf), &bytes_read,
                                    cancellable, error))
        goto out;

      if (bytes_read == 0)
        break;

      if (archive_write_data (zipfile, buf, bytes_read) != bytes_read)
        {
          propagate_libarchive_error (error, zipfile);
          goto out;
        }
    }

  ret = TRUE;
 out:
  if (entry != NULL)
    archive_entry_free (entry);
  return ret;
}

static gboolean
write_iter_to_zipfile (ArchiveState *self,
                       OstreeRepoCommitTraverseIter  *iter,
                       struct archive *zipfile,
                       GCancellable *cancellable,
                       GError **error)
{
  gboolean ret = FALSE;
  gboolean done = FALSE;
  
  while (!done)
    {
      OstreeRepoCommitIterResult res =
        ostree_repo_commit_traverse_iter_next (iter, cancellable, error);

      switch (res)
        {
          case OSTREE_REPO_COMMIT_ITER_RESULT_ERROR:
            goto out;
          case OSTREE_REPO_COMMIT_ITER_RESULT_END:
            done = TRUE;
            break;
          case OSTREE_REPO_COMMIT_ITER_RESULT_FILE:
            {
              char *name = NULL;
              char *checksum = NULL;
              ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);
              if (!write_object_to_zipfile (self, zipfile, OSTREE_OBJECT_TYPE_FILE, checksum,
                                            cancellable, error))
                goto out;
              break;
            }
          case OSTREE_REPO_COMMIT_ITER_RESULT_DIR:
            {
              char *name = NULL;
              char *content_checksum = NULL;
              char *meta_checksum = NULL;
              g_autoptr(GVariant) dirtree = NULL;
              ostree_cleanup_repo_commit_traverse_iter
                OstreeRepoCommitTraverseIter subiter = { 0, };

              ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum, &meta_checksum);
              if (!write_object_to_zipfile (self, zipfile,
                                            OSTREE_OBJECT_TYPE_DIR_TREE, content_checksum,
                                            cancellable, error))
                goto out;
              if (!write_object_to_zipfile (self, zipfile,
                                            OSTREE_OBJECT_TYPE_DIR_META, meta_checksum,
                                            cancellable, error))
                goto out;

              if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                             content_checksum, &dirtree,
                                             error))
                goto out;

              if (!ostree_repo_commit_traverse_iter_init_dirtree (&subiter, self->repo, dirtree,
                                                                  OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                                  error))
                goto out;

              if (!write_iter_to_zipfile (self, &subiter, zipfile, cancellable, error))
                goto out;
              
              break;
            }
        }
    }
  
  ret = TRUE;
 out:
  return ret;
}

#endif

gboolean
ostree_builtin_archive_export (int argc, char **argv, GCancellable *cancellable, GError **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *rev;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GVariant) commit = NULL;
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter ctraverseiter = { 0, };
  struct archive *zipfile = NULL;
  g_autoptr (GHashTable) seen_objects
    = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                             NULL, (GDestroyNotify)g_variant_unref);
  ArchiveState self = { NULL, };

  context = g_option_context_new ("- Serialize a single commit as a stream");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "A commit reference is required", error);
      goto out;
    }
  rev = argv[1];

  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &commit_checksum, error))
    goto out;

  self.repo = repo;
  self.seen_objects = seen_objects;
  
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit, error))
    goto out;

  self.commit_timestamp = ostree_commit_get_timestamp (commit);

  zipfile = archive_write_new ();
  if (zipfile == NULL)
    g_error ("OOM");
  if (archive_write_set_format_gnutar (zipfile) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, zipfile);
      goto out;
    }
  if (archive_write_open_fd (zipfile, 1) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, zipfile);
      goto out;
    }

  if (!write_object_to_zipfile (&self, zipfile, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum,
                                cancellable, error))
    goto out;

  if (!ostree_repo_commit_traverse_iter_init_commit (&ctraverseiter,
                                                     repo, commit,
                                                     OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                     error))
    goto out;

  if (!write_iter_to_zipfile (&self, &ctraverseiter, zipfile, cancellable, error))
    goto out;

  if (archive_write_close (zipfile) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, zipfile);
      goto out;
    }
  
  if (archive_write_free (zipfile) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, zipfile);
      goto out;
    }
             
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
#else
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "ostree was not built with libarchive support");
  return FALSE;
#endif
}
