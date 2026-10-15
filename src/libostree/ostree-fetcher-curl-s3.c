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

/* s3:// support for the curl fetcher: credentials, signing and S3 error
 * responses. */

#include "config.h"

#include "ostree-aws.h"
#include "ostree-fetcher-curl-private.h"
#include "ostree-fetcher-s3.h"
#include "ostree-fetcher-util.h"
#include "otutil.h"

/* How much of a non-2xx S3 response body to keep for error reporting. */
#define S3_ERROR_BODY_MAX (64 * 1024)
/* Signing region for ?endpoint= remotes that resolve none; S3-compatible
 * servers generally accept any. */
#define S3_ENDPOINT_FALLBACK_REGION "us-east-1"

/* One s3:// base URI as configured on a remote. */
typedef struct
{
  OstreeS3Location *loc;
  char *region;    /* resolved signing region */
  char *creds_key; /* key into OstreeFetcherS3.creds */
} S3Remote;

/* Credentials for one (profile, region), shared by every request using it. */
typedef struct
{
  OstreeAwsCredentials *creds; /* NULL until fetched, or after being dropped as expired */
  guint64 fetched_at;          /* wall-clock seconds */
  gboolean fetching;           /* a fetch is in flight */
  gboolean force_next;         /* the next fetch bypasses the process-wide cache */
  GQueue waiting;              /* GTask*, borrowed: outstanding_requests owns them */
} S3CredState;

struct OstreeFetcherS3
{
  GHashTable *remotes; /* base URI string -> S3Remote* */
  GHashTable *creds;   /* "profile\nregion" -> S3CredState* */
};

struct OstreeS3Request
{
  /* The current attempt. */
  GString *error_body;         /* body of a non-2xx response, capped */
  char *bucket_region;         /* x-amz-bucket-region response header */
  const char *creds_key;       /* owned by the S3Remote */
  OstreeAwsCredentials *creds; /* what the attempt was signed with */

  gboolean retried_expired; /* already retried once after an expired token */
};

static void
s3_remote_free (S3Remote *remote)
{
  _ostree_s3_location_free (remote->loc);
  g_free (remote->region);
  g_free (remote->creds_key);
  g_free (remote);
}

static void
s3_cred_state_free (S3CredState *cs)
{
  g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
  g_queue_clear (&cs->waiting);
  g_free (cs);
}

OstreeFetcherS3 *
_ostree_fetcher_s3_new (void)
{
  OstreeFetcherS3 *s3 = g_new0 (OstreeFetcherS3, 1);
  s3->remotes
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)s3_remote_free);
  s3->creds
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)s3_cred_state_free);
  return s3;
}

void
_ostree_fetcher_s3_free (OstreeFetcherS3 *s3)
{
  g_hash_table_unref (s3->remotes);
  g_hash_table_unref (s3->creds);
  g_free (s3);
}

void
_ostree_s3_request_free (OstreeS3Request *s3)
{
  if (s3 == NULL)
    return;
  if (s3->error_body)
    g_string_free (s3->error_body, TRUE);
  g_free (s3->bucket_region);
  g_clear_pointer (&s3->creds, _ostree_aws_credentials_unref);
  g_free (s3);
}

static guint64
s3_now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* Parse @baseuri and resolve its region, once per fetcher. */
static S3Remote *
s3_get_remote (OstreeFetcher *self, GUri *baseuri, GError **error)
{
  g_autofree char *key = g_uri_to_string_partial (baseuri, G_URI_HIDE_PASSWORD);
  S3Remote *remote = g_hash_table_lookup (self->s3->remotes, key);
  if (remote != NULL)
    return remote;

  g_autoptr (OstreeS3Location) loc = _ostree_s3_location_parse (baseuri, error);
  if (loc == NULL)
    return NULL;

  g_autoptr (GError) region_error = NULL;
  /* An ?endpoint= remote is not AWS, so the EC2 metadata service has
   * nothing to say about its region and is not worth blocking on. */
  g_autofree char *region = _ostree_aws_resolve_region (loc->region, loc->profile,
                                                        loc->endpoint == NULL, &region_error);
  if (region == NULL)
    {
      if (loc->endpoint == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&region_error));
          return NULL;
        }
      region = g_strdup (S3_ENDPOINT_FALLBACK_REGION);
    }

  remote = g_new0 (S3Remote, 1);
  remote->creds_key = g_strdup_printf ("%s\n%s", loc->profile ?: "", region);
  remote->region = g_steal_pointer (&region);
  remote->loc = g_steal_pointer (&loc);
  g_hash_table_insert (self->s3->remotes, g_steal_pointer (&key), remote);
  return remote;
}

typedef struct
{
  OstreeFetcher *self; /* owned */
  char *creds_key;
} S3CredsFetch;

