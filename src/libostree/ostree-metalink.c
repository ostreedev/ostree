/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ostree-metalink.h"
#include "ostree-fetcher-util.h"
#include <gio/gfiledescriptorbased.h>

#include "otutil.h"

typedef enum {
  OSTREE_METALINK_STATE_INITIAL,
  OSTREE_METALINK_STATE_METALINK,
  OSTREE_METALINK_STATE_FILES,
  OSTREE_METALINK_STATE_FILE,
  OSTREE_METALINK_STATE_SIZE,
  OSTREE_METALINK_STATE_VERIFICATION,
  OSTREE_METALINK_STATE_HASH,
  OSTREE_METALINK_STATE_RESOURCES,
  OSTREE_METALINK_STATE_URL,

  OSTREE_METALINK_STATE_PASSTHROUGH /* Ignoring unknown elements */
} OstreeMetalinkState;

struct OstreeMetalink
{
  GObject parent_instance;

  OstreeFetcherURI *uri;

  OstreeFetcher *fetcher;
  char *requested_file;
  guint64 max_size;
};

G_DEFINE_TYPE (OstreeMetalink, _ostree_metalink, G_TYPE_OBJECT)

typedef struct
{
  OstreeMetalink *metalink;

  GCancellable *cancellable;
  GMarkupParseContext *parser;

  guint passthrough_depth;
  OstreeMetalinkState passthrough_previous;
  
  guint found_a_file_element : 1;
  guint found_our_file_element : 1;
  guint verification_known : 1;

  GChecksumType in_verification_type;

  guint64 size;
  char *verification_sha256;
  char *verification_sha512;

  GBytes *result;

  char *last_metalink_error;
  guint current_url_index;
  GPtrArray *urls;

  OstreeMetalinkState state;
} OstreeMetalinkRequest;

static void
state_transition (OstreeMetalinkRequest  *self,
                  OstreeMetalinkState     new_state)
{
  g_assert (self->state != new_state);

  if (new_state == OSTREE_METALINK_STATE_PASSTHROUGH)
    self->passthrough_previous = self->state;

  self->state = new_state;
}

static void
unknown_element (OstreeMetalinkRequest         *self,
                 const char                    *element_name,
                 GError                       **error)
{
  state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
  g_assert (self->passthrough_depth == 0);
}

