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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-fetcher-s3.h"

#include <string.h>

void
_ostree_s3_location_free (OstreeS3Location *loc)
{
  if (!loc)
    return;
  g_free (loc->bucket);
  g_free (loc->key_prefix);
  g_free (loc->region);
  g_free (loc->profile);
  g_free (loc->endpoint);
  g_free (loc);
}

/* Set an "Invalid s3:// URL" error and return NULL. */
static gpointer G_GNUC_PRINTF (3, 4)
    throw_invalid (GError **error, const char *uri, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  g_autofree char *reason = g_strdup_vprintf (format, args);
  va_end (args);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid s3:// URL %s: %s", uri,
               reason);
  return NULL;
}

/* Non-empty and only a-z, 0-9 and '-'. */
static gboolean
is_lower_alnum_dash (const char *s)
{
  if (*s == '\0')
    return FALSE;
  for (const char *p = s; *p; p++)
    {
      if (!(g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '-'))
        return FALSE;
    }
  return TRUE;
}

gboolean
_ostree_s3_region_is_valid (const char *region)
{
  return region != NULL && is_lower_alnum_dash (region);
}

/* Whether @bucket can be used as a single TLS-matchable DNS label, i.e.
 * with virtual-hosted addressing. Dots break wildcard certificate matching;
 * uppercase and underscores exist in legacy buckets and are not valid labels. */
static gboolean
bucket_is_dns_safe (const char *bucket)
{
  const gsize len = strlen (bucket);
  return len >= 3 && len <= 63 && is_lower_alnum_dash (bucket);
}

/* Returns scheme://host[:port], or NULL if @value is not a bare http(s) origin. */
static char *
normalize_endpoint (const char *value)
{
  g_autoptr (GUri) u = g_uri_parse (value, G_URI_FLAGS_NONE, NULL);
  if (!u)
    return NULL;
  const char *scheme = g_uri_get_scheme (u);
  const char *host = g_uri_get_host (u);
  const char *path = g_uri_get_path (u);
  if (!(g_str_equal (scheme, "http") || g_str_equal (scheme, "https")))
    return NULL;
  if (host == NULL || *host == '\0')
    return NULL;
  if (g_uri_get_userinfo (u) != NULL || g_uri_get_query (u) != NULL
      || g_uri_get_fragment (u) != NULL)
    return NULL;
  if (!(*path == '\0' || g_str_equal (path, "/")))
    return NULL;
  /* GUri returns IPv6 literals without their brackets */
  g_autofree char *host_part
      = strchr (host, ':') != NULL ? g_strdup_printf ("[%s]", host) : g_strdup (host);
  if (g_uri_get_port (u) != -1)
    return g_strdup_printf ("%s://%s:%d", scheme, host_part, g_uri_get_port (u));
  return g_strdup_printf ("%s://%s", scheme, host_part);
}

OstreeS3Location *
_ostree_s3_location_parse (GUri *uri, GError **error)
{
  g_autofree char *uri_str = g_uri_to_string_partial (uri, G_URI_HIDE_PASSWORD);

  if (!g_str_equal (g_uri_get_scheme (uri), "s3"))
    return throw_invalid (error, uri_str, "scheme is not s3");

  const char *host = g_uri_get_host (uri);
  if (host == NULL || *host == '\0' || strchr (host, '%') != NULL)
    return throw_invalid (error, uri_str, "missing or malformed bucket name");
  if (g_uri_get_userinfo (uri) != NULL || g_uri_get_user (uri) != NULL)
    return throw_invalid (error, uri_str,
                          "credentials are not allowed in the URL; they come from the AWS "
                          "credential chain");
  if (g_uri_get_port (uri) != -1)
    return throw_invalid (error, uri_str, "a port is not allowed; use ?endpoint= instead");
  if (g_uri_get_fragment (uri) != NULL)
    return throw_invalid (error, uri_str, "a fragment is not allowed");

  g_autoptr (OstreeS3Location) loc = g_new0 (OstreeS3Location, 1);
  loc->bucket = g_strdup (host);

  /* An encoded slash would be decoded into a path separator. */
  if (strstr (g_uri_get_path (uri), "%2F") != NULL || strstr (g_uri_get_path (uri), "%2f") != NULL)
    return throw_invalid (error, uri_str, "an encoded '/' (%%2F) is not allowed in the path");
  g_autofree char *path = g_uri_unescape_string (g_uri_get_path (uri), NULL);
  if (path == NULL)
    return throw_invalid (error, uri_str, "invalid percent-encoding in path");
  const char *start = path;
  while (*start == '/')
    start++;
  gsize len = strlen (start);
  while (len > 0 && start[len - 1] == '/')
    len--;
  loc->key_prefix = g_strndup (start, len);

  const char *query = g_uri_get_query (uri);
  if (query != NULL && *query != '\0')
    {
      GUriParamsIter iter;
      char *attr_raw = NULL;
      char *value_raw = NULL;
      g_autoptr (GError) local_error = NULL;

      g_uri_params_iter_init (&iter, query, -1, "&", G_URI_PARAMS_NONE);
      while (g_uri_params_iter_next (&iter, &attr_raw, &value_raw, &local_error))
        {
          g_autofree char *attr = attr_raw;
          g_autofree char *value = value_raw;
          char **slot = NULL;

          if (g_str_equal (attr, "region"))
            slot = &loc->region;
          else if (g_str_equal (attr, "profile"))
            slot = &loc->profile;
          else if (g_str_equal (attr, "endpoint"))
            slot = &loc->endpoint;
          else
            return throw_invalid (error, uri_str,
                                  "Unknown parameter '%s' (allowed: region, profile, endpoint)",
                                  attr);
          if (*slot != NULL)
            return throw_invalid (error, uri_str, "parameter '%s' given more than once", attr);
          if (*value == '\0')
            return throw_invalid (error, uri_str, "parameter '%s' is empty", attr);
          *slot = g_steal_pointer (&value);
        }
      if (local_error != NULL)
        return throw_invalid (error, uri_str, "%s", local_error->message);
    }

  if (loc->region != NULL && !_ostree_s3_region_is_valid (loc->region))
    return throw_invalid (error, uri_str, "region may only contain a-z, 0-9 and '-'");

  if (loc->endpoint != NULL)
    {
      g_autofree char *normalized = normalize_endpoint (loc->endpoint);
      if (normalized == NULL)
        return throw_invalid (error, uri_str, "endpoint must look like http(s)://host[:port]");
      g_free (loc->endpoint);
      loc->endpoint = g_steal_pointer (&normalized);
    }

  return g_steal_pointer (&loc);
}

