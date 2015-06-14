/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

  SoupURI *uri;

  OstreeFetcher *fetcher;
  char *requested_file;
  guint64 max_size;
};

G_DEFINE_TYPE (OstreeMetalink, _ostree_metalink, G_TYPE_OBJECT)

typedef struct
{
  OstreeMetalink *metalink;

  GTask *task;
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

  char *result;

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
  GTask *task = user_data;
  OstreeMetalinkRequest *self = g_task_get_task_data (task);

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
  GTask *task = user_data;
  OstreeMetalinkRequest *self = g_task_get_task_data (task);

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
  GTask *task = user_data;
  OstreeMetalinkRequest *self = g_task_get_task_data (task);

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
        SoupURI *uri = soup_uri_new (uri_text);
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
  soup_uri_free (self->uri);

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
                      SoupURI        *uri)
{
  OstreeMetalink *self = (OstreeMetalink*)g_object_new (OSTREE_TYPE_METALINK, NULL);

  self->fetcher = g_object_ref (fetcher);
  self->requested_file = g_strdup (requested_file);
  self->max_size = max_size;
  self->uri = soup_uri_copy (uri);
 
  return self;
}

static void
try_next_url (OstreeMetalinkRequest          *self);

static gboolean
valid_hex_checksum (const char *s, gsize expected_len)
{
  gsize len = strspn (s, "01234567890abcdef");

  return len == expected_len && s[len] == '\0';
}

static void
on_fetched_url (GObject              *src,
                GAsyncResult         *res,
                gpointer              user_data)
{
  GTask *task = user_data;
  OstreeMetalinkRequest *self = g_task_get_task_data (task);
  GError *local_error = NULL;
  struct stat stbuf;
  int parent_dfd = _ostree_fetcher_get_dfd (self->metalink->fetcher);
  g_autoptr(GInputStream) instream = NULL;
  g_autofree char *result = NULL;
  GChecksum *checksum = NULL;

  result = _ostree_fetcher_request_uri_with_partial_finish ((OstreeFetcher*)src, res, &local_error);
  if (!result)
    goto out;

  if (!ot_openat_read_stream (parent_dfd, result, FALSE,
                              &instream, NULL, &local_error))
    goto out;
  
  if (fstat (g_file_descriptor_based_get_fd ((GFileDescriptorBased*)instream), &stbuf) != 0)
    {
      gs_set_error_from_errno (&local_error, errno);
      goto out;
    }

  if (stbuf.st_size != self->size)
    {
      g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Expected size is %" G_GUINT64_FORMAT " bytes but content is %" G_GUINT64_FORMAT " bytes",
                   self->size, stbuf.st_size);
      goto out;
    }
  
  if (self->verification_sha512)
    {
      const char *actual;

      checksum = g_checksum_new (G_CHECKSUM_SHA512);

      if (!ot_gio_splice_update_checksum (NULL, instream, checksum,
                                          g_task_get_cancellable (task),
                                          &local_error))
        goto out;
      
      actual = g_checksum_get_string (checksum);

      if (strcmp (self->verification_sha512, actual) != 0)
        {
          g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Expected checksum is %s but actual is %s",
                       self->verification_sha512, actual);
          goto out;
        }
    }
  else if (self->verification_sha256)
    {
      const char *actual;

      checksum = g_checksum_new (G_CHECKSUM_SHA256);

      if (!ot_gio_splice_update_checksum (NULL, instream, checksum,
                                          g_task_get_cancellable (task),
                                          &local_error))
        goto out;

      actual = g_checksum_get_string (checksum);

      if (strcmp (self->verification_sha256, actual) != 0)
        {
          g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Expected checksum is %s but actual is %s",
                       self->verification_sha256, actual);
          goto out;
        }
    }

 out:
  if (checksum)
    g_checksum_free (checksum);
  if (local_error)
    {
      g_free (self->last_metalink_error);
      self->last_metalink_error = g_strdup (local_error->message);
      g_clear_error (&local_error);

      /* And here we iterate on the next one if we hit an error */
      self->current_url_index++;
      try_next_url (self);
    }
  else
    {
      self->result = result;
      result = NULL; /* Transfer ownership */
      g_task_return_boolean (self->task, TRUE);
    }
}