static void
metalink_parser_start (GMarkupParseContext  *context,
                       const gchar          *element_name,
                       const gchar         **attribute_names,
                       const gchar         **attribute_values,
                       gpointer              user_data,
                       GError              **error)
{
  OstreeMetalinkRequest *self = user_data;

  switch (self->state)
    {
    case OSTREE_METALINK_STATE_INITIAL:
      if (strcmp (element_name, "metalink") == 0)
        state_transition (self, OSTREE_METALINK_STATE_METALINK);
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_METALINK:
      if (strcmp (element_name, "files") == 0)
        state_transition (self, OSTREE_METALINK_STATE_FILES);
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_FILES:
      /* If we've already processed a <file> element we're OK with, just
       * ignore the others.
       */
      if (self->urls->len > 0)
        {
          state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
        }
      else if (strcmp (element_name, "file") == 0)
        {
          const char *file_name;

          if (!g_markup_collect_attributes (element_name,
                                            attribute_names,
                                            attribute_values,
                                            error,
                                            G_MARKUP_COLLECT_STRING,
                                            "name",
                                            &file_name,
                                            G_MARKUP_COLLECT_INVALID))
            goto out;

          self->found_a_file_element = TRUE;

          if (strcmp (file_name, self->metalink->requested_file) != 0)
            {
              state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
              g_assert (self->passthrough_depth == 0);
            }
          else
            {
              self->found_our_file_element = TRUE;
              state_transition (self, OSTREE_METALINK_STATE_FILE);
            }
        }
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_FILE:
      if (strcmp (element_name, "size") == 0)
        state_transition (self, OSTREE_METALINK_STATE_SIZE);
      else if (strcmp (element_name, "verification") == 0)
        state_transition (self, OSTREE_METALINK_STATE_VERIFICATION);
      else if (strcmp (element_name, "resources") == 0)
        state_transition (self, OSTREE_METALINK_STATE_RESOURCES);
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_SIZE:
      unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_VERIFICATION:
      if (strcmp (element_name, "hash") == 0)
        {
           char *verification_type_str = NULL;

          state_transition (self, OSTREE_METALINK_STATE_HASH);
          if (!g_markup_collect_attributes (element_name,
                                            attribute_names,
                                            attribute_values,
                                            error,
                                            G_MARKUP_COLLECT_STRING,
                                            "type",
                                            &verification_type_str,
                                            G_MARKUP_COLLECT_INVALID))
            goto out;

          /* Only accept sha256/sha512 */
          self->verification_known = TRUE;
          if (strcmp (verification_type_str, "sha256") == 0)
            self->in_verification_type = G_CHECKSUM_SHA256;
          else if (strcmp (verification_type_str, "sha512") == 0)
            self->in_verification_type = G_CHECKSUM_SHA512;
          else
            self->verification_known = FALSE;
        }
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_HASH:
      unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_RESOURCES:
      if (self->size == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No <size> element found or it is zero");
          goto out;
        }
      if (!self->verification_known)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No <verification> element with known <hash type=> found");
          goto out;
        }

      if (strcmp (element_name, "url") == 0)
        {
          const char *protocol;

          if (!g_markup_collect_attributes (element_name,
                                            attribute_names,
                                            attribute_values,
                                            error,
                                            G_MARKUP_COLLECT_STRING,
                                            "protocol",
                                            &protocol,
                                            G_MARKUP_COLLECT_STRING,
                                            "type",
                                            NULL,
                                            G_MARKUP_COLLECT_STRING,
                                            "location",
                                            NULL,
                                            G_MARKUP_COLLECT_STRING,
                                            "preference",
                                            NULL,
                                            G_MARKUP_COLLECT_INVALID))
            goto out;

          /* Ignore non-HTTP resources */
          if (!(strcmp (protocol, "http") == 0 || strcmp (protocol, "https") == 0))
            state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
          else
            state_transition (self, OSTREE_METALINK_STATE_URL);
        }
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_URL:
      unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_PASSTHROUGH:
      self->passthrough_depth++;
      break;
    }

 out:
  return;
}

static void
metalink_parser_end (GMarkupParseContext  *context,
                     const gchar          *element_name,
                     gpointer              user_data,
                     GError              **error)
{
  OstreeMetalinkRequest *self = user_data;

  switch (self->state)
    {
    case OSTREE_METALINK_STATE_INITIAL:
      break;
    case OSTREE_METALINK_STATE_METALINK:
      state_transition (self, OSTREE_METALINK_STATE_INITIAL);
      break;
    case OSTREE_METALINK_STATE_FILES:
      state_transition (self, OSTREE_METALINK_STATE_METALINK);
      break;
    case OSTREE_METALINK_STATE_FILE:
      state_transition (self, OSTREE_METALINK_STATE_FILES);
      break;
    case OSTREE_METALINK_STATE_SIZE:
    case OSTREE_METALINK_STATE_VERIFICATION:
    case OSTREE_METALINK_STATE_RESOURCES:
      state_transition (self, OSTREE_METALINK_STATE_FILE);
      break;
    case OSTREE_METALINK_STATE_HASH:
      state_transition (self, OSTREE_METALINK_STATE_VERIFICATION);
      break;
    case OSTREE_METALINK_STATE_URL:
      state_transition (self, OSTREE_METALINK_STATE_RESOURCES);
      break;
    case OSTREE_METALINK_STATE_PASSTHROUGH:
      if (self->passthrough_depth > 0)
        self->passthrough_depth--;
      else
        state_transition (self, self->passthrough_previous);
      break;
    }
}

