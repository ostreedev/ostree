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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Refresh credentials once they are this close to expiring. */
#define OSTREE_S3_CREDS_REFRESH_WINDOW_SECS 300
/* Never refetch credentials more often than this, even if they are close to expiring. */
#define OSTREE_S3_CREDS_MIN_REFETCH_INTERVAL_SECS 60

typedef struct
{
  char *bucket;
  char *key_prefix; /* decoded, no leading or trailing slash; may be "" */
  char *region;     /* from ?region=, or NULL */
  char *profile;    /* from ?profile=, or NULL */
  char *endpoint;   /* from ?endpoint=, normalized to scheme://host[:port], or NULL */
} OstreeS3Location;

void _ostree_s3_location_free (OstreeS3Location *loc);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeS3Location, _ostree_s3_location_free)

/* Whether @region (which may be NULL) is safe to put in a hostname. */
gboolean _ostree_s3_region_is_valid (const char *region);

/* Parse and validate an s3:// URI. */
OstreeS3Location *_ostree_s3_location_parse (GUri *uri, GError **error);

/* Build the URL to request for @subpath (may be NULL) below the location.
 * @region must be non-NULL unless loc->endpoint is set. */
char *_ostree_s3_location_to_request_url (const OstreeS3Location *loc, const char *region,
                                          const char *subpath);

/* Extract fields from an S3 XML error body. Any out param may come back
 * NULL when the element is absent. Returns FALSE if @body is not an S3
 * error document. */
gboolean _ostree_s3_parse_error_body (const char *body, gsize len, char **out_code,
                                      char **out_message, char **out_region);

/* Parse a "Content-Range: bytes START-END/TOTAL" header value. Returns
 * FALSE, leaving the outputs untouched, unless all three are present and
 * consistent (START <= END < TOTAL). */
gboolean _ostree_s3_parse_content_range (const char *value, guint64 *out_start, guint64 *out_end,
                                         guint64 *out_total);

/* Whether credentials expiring at @expiry (seconds since the epoch,
 * G_MAXUINT64 for never) and obtained at @fetched_at may be used at @now. */
gboolean _ostree_s3_credentials_usable (guint64 expiry, guint64 fetched_at, guint64 now);

G_END_DECLS