static void
s3_on_credentials (GObject *source, GAsyncResult *result, gpointer user_data)
{
  S3CredsFetch *fetch = user_data;
  OstreeFetcher *self = fetch->self;
  S3CredState *cs = g_hash_table_lookup (self->s3->creds, fetch->creds_key);
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_get_credentials_finish (result, &error);
  const guint64 now = s3_now ();

  g_assert (cs != NULL);
  cs->fetching = FALSE;

  /* Credentials that are already expired would only be refetched again
   * immediately; turn that into an error instead of a loop. */
  if (creds != NULL && _ostree_aws_credentials_get_expiry (creds) <= now)
    {
      g_clear_pointer (&creds, _ostree_aws_credentials_unref);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   "AWS credential provider returned credentials that have already expired");
    }
  if (creds != NULL)
    {
      g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
      cs->creds = g_steal_pointer (&creds);
      cs->fetched_at = now;
    }

  /* Take the list first: resuming a request can park it again. */
  GQueue waiting = cs->waiting;
  g_queue_init (&cs->waiting);
  GTask *task;
  while ((task = g_queue_pop_head (&waiting)) != NULL)
    {
      if (error == NULL)
        _ostree_fetcher_request_initiate (g_task_get_task_data (task), task);
      else
        _ostree_fetcher_request_fail_later (self, task, g_error_copy (error));
    }

  g_object_unref (fetch->self);
  g_free (fetch->creds_key);
  g_free (fetch);
}

static void
s3_start_credentials_fetch (OstreeFetcher *self, S3Remote *remote, S3CredState *cs)
{
  S3CredsFetch *fetch = g_new0 (S3CredsFetch, 1);
  fetch->self = g_object_ref (self);
  fetch->creds_key = g_strdup (remote->creds_key);

  /* Replacing credentials we already hold means they are stale; the
   * process-wide cache would just hand the same ones back. */
  const gboolean force = cs->force_next || cs->creds != NULL;
  cs->force_next = FALSE;
  cs->fetching = TRUE;

  /* The result is delivered on the thread-default context; make it ours. */
  g_main_context_push_thread_default (self->mainctx);
  _ostree_aws_get_credentials_async (remote->loc->profile, remote->region, force, NULL,
                                     s3_on_credentials, fetch);
  g_main_context_pop_thread_default (self->mainctx);
}

/* Sign one attempt at an s3:// mirror.
 *
 * On success either *out_parked is TRUE (the request waits for
 * credentials and will be re-initiated), or *out_url is the URL to
 * request and the signed headers have been appended to req->req_headers.
 */
static gboolean
s3_sign_request (FetcherRequest *req, GTask *task, GUri *baseuri, char **out_url,
                 gboolean *out_parked, GError **error)
{
  OstreeFetcher *self = req->fetcher;
  S3Remote *remote = s3_get_remote (self, baseuri, error);
  if (remote == NULL)
    return FALSE;

  S3CredState *cs = g_hash_table_lookup (self->s3->creds, remote->creds_key);
  if (cs == NULL)
    {
      cs = g_new0 (S3CredState, 1);
      g_queue_init (&cs->waiting);
      g_hash_table_insert (self->s3->creds, g_strdup (remote->creds_key), cs);
    }

  if (cs->creds == NULL
      || !_ostree_s3_credentials_usable (_ostree_aws_credentials_get_expiry (cs->creds),
                                         cs->fetched_at, s3_now ()))
    {
      g_queue_push_tail (&cs->waiting, task);
      if (!cs->fetching)
        s3_start_credentials_fetch (self, remote, cs);
      *out_parked = TRUE;
      return TRUE;
    }

  g_autofree char *url
      = _ostree_s3_location_to_request_url (remote->loc, remote->region, req->filename);
  g_autoptr (GPtrArray) headers = g_ptr_array_new_with_free_func (g_free);
  if (!_ostree_aws_sign_get (cs->creds, url, remote->region, headers, error))
    return FALSE;
  for (guint i = 0; i < headers->len; i++)
    req->req_headers = curl_slist_append (req->req_headers, headers->pdata[i]);

  req->s3->creds_key = remote->creds_key;
  req->s3->creds = _ostree_aws_credentials_ref (cs->creds);
  *out_url = g_steal_pointer (&url);
  return TRUE;
}

gboolean
_ostree_fetcher_s3_begin_attempt (FetcherRequest *req, GTask *task, GUri *baseuri, char **out_url)
{
  g_autoptr (GError) error = NULL;
  gboolean parked = FALSE;

  if (req->s3 == NULL)
    req->s3 = g_new0 (OstreeS3Request, 1);
  OstreeS3Request *s3 = req->s3;

  /* Every attempt is signed afresh; never resend a stale signature. */
  g_clear_pointer (&s3->creds, _ostree_aws_credentials_unref);
  g_clear_pointer (&s3->bucket_region, g_free);
  if (s3->error_body)
    g_string_truncate (s3->error_body, 0);

  if (!s3_sign_request (req, task, baseuri, out_url, &parked, &error))
    {
      _ostree_fetcher_request_fail_later (req->fetcher, task, g_steal_pointer (&error));
      return FALSE;
    }
  return !parked;
}