static void
metalink_parser_text (GMarkupParseContext *context,
                      const gchar         *text,
                      gsize                text_len,
                      gpointer             user_data,
                      GError             **error)
{
  OstreeMetalinkRequest *self = user_data;

  switch (self->state)
    {
    case OSTREE_METALINK_STATE_INITIAL:
      break;
    case OSTREE_METALINK_STATE_METALINK:
      break;
    case OSTREE_METALINK_STATE_FILES:
      break;
    case OSTREE_METALINK_STATE_FILE:
      break;
    case OSTREE_METALINK_STATE_SIZE:
      {
        g_autofree char *duped = g_strndup (text, text_len);
        self->size = g_ascii_strtoull (duped, NULL, 10);
      }
      break;
    case OSTREE_METALINK_STATE_VERIFICATION:
      break;
    case OSTREE_METALINK_STATE_HASH:
      if (self->verification_known)
        {
          switch (self->in_verification_type)
            {
            case G_CHECKSUM_SHA256:
              g_free (self->verification_sha256);
              self->verification_sha256 = g_strndup (text, text_len);
              break;
            case G_CHECKSUM_SHA512:
              g_free (self->verification_sha512);
              self->verification_sha512 = g_strndup (text, text_len);
              break;
            default:
              g_assert_not_reached ();
            }
        }
      break;
    case OSTREE_METALINK_STATE_RESOURCES:
      break;
    case OSTREE_METALINK_STATE_URL:
      {
        g_autofree char *uri_text = g_strndup (text, text_len);
        OstreeFetcherURI *uri = _ostree_fetcher_uri_parse (uri_text, NULL);
        if (uri != NULL)
          g_ptr_array_add (self->urls, uri);
      }
      break;
    case OSTREE_METALINK_STATE_PASSTHROUGH:
      break;
    }

}

static void
_ostree_metalink_finalize (GObject *object)
{
  OstreeMetalink *self;

  self = OSTREE_METALINK (object);

  g_object_unref (self->fetcher);
  g_free (self->requested_file);
  _ostree_fetcher_uri_free (self->uri);

  G_OBJECT_CLASS (_ostree_metalink_parent_class)->finalize (object);
}

static void
_ostree_metalink_class_init (OstreeMetalinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_metalink_finalize;
}

static void
_ostree_metalink_init (OstreeMetalink *self)
{
}

OstreeMetalink *
_ostree_metalink_new (OstreeFetcher  *fetcher,
                      const char     *requested_file,
                      guint64         max_size,
                      OstreeFetcherURI *uri)
{
  OstreeMetalink *self = (OstreeMetalink*)g_object_new (OSTREE_TYPE_METALINK, NULL);

  self->fetcher = g_object_ref (fetcher);
  self->requested_file = g_strdup (requested_file);
  self->max_size = max_size;
  self->uri = _ostree_fetcher_uri_clone (uri);

  return self;
}

static gboolean
valid_hex_checksum (const char *s, gsize expected_len)
{
  gsize len = strspn (s, "01234567890abcdef");

  return len == expected_len && s[len] == '\0';
}

static gboolean
try_one_url (OstreeMetalinkRequest *self,
             OstreeFetcherURI *uri,
             GBytes              **out_data,
             GError         **error)
{
  gboolean ret = FALSE;
  g_autoptr(GBytes) bytes = NULL;
  gssize n_bytes;

  if (!_ostree_fetcher_request_uri_to_membuf (self->metalink->fetcher,
                                              uri, 0, &bytes,
                                              self->metalink->max_size,
                                              self->cancellable,
                                              error))
    goto out;

  n_bytes = g_bytes_get_size (bytes);
  if (n_bytes != self->size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Expected size is %" G_GUINT64_FORMAT " bytes but content is %" G_GSSIZE_FORMAT " bytes",
                   self->size, n_bytes);
      goto out;
    }

  if (self->verification_sha512)
    {
      g_autofree char *actual = NULL;

      actual = g_compute_checksum_for_bytes (G_CHECKSUM_SHA512, bytes);

      if (strcmp (self->verification_sha512, actual) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Expected checksum is %s but actual is %s",
                       self->verification_sha512, actual);
          goto out;
        }
    }
  else if (self->verification_sha256)
    {
      g_autofree char *actual = NULL;

      actual = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, bytes);

      if (strcmp (self->verification_sha256, actual) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Expected checksum is %s but actual is %s",
                       self->verification_sha256, actual);
          goto out;
        }
    }

  ret = TRUE;
  if (out_data)
    *out_data = g_steal_pointer (&bytes);
 out:
  return ret;
}

