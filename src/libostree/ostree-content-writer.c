/* 
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-content-writer.h"
#include "ostree-repo-private.h"
#include "ostree-autocleanups.h"

struct _OstreeContentWriter
{
  GOutputStream parent_instance;

  OstreeRepo *repo;
  OstreeRepoBareContent output;
};

G_DEFINE_TYPE (OstreeContentWriter, ostree_content_writer, G_TYPE_OUTPUT_STREAM)

static void     ostree_content_writer_finalize     (GObject *object);
static gssize   ostree_content_writer_write         (GOutputStream         *stream,
                                                     const void                 *buffer,
                                                     gsize                 count,
                                                     GCancellable         *cancellable,
                                                     GError              **error);
static gboolean ostree_content_writer_close        (GOutputStream         *stream,
                                                    GCancellable         *cancellable,
                                                    GError              **error);

static void
ostree_content_writer_class_init (OstreeContentWriterClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  
  gobject_class->finalize     = ostree_content_writer_finalize;

  stream_class->write_fn = ostree_content_writer_write;
  stream_class->close_fn = ostree_content_writer_close;
}

static void
ostree_content_writer_finalize (GObject *object)
{
  OstreeContentWriter *stream;

  stream = (OstreeContentWriter*)(object);

  g_clear_object (&stream->repo);
  _ostree_repo_bare_content_cleanup (&stream->output);

  G_OBJECT_CLASS (ostree_content_writer_parent_class)->finalize (object);
}

static void
ostree_content_writer_init (OstreeContentWriter *self)
{
  self->output.initialized = FALSE;
 }

OstreeContentWriter *
_ostree_content_writer_new (OstreeRepo           *repo,
                           const char            *checksum,
                           guint                  uid,
                           guint                  gid,
                           guint                  mode,
                           guint64                content_len,
                           GVariant              *xattrs,
                           GError               **error)
{
  g_autoptr(OstreeContentWriter) stream = g_object_new (OSTREE_TYPE_CONTENT_WRITER, NULL);
  stream->repo = g_object_ref (repo);
  if (!_ostree_repo_bare_content_open (stream->repo, checksum, content_len, uid, gid, mode, xattrs,
                                       &stream->output, NULL, error))
    return NULL;
  return g_steal_pointer (&stream);
}

static gssize
ostree_content_writer_write (GOutputStream  *stream,
                             const void    *buffer,
                             gsize          count,
                             GCancellable  *cancellable,
                             GError       **error)
{
  OstreeContentWriter *self = (OstreeContentWriter*) stream;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

  if (!_ostree_repo_bare_content_write (self->repo, &self->output,
                                        buffer, count, cancellable, error))
    return -1;
  return count;
}

static gboolean
ostree_content_writer_close (GOutputStream        *stream,
                             GCancellable         *cancellable,
                             GError              **error)
{
  /* We don't expect people to invoke close() - they need to call finish()
   * to get the checksum.  We'll clean up in finalize anyways if need be.
   */
  return TRUE;
}

/**
 * ostree_content_writer_finish:
 * @self: Writer
 * @cancellable: Cancellable
 * @error: Error
 *
 * Complete the object write and return the checksum.
 * Returns: (transfer full): Checksum, or %NULL on error
 */
char *
ostree_content_writer_finish (OstreeContentWriter  *self,
                              GCancellable         *cancellable,
                              GError              **error)
{
  char actual_checksum[OSTREE_SHA256_STRING_LEN+1];
  if (!_ostree_repo_bare_content_commit (self->repo, &self->output, actual_checksum,
                                         sizeof (actual_checksum), cancellable, error))
    return NULL;

  return g_strdup (actual_checksum);
}
