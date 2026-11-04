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

/* s3:// support for the curl fetcher: credentials, signing, S3 error
 * responses, and fetching large objects as parallel ranged requests. */

#include "config.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

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
/* Objects larger than this are fetched as parallel ranged requests of this
 * size each. One connection to S3 is often far slower than the link as a
 * whole, so a single large object would otherwise hold up the end of a pull. */
#define S3_RANGE_PART_SIZE (4 * 1024 * 1024)

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

/* A large object is fetched in parts: its request asks for the first part
 * and then starts one child request per remaining part, each writing
 * straight into the parent's tmpfile. The parent completes once its own
 * transfer and every child have. */
typedef enum
{
  S3_RANGES_OFF,      /* this attempt fetches the object in one request */
  S3_RANGES_FIRST,    /* this attempt asked for the first part only */
  S3_RANGES_RESTART,  /* the parts could not be tied together: fetch it whole */
  S3_RANGES_SPAWNING, /* the children are about to be started, from an idle */
  S3_RANGES_RUNNING,  /* the children were started (or cancelled) */
} S3RangesState;

/* The parent's side. */
typedef struct
{
  S3RangesState state;
  gboolean disabled;   /* the server refused a range or gave no strong ETag: don't ask again */
  gboolean own_done;   /* the parent's own transfer finished; waiting for the children */
  guint64 first;       /* bytes covered by the parent's own response */
  guint64 total;       /* size of the whole object; 0 until known */
  guint n_running;     /* children not yet completed */
  GPtrArray *children; /* their tasks, until each completes (not owned) */
  GSource *spawn;      /* the idle that starts them (owned) */
  GError *error;       /* the first failure among them */
} S3Ranges;

/* A child's side: one part of the parent's object. */
typedef struct
{
  GTask *parent;      /* owned; NULL if this request is not a part */
  guint64 offset;     /* first byte to fetch */
  guint64 length;     /* number of bytes to fetch */
  guint64 written;    /* bytes written so far */
  gboolean confirmed; /* Content-Range is exactly the range asked for */
} S3Part;

struct OstreeS3Request
{
  /* The current attempt. */
  GString *error_body;         /* body of a non-2xx response, capped */
  char *bucket_region;         /* x-amz-bucket-region response header */
  const char *creds_key;       /* owned by the S3Remote */
  OstreeAwsCredentials *creds; /* what the attempt was signed with */