void
_ostree_fetcher_s3_complete_error (OstreeFetcher *self, FetcherRequest *req, GTask *task,
                                   const char *eff_url, long response, gboolean retry_all,
                                   gboolean *out_reinitiate)
{
  OstreeS3Request *s3 = req->s3;
  g_autofree char *code = NULL;
  g_autofree char *message = NULL;
  g_autofree char *body_region = NULL;
  if (s3->error_body != NULL)
    (void)_ostree_s3_parse_error_body (s3->error_body->str, s3->error_body->len, &code, &message,
                                       &body_region);

  /* S3 reports ExpiredToken with HTTP 400 and RequestExpired with 403, so
   * go by the error code, not the status. */
  const gboolean expired
      = code != NULL
        && (g_str_equal (code, "ExpiredToken") || g_str_equal (code, "TokenRefreshRequired")
            || g_str_equal (code, "RequestExpired"));
  if (expired && !s3->retried_expired)
    {
      S3CredState *cs = g_hash_table_lookup (self->s3->creds, s3->creds_key);
      /* Only drop the credentials this request was signed with: when many
       * requests fail together, the first one may already have replaced them. */
      if (cs != NULL && cs->creds == s3->creds)
        {
          g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
          cs->force_next = TRUE;
        }
      s3->retried_expired = TRUE;
      *out_reinitiate = TRUE;
      return;
    }

  if (req->idx + 1 < req->mirrorlist->len)
    {
      req->idx++;
      *out_reinitiate = TRUE;
      return;
    }

  const char *bucket_region = s3->bucket_region ?: body_region;
  const gboolean wrong_region
      = response == 301 || response == 307
        || (code != NULL && g_str_equal (code, "AuthorizationHeaderMalformed")
            && body_region != NULL);
  GIOErrorEnum giocode;
  g_autofree char *msg = NULL;

  if (wrong_region)
    {
      giocode = G_IO_ERROR_FAILED;
      if (bucket_region != NULL)
        msg = g_strdup_printf ("While fetching %s: bucket is in region %s; add ?region=%s to "
                               "the s3:// URL",
                               eff_url, bucket_region, bucket_region);
      else
        msg = g_strdup_printf ("While fetching %s: bucket is in a different region (HTTP %ld); "
                               "set ?region= on the s3:// URL",
                               eff_url, response);
    }
  else
    {
      if (response == 404)
        giocode = G_IO_ERROR_NOT_FOUND;
      else if (response == 403 || expired)
        giocode = G_IO_ERROR_PERMISSION_DENIED;
      else
        giocode = _ostree_fetcher_http_status_code_to_io_error (response, retry_all);
      /* The code tells apart what the status does not: NoSuchKey from
       * NoSuchBucket, AccessDenied from SignatureDoesNotMatch. */
      if (code != NULL)
        msg = g_strdup_printf ("While fetching %s: S3 returned HTTP %ld %s: %s", eff_url, response,
                               code, message ?: "(no message)");
      else
        msg = g_strdup_printf ("While fetching %s: Server returned HTTP %ld", eff_url, response);
    }

  g_task_return_new_error (task, G_IO_ERROR, giocode, "%s", msg);
  if (self->remote_name
      && !((req->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0
           && giocode == G_IO_ERROR_NOT_FOUND))
    _ostree_fetcher_journal_failure (self->remote_name, eff_url, msg);
}

gboolean
_ostree_fetcher_s3_write (FetcherRequest *req, GTask *task, const void *ptr, size_t realsize,
                          size_t *out_ret)
{
  OstreeS3Request *s3 = req->s3;
  long response = 0;
  (void)curl_easy_getinfo (req->easy, CURLINFO_RESPONSE_CODE, &response);

  if (!(response >= 200 && response < 300))
    {
      /* An error document, not content: keep a bounded copy for the
       * error message and never let it reach the output. */
      if (s3->error_body == NULL)
        s3->error_body = g_string_new (NULL);
      if (s3->error_body->len < S3_ERROR_BODY_MAX)
        g_string_append_len (s3->error_body, ptr,
                             MIN (realsize, S3_ERROR_BODY_MAX - s3->error_body->len));
      *out_ret = realsize;
      return TRUE;
    }

  return FALSE;
}

void
_ostree_fetcher_s3_header (FetcherRequest *req, const char *line)
{
  OstreeS3Request *s3 = req->s3;
  const char *bucket_region_header = "x-amz-bucket-region:";

  if (g_ascii_strncasecmp (line, bucket_region_header, strlen (bucket_region_header)) == 0)
    {
      g_free (s3->bucket_region);
      s3->bucket_region = g_strdup (g_strstrip ((char *)line + strlen (bucket_region_header)));
    }
}
