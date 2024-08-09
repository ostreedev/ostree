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

/* This implements a simple parser of the PEM format defined in RFC
 * 7468, which doesn't allow headers to be encoded alongside the data
 * (unlike the legacy RFC 1421).
 */

#include "config.h"

#include "ostree-blob-reader-pem.h"
#include "ostree-blob-reader-private.h"
#include <string.h>

enum
{
  PROP_0,
  PROP_LABEL
};

struct _OstreeBlobReaderPem
{
  GDataInputStream parent_instance;

  gchar *label;
};

static void ostree_blob_reader_pem_iface_init (OstreeBlobReaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBlobReaderPem, _ostree_blob_reader_pem, G_TYPE_DATA_INPUT_STREAM,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BLOB_READER,
                                                ostree_blob_reader_pem_iface_init));

static void
ostree_blob_reader_pem_iface_init (OstreeBlobReaderInterface *iface)
{
  iface->read_blob = ostree_blob_reader_pem_read_blob;
}

static void
ostree_blob_reader_pem_set_property (GObject *object, guint prop_id, const GValue *value,
                                     GParamSpec *pspec)
{
  OstreeBlobReaderPem *self = OSTREE_BLOB_READER_PEM (object);

  switch (prop_id)
    {
    case PROP_LABEL:
      self->label = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_blob_reader_pem_get_property (GObject *object, guint prop_id, GValue *value,
                                     GParamSpec *pspec)
{
  OstreeBlobReaderPem *self = OSTREE_BLOB_READER_PEM (object);

  switch (prop_id)
    {
    case PROP_LABEL:
      g_value_set_string (value, self->label);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_blob_reader_pem_finalize (GObject *object)
{
  OstreeBlobReaderPem *self = OSTREE_BLOB_READER_PEM (object);

  g_free (self->label);

  G_OBJECT_CLASS (_ostree_blob_reader_pem_parent_class)->finalize (object);
}

static void
_ostree_blob_reader_pem_class_init (OstreeBlobReaderPemClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = ostree_blob_reader_pem_set_property;
  gobject_class->get_property = ostree_blob_reader_pem_get_property;
  gobject_class->finalize = ostree_blob_reader_pem_finalize;

  /*
   * OstreeBlobReaderPem:label:
   *
   * The label to filter the PEM blocks.
   */
  g_object_class_install_property (
      gobject_class, PROP_LABEL,
      g_param_spec_string ("label", "", "", "", G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
_ostree_blob_reader_pem_init (OstreeBlobReaderPem *self)
{
}

OstreeBlobReaderPem *
_ostree_blob_reader_pem_new (GInputStream *base, const gchar *label)
{
  g_assert (G_IS_INPUT_STREAM (base));

  return g_object_new (OSTREE_TYPE_BLOB_READER_PEM, "base-stream", base, "label", label, NULL);
}

enum PemInputState
{
  PEM_INPUT_STATE_OUTER,
  PEM_INPUT_STATE_INNER
};

#define PEM_SUFFIX "-----"
#define PEM_PREFIX_BEGIN "-----BEGIN "
#define PEM_PREFIX_END "-----END "

GBytes *
_ostree_read_pem_block (GDataInputStream *stream, gchar **label, GCancellable *cancellable,
                        GError **error)
{
  enum PemInputState state = PEM_INPUT_STATE_OUTER;
  g_autofree gchar *tmp_label = NULL;
  g_autoptr (GString) buf = g_string_new ("");

  while (TRUE)
    {
      gsize length;
      g_autofree gchar *line = g_data_input_stream_read_line (stream, &length, cancellable, error);
      if (!line)
        break;

      line = g_strstrip (line);
      if (*line == '\0')
        continue;

      switch (state)
        {
        case PEM_INPUT_STATE_OUTER:
          if (g_str_has_prefix (line, PEM_PREFIX_BEGIN) && g_str_has_suffix (line, PEM_SUFFIX))
            {
              const gchar *start = line + sizeof (PEM_PREFIX_BEGIN) - 1;
              const gchar *end = g_strrstr (start + 1, PEM_SUFFIX);

              tmp_label = g_strndup (start, end - start);
              state = PEM_INPUT_STATE_INNER;
            }
          break;

        case PEM_INPUT_STATE_INNER:
          if (g_str_has_prefix (line, PEM_PREFIX_END) && g_str_has_suffix (line, PEM_SUFFIX))
            {
              const gchar *start = line + sizeof (PEM_PREFIX_END) - 1;
              const gchar *end = g_strrstr (start + 1, PEM_SUFFIX);

              if (strncmp (tmp_label, start, end - start) != 0)
                {
                  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "Unmatched PEM header");
                  return NULL;
                }

              g_base64_decode_inplace (buf->str, &buf->len);
              GBytes *result = g_bytes_new_take (buf->str, buf->len);

              /* Don't leak the trailing encoded bytes */
              explicit_bzero (buf->str + buf->len, buf->allocated_len - buf->len);
              g_string_free (buf, FALSE);
              buf = NULL;

              if (label)
                *label = g_steal_pointer (&tmp_label);

              return result;
            }
          else
            g_string_append (buf, line);
          break;

        default:
          g_assert_not_reached ();
        }
    }

  if (state != PEM_INPUT_STATE_OUTER)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "PEM trailer not found");
  return NULL;
}

GBytes *
ostree_blob_reader_pem_read_blob (OstreeBlobReader *self, GCancellable *cancellable, GError **error)
{
  OstreeBlobReaderPem *pself = OSTREE_BLOB_READER_PEM (self);

  g_autofree gchar *label = NULL;
  g_autoptr (GBytes) blob
      = _ostree_read_pem_block (G_DATA_INPUT_STREAM (self), &label, cancellable, error);
  if (blob != NULL && !g_str_equal (label, pself->label))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unexpected label \"%s\"", label);
      g_clear_pointer (&blob, g_bytes_unref);
    }
  return g_steal_pointer (&blob);
}