static void
try_next_url (OstreeMetalinkRequest          *self)
{
  if (self->current_url_index >= self->urls->len)
    {
      g_task_return_new_error (self->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Exhausted %u metalink targets, last error: %s",
                               self->urls->len, self->last_metalink_error);
    }
  else
    {
      SoupURI *next = self->urls->pdata[self->current_url_index];
      
      _ostree_fetcher_request_uri_with_partial_async (self->metalink->fetcher, next,
                                                      self->metalink->max_size,
                                                      OSTREE_FETCHER_DEFAULT_PRIORITY,
                                                      g_task_get_cancellable (self->task),
                                                      on_fetched_url, self->task);
    }
}

static gboolean
start_target_request_phase (OstreeMetalinkRequest      *self,
                            GError                    **error)
{
  gboolean ret = FALSE;

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

  try_next_url (self);
  
  ret = TRUE;
 out:
  return ret;
}

static void
ostree_metalink_request_unref (gpointer data)
{
  OstreeMetalinkRequest  *request = data;
  g_object_unref (request->metalink);
  g_free (request->result);
  g_free (request->last_metalink_error);
  g_ptr_array_unref (request->urls);
  g_free (request);
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
  SoupURI               **out_target_uri;
  char                  **out_data;
  gboolean              success;
  GError                **error;
  GMainLoop             *loop;
} FetchMetalinkSyncData;

static gboolean
ostree_metalink_request_finish (OstreeMetalink         *self,
                                GAsyncResult           *result,
                                SoupURI               **out_target_uri,
                                char                  **out_data,
                                GError                **error)
{
  OstreeMetalinkRequest *request;

  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  request = g_task_get_task_data ((GTask*)result);

  if (g_task_propagate_boolean ((GTask*)result, error))
    {
      g_assert_cmpint (request->current_url_index, <, request->urls->len);
      *out_target_uri = request->urls->pdata[request->current_url_index];
      *out_data = g_strdup (request->result);
      return TRUE;
    }
  else
    return FALSE;
}

static void
on_metalink_fetched (GObject          *src,
                     GAsyncResult     *result,
                     gpointer          user_data)
{
  FetchMetalinkSyncData *data = user_data;

  data->success = ostree_metalink_request_finish ((OstreeMetalink*)src,
                                                  result,
                                                  data->out_target_uri,
                                                  data->out_data,
                                                  data->error);
  g_main_loop_quit (data->loop);
}

static gboolean
on_metalink_bytes_read (OstreeMetalinkRequest *self,
                        OstreeMetalinkRequest *request,
                        FetchMetalinkSyncData *sync_data,
                        GBytes *bytes,
                        GError **error)
{
  gsize len;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  if (!g_markup_parse_context_parse (self->parser, (const char*)data, len, error))
    return FALSE;

  if (!start_target_request_phase (self, error))
    return FALSE;

  return TRUE;
}

gboolean
_ostree_metalink_request_sync (OstreeMetalink        *self,
                               GMainLoop             *loop,
                               SoupURI               **out_target_uri,
                               char                  **out_data,
                               SoupURI               **fetching_sync_uri,
                               GCancellable          *cancellable,
                               GError                **error)
{
  OstreeMetalinkRequest *request = g_new0 (OstreeMetalinkRequest, 1);
  FetchMetalinkSyncData data = { 0, };
  GTask *task = g_task_new (self, cancellable, on_metalink_fetched, &data);
  GBytes *out_contents = NULL;
  gboolean ret = FALSE;

  data.out_target_uri = out_target_uri;
  data.out_data = out_data;
  data.loop = loop;
  data.error = error;
  *fetching_sync_uri = _ostree_metalink_get_uri (self);

  request->metalink = g_object_ref (self);
  request->urls = g_ptr_array_new_with_free_func ((GDestroyNotify) soup_uri_free);
  request->task = task; /* Unowned */

  request->parser = g_markup_parse_context_new (&metalink_parser, G_MARKUP_PREFIX_ERROR_POSITION, task, NULL);

  g_task_set_task_data (task, request, ostree_metalink_request_unref);

  if (! _ostree_fetcher_request_uri_to_membuf (self->fetcher,
                                               self->uri,
                                               FALSE,
                                               FALSE,
                                               &out_contents,
                                               loop,
                                               self->max_size,
                                               cancellable,
                                               error))
    goto out;

  if (! on_metalink_bytes_read (request, request, &data, out_contents, error))
    goto out;

  g_main_loop_run (data.loop);

  ret = data.success;

 out:
  return ret;
}

SoupURI *
_ostree_metalink_get_uri (OstreeMetalink        *self)
{
  return self->uri;
}