  gboolean retried_expired; /* already retried once after an expired token */
  S3Ranges ranges;
  S3Part part;
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
  g_clear_error (&s3->ranges.error);
  g_clear_pointer (&s3->ranges.children, g_ptr_array_unref);
  g_clear_pointer (&s3->ranges.spawn, g_source_unref);
  g_clear_object (&s3->part.parent);
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

/* Only plain downloads to a file are split: in-memory requests are small,
 * and conditional requests must see the server's real status. */
static gboolean
s3_ranges_eligible (FetcherRequest *req)
{
  return !req->is_membuf && req->s3->part.parent == NULL && !req->s3->ranges.disabled
         && req->if_none_match == NULL && req->if_modified_since == 0;
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
  if (parked)
    return FALSE;

  S3Ranges *ranges = &s3->ranges;
  ranges->state = S3_RANGES_OFF;
  ranges->first = 0;
  ranges->total = 0;
  if (s3->part.parent != NULL)
    {
      FetcherRequest *preq = g_task_get_task_data (s3->part.parent);
      g_autofree char *range
          = g_strdup_printf ("Range: bytes=%" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT,
                             s3->part.offset, s3->part.offset + s3->part.length - 1);
      req->req_headers = curl_slist_append (req->req_headers, range);
      /* Refuse to stitch together parts of two different objects. The
       * parent only starts children once it has a strong ETag. */
      g_assert (preq->out_etag != NULL);
      g_autofree char *if_match = g_strconcat ("If-Match: ", preq->out_etag, NULL);
      req->req_headers = curl_slist_append (req->req_headers, if_match);
      s3->part.written = 0;
      s3->part.confirmed = FALSE;
    }
  else if (s3_ranges_eligible (req))
    {
      ranges->state = S3_RANGES_FIRST;
      g_autofree char *range = g_strdup_printf ("Range: bytes=0-%d", S3_RANGE_PART_SIZE - 1);
      req->req_headers = curl_slist_append (req->req_headers, range);
    }
  return TRUE;
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

  if (response == 416 && s3->ranges.state == S3_RANGES_FIRST)
    {
      /* A zero-length object cannot satisfy any range; ask for all of it. */
      s3->ranges.disabled = TRUE;
      *out_reinitiate = TRUE;
      return;
    }

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

  /* A part belongs to its parent's mirror; it fails rather than moving on. */
  if (s3->part.parent == NULL && req->idx + 1 < req->mirrorlist->len)
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

/* Complete a parent whose own transfer and all children have finished. */
static void
s3_ranges_finish (OstreeFetcher *self, GTask *ptask)
{
  FetcherRequest *preq = g_task_get_task_data (ptask);
  S3Ranges *ranges = &preq->s3->ranges;
  g_autoptr (GError) error = g_steal_pointer (&ranges->error);
  struct stat stbuf;

  if (error != NULL)
    ;
  else if (!glnx_fstat (preq->tmpf.fd, &stbuf, &error))
    ;
  else if ((guint64)stbuf.st_size != ranges->total)
    /* Every part checked its own length, so this cannot happen; but with
     * --http-trusted the file is linked into the repo unverified, so make
     * sure of it. */
    g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Ranged fetch assembled %" G_GUINT64_FORMAT " bytes, expected %" G_GUINT64_FORMAT,
                 (guint64)stbuf.st_size, ranges->total);
  else if (lseek (preq->tmpf.fd, 0, SEEK_SET) < 0)
    glnx_throw_errno_prefix (&error, "lseek");

  if (error != NULL)
    g_task_return_error (ptask, g_steal_pointer (&error));
  else
    g_task_return_boolean (ptask, TRUE);
  _ostree_fetcher_request_finished (self, ptask);
}

void
_ostree_fetcher_s3_complete_after_ranges (OstreeFetcher *self, GTask *task)
{
  FetcherRequest *req = g_task_get_task_data (task);
  S3Ranges *ranges = &req->s3->ranges;
  if (ranges->state != S3_RANGES_SPAWNING && ranges->n_running == 0)
    s3_ranges_finish (self, task);
}

/* Forget @task if it is waiting for credentials. */
static void
s3_unpark (OstreeFetcher *self, GTask *task)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init (&iter, self->s3->creds);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_queue_remove (&((S3CredState *)value)->waiting, task);
}

/* Called as soon as the object has failed, so that no range outlives it:
 * the pull only counts the parent, and may finish (and free the fetcher)
 * once the parent is done. */
void
_ostree_fetcher_s3_cancel_ranges (OstreeFetcher *self, FetcherRequest *req)
{
  if (req->s3 == NULL)
    return;
  S3Ranges *ranges = &req->s3->ranges;

  if (ranges->spawn != NULL)
    {
      g_source_destroy (ranges->spawn);
      g_clear_pointer (&ranges->spawn, g_source_unref);
      ranges->state = S3_RANGES_RUNNING;
    }

  g_autoptr (GPtrArray) tasks = g_steal_pointer (&ranges->children);
  for (guint i = 0; tasks != NULL && i < tasks->len; i++)
    {
      GTask *ctask = tasks->pdata[i];
      FetcherRequest *creq = g_task_get_task_data (ctask);
      if (creq->finished)
        continue;
      if (creq->easy != NULL)
        (void)curl_multi_remove_handle (self->multi, creq->easy);
      s3_unpark (self, ctask);
      g_task_return_new_error (ctask, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "Another part of this object failed");
      _ostree_fetcher_request_finished (self, ctask);
    }
}

static void
s3_part_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OstreeFetcher *self = OSTREE_FETCHER (source);
  GTask *ctask = G_TASK (result);
  FetcherRequest *creq = g_task_get_task_data (ctask);
  GTask *ptask = creq->s3->part.parent;
  FetcherRequest *preq = g_task_get_task_data (ptask);
  S3Ranges *ranges = &preq->s3->ranges;
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (ctask, &error) && ranges->error == NULL)
    ranges->error = g_steal_pointer (&error);

  if (ranges->children != NULL)
    g_ptr_array_remove (ranges->children, ctask);
  g_assert_cmpuint (ranges->n_running, >, 0);
  ranges->n_running--;
  /* Once one part has failed the object has, so stop the others. */
  if (ranges->error != NULL)
    _ostree_fetcher_s3_cancel_ranges (self, preq);
  /* The parent may already have failed on its own and been returned. */
  if (ranges->n_running == 0 && ranges->own_done && !preq->finished)
    s3_ranges_finish (self, ptask);
}