/* Append each non-empty '/'-separated segment of @path to @out as
 * "/<segment>", percent-encoding everything outside the SigV4 unreserved
 * set (A-Z a-z 0-9 - . _ ~) exactly once. */
static void
append_escaped_segments (GString *out, const char *path)
{
  if (path == NULL)
    return;
  g_auto (GStrv) segments = g_strsplit (path, "/", -1);
  for (char **seg = segments; *seg != NULL; seg++)
    {
      if (**seg == '\0')
        continue;
      g_autofree char *escaped = g_uri_escape_string (*seg, NULL, FALSE);
      g_string_append_c (out, '/');
      g_string_append (out, escaped);
    }
}

char *
_ostree_s3_location_to_request_url (const OstreeS3Location *loc, const char *region,
                                    const char *subpath)
{
  g_autoptr (GString) url = g_string_new (NULL);
  gboolean path_style;

  if (loc->endpoint != NULL)
    {
      g_string_append (url, loc->endpoint);
      path_style = TRUE;
    }
  else
    {
      g_assert (region != NULL);
      path_style = !bucket_is_dns_safe (loc->bucket);
      if (path_style)
        g_string_append_printf (url, "https://s3.%s.amazonaws.com", region);
      else
        g_string_append_printf (url, "https://%s.s3.%s.amazonaws.com", loc->bucket, region);
    }

  const gsize origin_len = url->len;
  if (path_style)
    append_escaped_segments (url, loc->bucket);
  append_escaped_segments (url, loc->key_prefix);
  append_escaped_segments (url, subpath);
  if (url->len == origin_len)
    g_string_append_c (url, '/');

  return g_string_free (g_steal_pointer (&url), FALSE);
}

typedef struct
{
  int depth;
  gboolean is_error_doc;
  GString *text;
  char *code;
  char *message;
  char *region;
} S3ErrorParse;

static void
s3_error_start_element (GMarkupParseContext *context, const char *element_name,
                        const char **attribute_names, const char **attribute_values,
                        gpointer user_data, GError **error)
{
  S3ErrorParse *parse = user_data;
  parse->depth++;
  if (parse->depth == 1)
    parse->is_error_doc = g_str_equal (element_name, "Error");
  g_string_truncate (parse->text, 0);
}

static void
s3_error_text (GMarkupParseContext *context, const char *text, gsize text_len, gpointer user_data,
               GError **error)
{
  S3ErrorParse *parse = user_data;
  g_string_append_len (parse->text, text, text_len);
}

static void
s3_error_end_element (GMarkupParseContext *context, const char *element_name, gpointer user_data,
                      GError **error)
{
  S3ErrorParse *parse = user_data;
  if (parse->is_error_doc && parse->depth == 2)
    {
      char **slot = NULL;
      if (g_str_equal (element_name, "Code"))
        slot = &parse->code;
      else if (g_str_equal (element_name, "Message"))
        slot = &parse->message;
      else if (g_str_equal (element_name, "Region"))
        slot = &parse->region;
      if (slot != NULL && *slot == NULL)
        *slot = g_strdup (parse->text->str);
    }
  parse->depth--;
}

gboolean
_ostree_s3_parse_error_body (const char *body, gsize len, char **out_code, char **out_message,
                             char **out_region)
{
  static const GMarkupParser parser
      = { s3_error_start_element, s3_error_end_element, s3_error_text, NULL, NULL };
  S3ErrorParse parse = { 0 };
  parse.text = g_string_new (NULL);
  g_autoptr (GMarkupParseContext) context = g_markup_parse_context_new (&parser, 0, &parse, NULL);

  gboolean ok = g_markup_parse_context_parse (context, body, len, NULL)
                && g_markup_parse_context_end_parse (context, NULL) && parse.is_error_doc;
  g_string_free (parse.text, TRUE);

  if (ok)
    {
      if (out_code)
        *out_code = g_steal_pointer (&parse.code);
      if (out_message)
        *out_message = g_steal_pointer (&parse.message);
      if (out_region)
        *out_region = g_steal_pointer (&parse.region);
    }
  g_free (parse.code);
  g_free (parse.message);
  g_free (parse.region);
  return ok;
}

gboolean
_ostree_s3_credentials_usable (guint64 expiry, guint64 fetched_at, guint64 now)
{
  if (expiry == G_MAXUINT64)
    return TRUE;
  if (expiry <= now)
    return FALSE;
  if (expiry - now > OSTREE_S3_CREDS_REFRESH_WINDOW_SECS)
    return TRUE;
  /* Close to expiry. If we only just fetched them, the provider has nothing
   * fresher to give, so use them rather than refetching in a loop. */
  return now >= fetched_at && now - fetched_at < OSTREE_S3_CREDS_MIN_REFETCH_INTERVAL_SECS;
}
