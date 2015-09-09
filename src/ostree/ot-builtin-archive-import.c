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
#include "ostree-libarchive-input-stream.h"
#endif

static GOptionEntry options[] = {
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  char *commit_checksum;
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
parse_object_name (const char *s,
                   char **out_checksum,
                   OstreeObjectType *out_objtype,
                   GError **error)
{
  gboolean ret = FALSE;
  const char *dot;
  
  dot = strrchr (s, '.');
  if (!dot)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                           "Missing '.' in object filename");
      goto out;
    }

  if (strcmp (dot, ".file") == 0)
    *out_objtype = OSTREE_OBJECT_TYPE_FILE;
  else if (strcmp (dot, ".dirtree") == 0)
    *out_objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
  else if (strcmp (dot, ".dirmeta") == 0)
    *out_objtype = OSTREE_OBJECT_TYPE_DIR_META;
  else if (strcmp (dot, ".commit") == 0)
    *out_objtype = OSTREE_OBJECT_TYPE_COMMIT;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                   "Invalid object suffix: %s", dot);
      goto out;
    }

  *out_checksum = g_strndup (s, dot - s);

  ret = TRUE;
 out:
  return ret;
}


static gboolean
import_object_from_zipfile (ArchiveState          *self,
                            struct archive        *zipfile,
                            struct archive_entry  *entry,
                            GCancellable          *cancellable,
                            GError               **error)
{
  gboolean ret = FALSE;
  g_autoptr (GInputStream) instream = NULL;
  const char *pathname = NULL;
  g_autofree char *checksum = NULL;
  OstreeObjectType objtype;

  pathname = archive_entry_pathname (entry);
  
  if (!parse_object_name (pathname, &checksum, &objtype, error))
    goto out;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      GByteArray *buf = g_byte_array_new ();

      while (TRUE)
        {
          const guint8 *tmp_buf = NULL;
          size_t read_size;
          gint64 offset;
          int r;
          
          r = archive_read_data_block (zipfile, (const void**)&tmp_buf, &read_size, &offset);
          if (r == ARCHIVE_EOF)
            break;
          if (r < ARCHIVE_OK)
            {
              propagate_libarchive_error (error, zipfile);
              goto out;
            }

          if (read_size == 0)
            break;

          g_byte_array_append (buf, tmp_buf, read_size);
        }
      
      {
        g_autoptr(GVariant) metadata
          = g_variant_new_from_data (ostree_metadata_variant_type (objtype),
                                     buf->data, buf->len, TRUE, NULL, NULL);
        if (!ostree_repo_write_metadata_trusted (self->repo, objtype, checksum, metadata,
                                                 cancellable, error))
          goto out;
      }

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT && self->commit_checksum == NULL)
        {
          self->commit_checksum = g_strdup (checksum);
        }

      g_byte_array_unref (buf);
    }
  else
    {
      g_autoptr(GInputStream) archive_stream = _ostree_libarchive_input_stream_new (zipfile);

      if (!ostree_repo_write_content_trusted (self->repo,
                                              checksum, archive_stream, archive_entry_size (entry),
                                              cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

#endif

gboolean
ostree_builtin_archive_import (int argc, char **argv, GCancellable *cancellable, GError **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *ref;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GVariant) commit = NULL;
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
      ot_util_usage_error (context, "A branch name is required", error);
      goto out;
    }
  ref = argv[1];

  self.repo = repo;

  zipfile = archive_read_new ();
  archive_read_support_format_all (zipfile);
  archive_read_support_filter_all (zipfile);

  if (archive_read_open_filename (zipfile, NULL, 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, zipfile);
      goto out;
    }

  while (TRUE)
    {
      struct archive_entry *entry = NULL;
      int r;

      r = archive_read_next_header (zipfile, &entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, zipfile);
          goto out;
        }

      if (archive_entry_filetype (entry) == AE_IFREG)
        {
          if (!import_object_from_zipfile (&self, zipfile, entry, cancellable, error))
            goto out;
        }
    }

  if (archive_read_close (zipfile) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, zipfile);
      goto out;
    }

  if (!self.commit_checksum)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No commit found in import");
      goto out;
    }

  if (!ostree_repo_set_ref_immediate (repo, NULL, ref, self.commit_checksum,
                                      cancellable, error))
    goto out;
             
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