/* Start the child requests for everything after the parent's first part.
 * Runs from an idle because transfers can't be added inside a curl callback. */
static gboolean
s3_ranges_spawn_idle (gpointer user_data)
{
  GTask *ptask = user_data;
  OstreeFetcher *self = g_task_get_source_object (ptask);
  FetcherRequest *preq = g_task_get_task_data (ptask);
  S3Ranges *ranges = &preq->s3->ranges;

  g_clear_pointer (&ranges->spawn, g_source_unref);
  ranges->state = S3_RANGES_RUNNING;
  if (preq->finished)
    return G_SOURCE_REMOVE;

  /* Children write into the parent's file, so it has to exist now. */
  if (!_ostree_fetcher_request_ensure_tmpfile (preq, &ranges->error))
    {
      if (ranges->own_done)
        s3_ranges_finish (self, ptask);
      return G_SOURCE_REMOVE;
    }

  /* Child tasks complete on the thread-default context; make it ours. */
  g_main_context_push_thread_default (self->mainctx);
  for (guint64 offset = ranges->first; offset < ranges->total; offset += S3_RANGE_PART_SIZE)
    {
      FetcherRequest *creq
          = _ostree_fetcher_request_new (self, preq->mirrorlist, preq->filename, preq->flags);
      creq->idx = preq->idx;
      creq->s3 = g_new0 (OstreeS3Request, 1);
      creq->s3->part.parent = g_object_ref (ptask);
      creq->s3->part.offset = offset;
      creq->s3->part.length = MIN (S3_RANGE_PART_SIZE, ranges->total - offset);

      GTask *ctask
          = _ostree_fetcher_request_task_new (self, creq, g_task_get_priority (ptask),
                                              g_task_get_cancellable (ptask), s3_part_done, NULL);

      ranges->n_running++;
      if (ranges->children == NULL)
        ranges->children = g_ptr_array_new ();
      g_ptr_array_add (ranges->children, ctask);
      g_hash_table_add (self->outstanding_requests, ctask); /* takes ownership */
      _ostree_fetcher_request_initiate (creq, ctask);
    }
  g_main_context_pop_thread_default (self->mainctx);

  return G_SOURCE_REMOVE;
}

/* Write one chunk of a part at its place in the parent's tmpfile. */
static gboolean
s3_part_write (FetcherRequest *req, long response, const void *ptr, size_t realsize)
{
  S3Part *part = &req->s3->part;
  FetcherRequest *preq = g_task_get_task_data (part->parent);

  if (response != 206 || !part->confirmed || part->written + realsize > part->length)
    {
      req->caught_write_error
          = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Unexpected response to a ranged request (HTTP %ld)", response);
      return FALSE;
    }
  size_t done = 0;
  while (done < realsize)
    {
      ssize_t n = pwrite (preq->tmpf.fd, (const char *)ptr + done, realsize - done,
                          part->offset + part->written + done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        {
          glnx_set_error_from_errno (&req->caught_write_error);
          return FALSE;
        }
      done += n;
    }
  part->written += realsize;
  req->fetcher->bytes_transferred += realsize;
  return TRUE;
}

gboolean
_ostree_fetcher_s3_write (FetcherRequest *req, GTask *task, const void *ptr, size_t realsize,
                          size_t *out_ret)
{
  OstreeS3Request *s3 = req->s3;
  S3Ranges *ranges = &s3->ranges;
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

  if (s3->part.parent != NULL)
    {
      *out_ret = s3_part_write (req, response, ptr, realsize) ? realsize : (size_t)-1;
      return TRUE;
    }

  if (ranges->state != S3_RANGES_FIRST || response != 206)
    return FALSE;

  /* We asked for the first part only, so a 206 must say which bytes it holds;
   * otherwise it could be taken for the whole object. */
  if (ranges->total == 0)
    {
      req->caught_write_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "Ranged response without a usable Content-Range");
      *out_ret = (size_t)-1;
      return TRUE;
    }

  if (ranges->total > ranges->first)
    {
      /* This is the first part of a larger object, and by the first
       * byte of the body every header is in. The rest may only be
       * fetched separately if it can be tied to this response, which
       * takes a strong ETag for If-Match. Otherwise an object replaced
       * mid-download could be stitched together from two versions, so
       * drop this attempt before writing anything and fetch it whole. */
      if (req->out_etag == NULL || g_str_has_prefix (req->out_etag, "W/"))
        {
          ranges->disabled = TRUE;
          ranges->state = S3_RANGES_RESTART;
          *out_ret = (size_t)-1;
          return TRUE;
        }
      ranges->state = S3_RANGES_SPAWNING;
      /* Transfers can't be added from inside a curl callback. */
      GSource *source = g_idle_source_new ();
      g_source_set_callback (source, s3_ranges_spawn_idle, g_object_ref (task), g_object_unref);
      g_source_attach (source, req->fetcher->mainctx);
      ranges->spawn = source;
    }

  return FALSE;
}

