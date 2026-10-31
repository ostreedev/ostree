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

/* Refcounted, immutable set of AWS credentials. */
typedef struct OstreeAwsCredentials OstreeAwsCredentials;

OstreeAwsCredentials *_ostree_aws_credentials_ref (OstreeAwsCredentials *creds);
void _ostree_aws_credentials_unref (OstreeAwsCredentials *creds);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeAwsCredentials, _ostree_aws_credentials_unref)

/* Seconds since the epoch; G_MAXUINT64 for non-expiring credentials. */
guint64 _ostree_aws_credentials_get_expiry (OstreeAwsCredentials *creds);

/* Fixed credentials, bypassing the provider chain. Used by tests.
 * @session_token may be NULL. */
OstreeAwsCredentials *_ostree_aws_credentials_new_static (const char *access_key_id,
                                                          const char *secret_access_key,
                                                          const char *session_token);

/* Resolve the region: @url_region, then AWS_REGION, AWS_DEFAULT_REGION, the
 * profile's region, then (if @allow_imds) IMDS. @url_region and @profile may
 * be NULL. Blocks only for the IMDS fallback, for up to ten seconds. */
char *_ostree_aws_resolve_region (const char *url_region, const char *profile, gboolean allow_imds,
                                  GError **error);

/* Async. Completes on the thread-default main context of the caller.
 * @profile may be NULL. @force_refresh bypasses the process-wide cache. */
void _ostree_aws_get_credentials_async (const char *profile, const char *region,
                                        gboolean force_refresh, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data);
OstreeAwsCredentials *_ostree_aws_get_credentials_finish (GAsyncResult *result, GError **error);

/* Synchronous. Signs a bodyless GET for @url (already percent-encoded) and
 * appends the resulting headers, as "Name: value" strings, to @headers. */
gboolean _ostree_aws_sign_get (OstreeAwsCredentials *creds, const char *url, const char *region,
                               GPtrArray *headers, GError **error);

/* As above with an explicit timestamp (seconds since the epoch). Used by tests. */
gboolean _ostree_aws_sign_get_at (OstreeAwsCredentials *creds, const char *url, const char *region,
                                  guint64 unix_time, GPtrArray *headers, GError **error);

G_END_DECLS