static gboolean
try_metalink_targets (OstreeMetalinkRequest      *self,
                      OstreeFetcherURI          **out_target_uri,
                      GBytes                    **out_data,
                      GError                    **error)
{
  gboolean ret = FALSE;
  OstreeFetcherURI *target_uri = NULL;
  g_autoptr(GBytes) ret_data = NULL;

  if (!self->found_a_file_element)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <file> element found");
      goto out;
    }

  if (!self->found_our_file_element)
    {
      /* XXX Use NOT_FOUND here so we can distinguish not finding the
       *     requested file from other errors.  This is a bit of a hack
       *     through; metalinks should have their own error enum. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No <file name='%s'> found", self->metalink->requested_file);
      goto out;
    }

  if (!(self->verification_sha256 || self->verification_sha512))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <verification> hash for sha256 or sha512 found");
      goto out;
    }

  if (self->verification_sha256 && !valid_hex_checksum (self->verification_sha256, 64))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid hash digest for sha256");
      goto out;
    }

  if (self->verification_sha512 && !valid_hex_checksum (self->verification_sha512, 128))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid hash digest for sha512");
      goto out;
    }

  if (self->urls->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <url method='http'> elements found");
      goto out;
    }

  for (self->current_url_index = 0;
       self->current_url_index < self->urls->len;
       self->current_url_index++)
    {
      GError *temp_error = NULL;

      target_uri = self->urls->pdata[self->current_url_index];
      
      if (try_one_url (self, target_uri, &ret_data, &temp_error))
        break;
      else
        {
          g_free (self->last_metalink_error);
          self->last_metalink_error = g_strdup (temp_error->message);
          g_clear_error (&temp_error);
        }
    }

  if (self->current_url_index >= self->urls->len)
    {
      g_assert (self->last_metalink_error != NULL);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted %u metalink targets, last error: %s",
                   self->urls->len, self->last_metalink_error);
      goto out;
    }

  ret = TRUE;
  if (out_target_uri)
    *out_target_uri = _ostree_fetcher_uri_clone (target_uri);
  if (out_data)
    *out_data = g_steal_pointer (&ret_data);
 out:
  return ret;
}

static const GMarkupParser metalink_parser = {
  metalink_parser_start,
  metalink_parser_end,
  metalink_parser_text,
  NULL,
  NULL
};

typedef struct
{
  OstreeFetcherURI      **out_target_uri;
  GBytes                **out_data;
  gboolean              success;
  GError                **error;
  GMainLoop             *loop;
} FetchMetalinkSyncData;

gboolean
_ostree_metalink_request_sync (OstreeMetalink        *self,
                               OstreeFetcherURI      **out_target_uri,
                               GBytes                **out_data,
                               GCancellable          *cancellable,
                               GError                **error)
{
  gboolean ret = FALSE;
  OstreeMetalinkRequest request = { 0, };
  g_autoptr(GMainContext) mainctx = NULL;
  g_autoptr(GBytes) contents = NULL;
  gsize len;
  const guint8 *data;

  mainctx = g_main_context_new ();
  g_main_context_push_thread_default (mainctx);

  request.metalink = g_object_ref (self);
  request.urls = g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
  request.parser = g_markup_parse_context_new (&metalink_parser, G_MARKUP_PREFIX_ERROR_POSITION, &request, NULL);

  if (!_ostree_fetcher_request_uri_to_membuf (self->fetcher, self->uri, 0,
                                              &contents, self->max_size,
                                              cancellable, error))
    goto out;

  data = g_bytes_get_data (contents, &len);
  if (!g_markup_parse_context_parse (request.parser, (const char*)data, len, error))
    goto out;

  if (!try_metalink_targets (&request, out_target_uri, out_data, error))
    goto out;

  ret = TRUE;
 out:
  if (mainctx)
    g_main_context_pop_thread_default (mainctx);
  g_clear_object (&request.metalink);
  g_clear_pointer (&request.verification_sha256, g_free);
  g_clear_pointer (&request.verification_sha512, g_free);
  g_clear_pointer (&request.last_metalink_error, g_free);
  g_clear_pointer (&request.urls, g_ptr_array_unref);
  g_clear_pointer (&request.parser, g_markup_parse_context_free);
  return ret;
}