void
_ostree_fetcher_s3_header (FetcherRequest *req, const char *line)
{
  OstreeS3Request *s3 = req->s3;
  const char *bucket_region_header = "x-amz-bucket-region:";
  const char *content_range_header = "Content-Range:";
  guint64 start, end, total;

  if (g_ascii_strncasecmp (line, bucket_region_header, strlen (bucket_region_header)) == 0)
    {
      g_free (s3->bucket_region);
      s3->bucket_region = g_strdup (g_strstrip ((char *)line + strlen (bucket_region_header)));
    }
  else if (g_ascii_strncasecmp (line, content_range_header, strlen (content_range_header)) != 0)
    ;
  else if (s3->part.parent != NULL)
    {
      S3Part *part = &s3->part;
      part->confirmed = _ostree_s3_parse_content_range (line + strlen (content_range_header),
                                                        &start, &end, &total)
                        && start == part->offset && end == part->offset + part->length - 1;
    }
  else if (s3->ranges.state == S3_RANGES_FIRST && s3->ranges.total == 0)
    {
      long response = 0;
      (void)curl_easy_getinfo (req->easy, CURLINFO_RESPONSE_CODE, &response);
      if (response == 206
          && _ostree_s3_parse_content_range (line + strlen (content_range_header), &start, &end,
                                             &total)
          && start == 0)
        {
          s3->ranges.first = end + 1;
          s3->ranges.total = total;
          if (req->max_size > 0 && total > req->max_size && req->caught_write_error == NULL)
            req->caught_write_error = g_error_new (
                G_IO_ERROR, G_IO_ERROR_FAILED,
                "Object exceeded maximum size of %" G_GUINT64_FORMAT " bytes", req->max_size);
        }
    }
}

gboolean
_ostree_fetcher_s3_restarting (FetcherRequest *req)
{
  return req->s3->ranges.state == S3_RANGES_RESTART;
}

gboolean
_ostree_fetcher_s3_transfer_done (FetcherRequest *req, GTask *task, const char *eff_url,
                                  long response, gboolean *out_waiting)
{
  OstreeS3Request *s3 = req->s3;
  S3Ranges *ranges = &s3->ranges;

  if (s3->part.parent != NULL)
    {
      if (response != 206 || s3->part.written != s3->part.length)
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "While fetching %s: incomplete ranged response (HTTP "
                                 "%ld, %" G_GUINT64_FORMAT " of %" G_GUINT64_FORMAT " bytes)",
                                 eff_url, response, s3->part.written, s3->part.length);
      else
        g_task_return_boolean (task, TRUE);
      return TRUE;
    }

  if (ranges->state == S3_RANGES_OFF)
    return FALSE;

  if (response == 206 && ranges->total == 0)
    /* Also checked in _ostree_fetcher_s3_write(), but that never runs for
     * an empty body. */
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "While fetching %s: ranged response without a usable "
                             "Content-Range",
                             eff_url);
  else if (ranges->total > 0 && req->current_size != ranges->first)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "While fetching %s: ranged response has %" G_GUINT64_FORMAT
                             " bytes, expected %" G_GUINT64_FORMAT,
                             eff_url, req->current_size, ranges->first);
  else if (ranges->state == S3_RANGES_SPAWNING || ranges->state == S3_RANGES_RUNNING)
    {
      /* The rest of the object is arriving in parts; it is completed once
       * they all have. */
      ranges->own_done = TRUE;
      *out_waiting = TRUE;
    }
  else
    return FALSE; /* the object fitted in the first part, or was sent whole */
  return TRUE;
}
