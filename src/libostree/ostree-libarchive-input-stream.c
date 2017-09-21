/* 
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"


#include <archive.h>
#include <gio/gio.h>
#include "ostree-libarchive-input-stream.h"

enum {
  PROP_0,
  PROP_ARCHIVE
};

G_DEFINE_TYPE (OstreeLibarchiveInputStream, _ostree_libarchive_input_stream, G_TYPE_INPUT_STREAM)

struct _OstreeLibarchiveInputStreamPrivate {
  struct archive *archive;
};

static void     ostree_libarchive_input_stream_set_property (GObject              *object,
						  guint                 prop_id,
						  const GValue         *value,
						  GParamSpec           *pspec);
static void     ostree_libarchive_input_stream_get_property (GObject              *object,
						  guint                 prop_id,
						  GValue               *value,
						  GParamSpec           *pspec);
static gssize   ostree_libarchive_input_stream_read         (GInputStream         *stream,
						  void                 *buffer,
						  gsize                 count,
						  GCancellable         *cancellable,
						  GError              **error);
static gboolean ostree_libarchive_input_stream_close        (GInputStream         *stream,
						  GCancellable         *cancellable,
						  GError              **error);

static void
ostree_libarchive_input_stream_finalize (GObject *object)
{
  G_OBJECT_CLASS (_ostree_libarchive_input_stream_parent_class)->finalize (object);
}

static void
_ostree_libarchive_input_stream_class_init (OstreeLibarchiveInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (OstreeLibarchiveInputStreamPrivate));

  gobject_class->get_property = ostree_libarchive_input_stream_get_property;
  gobject_class->set_property = ostree_libarchive_input_stream_set_property;
  gobject_class->finalize = ostree_libarchive_input_stream_finalize;

  stream_class->read_fn = ostree_libarchive_input_stream_read;
  stream_class->close_fn = ostree_libarchive_input_stream_close;

  /**
   * OstreeLibarchiveInputStream:archive:
   *
   * The archive that the stream reads from.
   */
  g_object_class_install_property (gobject_class,
				   PROP_ARCHIVE,
				   g_param_spec_pointer ("archive",
							 "", "",
							 G_PARAM_READWRITE |
							 G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_STRINGS));

}

static void
ostree_libarchive_input_stream_set_property (GObject         *object,
					     guint            prop_id,
					     const GValue    *value,
					     GParamSpec      *pspec)
{
  OstreeLibarchiveInputStream *self;
  
  self = OSTREE_LIBARCHIVE_INPUT_STREAM (object);

  switch (prop_id)
    {
    case PROP_ARCHIVE:
      self->priv->archive = g_value_get_pointer (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_libarchive_input_stream_get_property (GObject    *object,
					     guint       prop_id,
					     GValue     *value,
					     GParamSpec *pspec)
{
  OstreeLibarchiveInputStream *self;

  self = OSTREE_LIBARCHIVE_INPUT_STREAM (object);

  switch (prop_id)
    {
    case PROP_ARCHIVE:
      g_value_set_pointer (value, self->priv->archive);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
_ostree_libarchive_input_stream_init (OstreeLibarchiveInputStream *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
					    OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM,
					    OstreeLibarchiveInputStreamPrivate);

}

GInputStream *
_ostree_libarchive_input_stream_new (struct archive *a)
{
  OstreeLibarchiveInputStream *stream;

  stream = g_object_new (OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM,
			 "archive", a,
			 NULL);

  return G_INPUT_STREAM (stream);
}

static gssize
ostree_libarchive_input_stream_read (GInputStream  *stream,
				     void          *buffer,
				     gsize          count,
				     GCancellable  *cancellable,
				     GError       **error)
{
  OstreeLibarchiveInputStream *self;
  gssize res = -1;

  self = OSTREE_LIBARCHIVE_INPUT_STREAM (stream);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

  res = archive_read_data (self->priv->archive, buffer, count);
  if (res < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", archive_error_string (self->priv->archive));
    }

  return res;
}

static gboolean
ostree_libarchive_input_stream_close (GInputStream  *stream,
				      GCancellable  *cancellable,
				      GError       **error)
{
  return TRUE;
}
