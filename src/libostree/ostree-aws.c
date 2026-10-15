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

#include "ostree-aws.h"
#include "ostree-fetcher-s3.h"

#include <aws/auth/auth.h>
#include <aws/auth/aws_imds_client.h>
#include <aws/auth/credentials.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_config.h>
#include <aws/auth/signing_result.h>
#include <aws/common/date_time.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/sdkutils/aws_profile.h>

#include <string.h>

/* How long the caching provider keeps credentials that carry no expiry. */
#define OSTREE_AWS_CACHE_REFRESH_MS (15 * 60 * 1000)
/* How long to wait for IMDS when it is the last resort for the region. */
#define OSTREE_AWS_IMDS_REGION_TIMEOUT_SECS 10

struct OstreeAwsCredentials
{
  gatomicrefcount refcount;
  struct aws_credentials *creds;
};

typedef struct
{
  struct aws_credentials_provider *provider;
  gboolean has_sso;
} ProviderEntry;

typedef struct
{
  struct aws_allocator *alloc;
  struct aws_client_bootstrap *bootstrap;
  struct aws_tls_ctx *tls_ctx; /* NULL if TLS setup failed; HTTPS-based providers are skipped */
  GMutex lock;
  GHashTable *providers; /* "profile\nregion" -> ProviderEntry*, protected by lock */
} OstreeAwsRuntime;

/* Process-wide AWS runtime, started on first use and never torn down. */
static OstreeAwsRuntime *
get_runtime (void)
{
  static gsize initialized;
  static OstreeAwsRuntime runtime;

  if (g_once_init_enter (&initialized))
    {
      OstreeAwsRuntime *rt = &runtime;
      rt->alloc = aws_default_allocator ();
      aws_auth_library_init (rt->alloc);

      struct aws_event_loop_group_options elg_opts = { .loop_count = 1 };
      struct aws_event_loop_group *elg = aws_event_loop_group_new (rt->alloc, &elg_opts);
      g_assert (elg != NULL);

      struct aws_host_resolver_default_options resolver_opts
          = { .max_entries = 8, .el_group = elg };
      struct aws_host_resolver *resolver
          = aws_host_resolver_new_default (rt->alloc, &resolver_opts);
      g_assert (resolver != NULL);

      struct aws_client_bootstrap_options bootstrap_opts
          = { .event_loop_group = elg, .host_resolver = resolver };
      rt->bootstrap = aws_client_bootstrap_new (rt->alloc, &bootstrap_opts);
      g_assert (rt->bootstrap != NULL);

      struct aws_tls_ctx_options tls_opts;
      aws_tls_ctx_options_init_default_client (&tls_opts, rt->alloc);
      rt->tls_ctx = aws_tls_client_ctx_new (rt->alloc, &tls_opts);
      aws_tls_ctx_options_clean_up (&tls_opts);
      if (rt->tls_ctx == NULL)
        g_debug ("aws: no TLS context; SSO, web identity and ECS credentials are unavailable");

      g_mutex_init (&rt->lock);
      rt->providers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

      g_once_init_leave (&initialized, 1);
    }
  return &runtime;
}

static gboolean
imds_disabled (void)
{
  const char *v = g_getenv ("AWS_EC2_METADATA_DISABLED");
  return v != NULL && g_ascii_strcasecmp (v, "true") == 0;
}

/* ---- credentials ---- */

static OstreeAwsCredentials *
credentials_wrap (struct aws_credentials *creds)
{
  OstreeAwsCredentials *ret = g_new0 (OstreeAwsCredentials, 1);
  g_atomic_ref_count_init (&ret->refcount);
  aws_credentials_acquire (creds);
  ret->creds = creds;
  return ret;
}

OstreeAwsCredentials *
_ostree_aws_credentials_ref (OstreeAwsCredentials *creds)
{
  g_atomic_ref_count_inc (&creds->refcount);
  return creds;
}

void
_ostree_aws_credentials_unref (OstreeAwsCredentials *creds)
{
  if (creds == NULL)
    return;
  if (g_atomic_ref_count_dec (&creds->refcount))
    {
      aws_credentials_release (creds->creds);
      g_free (creds);
    }
}

guint64
_ostree_aws_credentials_get_expiry (OstreeAwsCredentials *creds)
{
  return aws_credentials_get_expiration_timepoint_seconds (creds->creds);
}

OstreeAwsCredentials *
_ostree_aws_credentials_new_static (const char *access_key_id, const char *secret_access_key,
                                    const char *session_token)
{
  OstreeAwsRuntime *rt = get_runtime ();
  struct aws_byte_cursor token = { 0 };
  if (session_token != NULL)
    token = aws_byte_cursor_from_c_str (session_token);
  struct aws_credentials *creds
      = aws_credentials_new (rt->alloc, aws_byte_cursor_from_c_str (access_key_id),
                             aws_byte_cursor_from_c_str (secret_access_key), token, UINT64_MAX);
  g_assert (creds != NULL);
  OstreeAwsCredentials *ret = credentials_wrap (creds);
  aws_credentials_release (creds);
  return ret;
}

/* Build: environment -> SSO -> profile -> web identity -> ECS or IMDS,
 * wrapped in a caching provider. aws-c-auth's own default chain omits SSO. */
static struct aws_credentials_provider *
build_chain (OstreeAwsRuntime *rt, const char *profile, const char *region, gboolean *out_has_sso)
{
  struct aws_credentials_provider *providers[5];
  size_t n = 0;
  struct aws_credentials_provider *p;
  struct aws_byte_cursor profile_cur = { 0 };
  if (profile != NULL)
    profile_cur = aws_byte_cursor_from_c_str (profile);

  *out_has_sso = FALSE;

  struct aws_credentials_provider_environment_options env_opts;
  AWS_ZERO_STRUCT (env_opts);
  p = aws_credentials_provider_new_environment (rt->alloc, &env_opts);
  if (p != NULL)
    providers[n++] = p;

  if (rt->tls_ctx != NULL)
    {
      /* Returns NULL when the profile has no SSO configuration. */
      struct aws_credentials_provider_sso_options sso_opts;
      AWS_ZERO_STRUCT (sso_opts);
      sso_opts.profile_name_override = profile_cur;
      sso_opts.bootstrap = rt->bootstrap;
      sso_opts.tls_ctx = rt->tls_ctx;
      p = aws_credentials_provider_new_sso (rt->alloc, &sso_opts);
      if (p != NULL)
        {
          providers[n++] = p;
          *out_has_sso = TRUE;
        }
    }

  struct aws_credentials_provider_profile_options profile_opts;
  AWS_ZERO_STRUCT (profile_opts);
  profile_opts.profile_name_override = profile_cur;
  profile_opts.bootstrap = rt->bootstrap;
  profile_opts.tls_ctx = rt->tls_ctx;
  p = aws_credentials_provider_new_profile (rt->alloc, &profile_opts);
  if (p != NULL)
    providers[n++] = p;

  if (rt->tls_ctx != NULL)
    {
      /* The region is passed explicitly: without one this provider fails
       * outright in IRSA setups that only set the role and token file. */
      struct aws_credentials_provider_sts_web_identity_options web_opts;
      AWS_ZERO_STRUCT (web_opts);
      web_opts.bootstrap = rt->bootstrap;
      web_opts.tls_ctx = rt->tls_ctx;
      web_opts.profile_name_override = profile_cur;
      web_opts.region = aws_byte_cursor_from_c_str (region);
      p = aws_credentials_provider_new_sts_web_identity (rt->alloc, &web_opts);
      if (p != NULL)
        providers[n++] = p;
    }

  struct aws_credentials_provider_ecs_environment_options ecs_opts;
  AWS_ZERO_STRUCT (ecs_opts);
  ecs_opts.bootstrap = rt->bootstrap;
  ecs_opts.tls_ctx = rt->tls_ctx;
  p = aws_credentials_provider_new_ecs_from_environment (rt->alloc, &ecs_opts);
  if (p == NULL && !imds_disabled ())
    {
      struct aws_credentials_provider_imds_options imds_opts;
      AWS_ZERO_STRUCT (imds_opts);
      imds_opts.bootstrap = rt->bootstrap;
      imds_opts.imds_version = IMDS_PROTOCOL_V2;
      p = aws_credentials_provider_new_imds (rt->alloc, &imds_opts);
    }
  if (p != NULL)
    providers[n++] = p;

  struct aws_credentials_provider_chain_options chain_opts
      = { .providers = providers, .provider_count = n };
  struct aws_credentials_provider *chain
      = aws_credentials_provider_new_chain (rt->alloc, &chain_opts);
  for (size_t i = 0; i < n; i++)
    aws_credentials_provider_release (providers[i]);
  if (chain == NULL)
    return NULL;

  struct aws_credentials_provider_cached_options cached_opts;
  AWS_ZERO_STRUCT (cached_opts);
  cached_opts.source = chain;
  cached_opts.refresh_time_in_milliseconds = OSTREE_AWS_CACHE_REFRESH_MS;
  struct aws_credentials_provider *cached
      = aws_credentials_provider_new_cached (rt->alloc, &cached_opts);
  aws_credentials_provider_release (chain);
  return cached;
}

typedef struct
{
  char *profile_display;
  gboolean has_sso;
} CredsTaskData;

static void
creds_task_data_free (CredsTaskData *data)
{
  g_free (data->profile_display);
  g_free (data);
}

/* May run on the AWS event-loop thread or synchronously on the caller's
 * thread; GTask delivers the result on the caller's main context either way. */
static void
on_get_credentials (struct aws_credentials *creds, int error_code, void *user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  CredsTaskData *data = g_task_get_task_data (task);

  if (creds != NULL)
    {
      g_task_return_pointer (task, credentials_wrap (creds),
                             (GDestroyNotify)_ostree_aws_credentials_unref);
      return;
    }

  g_autoptr (GString) msg = g_string_new (NULL);
  g_string_append_printf (msg,
                          "No AWS credentials available for s3:// remote (tried environment, "
                          "%sshared config profile '%s', web identity, ECS/IMDS): %s",
                          data->has_sso ? "SSO, " : "", data->profile_display,
                          aws_error_str (error_code));
  if (data->has_sso)
    g_string_append_printf (msg,
                            ". Profile '%s' uses SSO: if the SSO session has expired, run "
                            "`aws sso login --profile %s`, or configure the profile with "
                            "`credential_process = aws configure export-credentials --profile %s "
                            "--format process`",
                            data->profile_display, data->profile_display, data->profile_display);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s", msg->str);
}

void
_ostree_aws_get_credentials_async (const char *profile, const char *region, gboolean force_refresh,
                                   GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  OstreeAwsRuntime *rt = get_runtime ();
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, _ostree_aws_get_credentials_async);

  g_autofree char *key = g_strdup_printf ("%s\n%s", profile ?: "", region);

  g_mutex_lock (&rt->lock);
  ProviderEntry *entry = g_hash_table_lookup (rt->providers, key);
  if (entry != NULL && force_refresh)
    {
      /* The caching provider has no invalidate call, so replace it. The old
       * one is deliberately leaked rather than released: another fetcher may
       * have a query in flight on it, and forced refreshes are rare. */
      g_hash_table_remove (rt->providers, key);
      entry = NULL;
    }
  if (entry == NULL)
    {
      gboolean has_sso = FALSE;
      struct aws_credentials_provider *provider = build_chain (rt, profile, region, &has_sso);
      if (provider != NULL)
        {
          entry = g_new0 (ProviderEntry, 1);
          entry->provider = provider;
          entry->has_sso = has_sso;
          g_hash_table_insert (rt->providers, g_strdup (key), entry);
        }
    }
  struct aws_credentials_provider *provider = entry ? entry->provider : NULL;
  const gboolean has_sso = entry ? entry->has_sso : FALSE;
  g_mutex_unlock (&rt->lock);

  if (provider == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to set up AWS credential providers: %s",
                               aws_error_str (aws_last_error ()));
      return;
    }

  CredsTaskData *data = g_new0 (CredsTaskData, 1);
  data->profile_display = g_strdup (profile ?: (g_getenv ("AWS_PROFILE") ?: "default"));
  data->has_sso = has_sso;
  g_task_set_task_data (task, data, (GDestroyNotify)creds_task_data_free);

  if (aws_credentials_provider_get_credentials (provider, on_get_credentials, g_object_ref (task))
      != AWS_OP_SUCCESS)
    {
      g_object_unref (task); /* the callback will not run */
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to query AWS credentials: %s",
                               aws_error_str (aws_last_error ()));
    }
}

OstreeAwsCredentials *
_ostree_aws_get_credentials_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---- region ---- */

static char *
region_from_profile (OstreeAwsRuntime *rt, const char *profile)
{
  char *ret = NULL;
  struct aws_string *path = aws_get_config_file_path (rt->alloc, NULL);
  if (path == NULL)
    return NULL;
  struct aws_profile_collection *collection
      = aws_profile_collection_new_from_file (rt->alloc, path, AWS_PST_CONFIG);
  aws_string_destroy (path);
  if (collection == NULL)
    return NULL;

  struct aws_byte_cursor override = { 0 };
  if (profile != NULL)
    override = aws_byte_cursor_from_c_str (profile);
  struct aws_string *name = aws_get_profile_name (rt->alloc, profile ? &override : NULL);
  if (name != NULL)
    {
      const struct aws_profile *p = aws_profile_collection_get_profile (collection, name);
      if (p != NULL)
        {
          struct aws_string *prop_name = aws_string_new_from_c_str (rt->alloc, "region");
          const struct aws_profile_property *prop = aws_profile_get_property (p, prop_name);
          if (prop != NULL)
            ret = g_strdup (aws_string_c_str (aws_profile_property_get_value (prop)));
          aws_string_destroy (prop_name);
        }
      aws_string_destroy (name);
    }
  aws_profile_collection_release (collection);
  return ret;
}

typedef struct
{
  gatomicrefcount refcount;
  GMutex lock;
  GCond cond;
  gboolean done;
  char *region;
} ImdsWait;

static void
imds_wait_unref (ImdsWait *wait)
{
  if (!g_atomic_ref_count_dec (&wait->refcount))
    return;
  g_mutex_clear (&wait->lock);
  g_cond_clear (&wait->cond);
  g_free (wait->region);
  g_free (wait);
}

static void
on_imds_instance_info (const struct aws_imds_instance_info *info, int error_code, void *user_data)
{
  ImdsWait *wait = user_data;
  g_mutex_lock (&wait->lock);
  if (error_code == 0 && info != NULL && info->region.len > 0)
    wait->region = g_strndup ((const char *)info->region.ptr, info->region.len);
  wait->done = TRUE;
  g_cond_signal (&wait->cond);
  g_mutex_unlock (&wait->lock);
  imds_wait_unref (wait);
}

static char *
region_from_imds (OstreeAwsRuntime *rt)
{
  if (imds_disabled ())
    return NULL;

  struct aws_imds_client_options opts;
  AWS_ZERO_STRUCT (opts);
  opts.bootstrap = rt->bootstrap;
  opts.imds_version = IMDS_PROTOCOL_V2;
  struct aws_imds_client *client = aws_imds_client_new (rt->alloc, &opts);
  if (client == NULL)
    return NULL;

  char *ret = NULL;
  ImdsWait *wait = g_new0 (ImdsWait, 1);
  g_atomic_ref_count_init (&wait->refcount);
  g_mutex_init (&wait->lock);
  g_cond_init (&wait->cond);

  g_atomic_ref_count_inc (&wait->refcount); /* for the callback */
  if (aws_imds_client_get_instance_info (client, on_imds_instance_info, wait) != AWS_OP_SUCCESS)
    {
      imds_wait_unref (wait); /* the callback will not run */
    }
  else
    {
      const gint64 deadline
          = g_get_monotonic_time () + OSTREE_AWS_IMDS_REGION_TIMEOUT_SECS * G_TIME_SPAN_SECOND;
      g_mutex_lock (&wait->lock);
      while (!wait->done)
        {
          if (!g_cond_wait_until (&wait->cond, &wait->lock, deadline))
            break;
        }
      ret = g_steal_pointer (&wait->region);
      g_mutex_unlock (&wait->lock);
    }
  imds_wait_unref (wait);
  aws_imds_client_release (client);
  return ret;
}

char *
_ostree_aws_resolve_region (const char *url_region, const char *profile, gboolean allow_imds,
                            GError **error)
{
  if (url_region != NULL)
    return g_strdup (url_region);

  static const char *const env_names[] = { "AWS_REGION", "AWS_DEFAULT_REGION" };
  for (guint i = 0; i < G_N_ELEMENTS (env_names); i++)
    {
      const char *v = g_getenv (env_names[i]);
      if (_ostree_s3_region_is_valid (v))
        return g_strdup (v);
    }

  OstreeAwsRuntime *rt = get_runtime ();
  g_autofree char *from_profile = region_from_profile (rt, profile);
  if (_ostree_s3_region_is_valid (from_profile))
    return g_steal_pointer (&from_profile);

  g_autofree char *from_imds = allow_imds ? region_from_imds (rt) : NULL;
  if (_ostree_s3_region_is_valid (from_imds))
    return g_steal_pointer (&from_imds);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
               "Could not determine the AWS region for the s3:// remote; add ?region=<region> "
               "to the URL or set AWS_REGION");
  return NULL;
}

/* ---- signing ---- */

typedef struct
{
  GPtrArray *headers;
  gboolean done;
  int error_code;
} SignState;

static void
on_sign_complete (struct aws_signing_result *result, int error_code, void *user_data)
{
  SignState *state = user_data;
  state->done = TRUE;
  state->error_code = error_code;
  if (error_code != 0)
    return;

  struct aws_array_list *list = NULL;
  aws_signing_result_get_property_list (result, g_aws_http_headers_property_list_name, &list);
  const size_t n = list ? aws_array_list_length (list) : 0;
  for (size_t i = 0; i < n; i++)
    {
      struct aws_signing_result_property prop;
      AWS_ZERO_STRUCT (prop);
      if (aws_array_list_get_at (list, &prop, i) != AWS_OP_SUCCESS)
        continue;
      g_ptr_array_add (state->headers, g_strdup_printf ("%s: %s", aws_string_c_str (prop.name),
                                                        aws_string_c_str (prop.value)));
    }
}

gboolean
_ostree_aws_sign_get_at (OstreeAwsCredentials *creds, const char *url, const char *region,
                         guint64 unix_time, GPtrArray *headers, GError **error)
{
  OstreeAwsRuntime *rt = get_runtime ();

  g_autoptr (GUri) uri = g_uri_parse (url, G_URI_FLAGS_ENCODED, error);
  if (uri == NULL)
    return FALSE;
  const char *scheme = g_uri_get_scheme (uri);
  const int port = g_uri_get_port (uri);
  const gboolean default_port = port == -1 || (port == 443 && g_str_equal (scheme, "https"))
                                || (port == 80 && g_str_equal (scheme, "http"));
  /* GUri returns IPv6 literals without their brackets */
  const char *h = g_uri_get_host (uri);
  g_autofree char *host_name = strchr (h, ':') != NULL ? g_strdup_printf ("[%s]", h) : g_strdup (h);
  g_autofree char *host
      = default_port ? g_strdup (host_name) : g_strdup_printf ("%s:%d", host_name, port);
  const char *path = g_uri_get_path (uri);
  const char *query = g_uri_get_query (uri);
  g_autofree char *path_and_query = query ? g_strdup_printf ("%s?%s", *path ? path : "/", query)
                                          : g_strdup (*path ? path : "/");

  struct aws_http_message *msg = aws_http_message_new_request (rt->alloc);
  g_assert (msg != NULL);
  aws_http_message_set_request_method (msg, aws_byte_cursor_from_c_str ("GET"));
  aws_http_message_set_request_path (msg, aws_byte_cursor_from_c_str (path_and_query));
  struct aws_http_header host_header
      = { .name = aws_byte_cursor_from_c_str ("host"), .value = aws_byte_cursor_from_c_str (host) };
  aws_http_message_add_header (msg, host_header);
  struct aws_signable *signable = aws_signable_new_http_request (rt->alloc, msg);
  g_assert (signable != NULL);

  struct aws_signing_config_aws config;
  AWS_ZERO_STRUCT (config);
  config.config_type = AWS_SIGNING_CONFIG_AWS;
  config.algorithm = AWS_SIGNING_ALGORITHM_V4;
  config.signature_type = AWS_ST_HTTP_REQUEST_HEADERS;
  config.region = aws_byte_cursor_from_c_str (region);
  config.service = aws_byte_cursor_from_c_str ("s3");
  aws_date_time_init_epoch_secs (&config.date, (double)unix_time);
  /* S3 wants the path signed exactly as sent: encoded once, not normalized. */
  config.flags.use_double_uri_encode = false;
  config.flags.should_normalize_uri_path = false;
  config.signed_body_value = g_aws_signed_body_value_unsigned_payload;
  config.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
  config.credentials = creds->creds;

  SignState state = { .headers = headers };
  const int rc = aws_sign_request_aws (
      rt->alloc, signable, (struct aws_signing_config_base *)&config, on_sign_complete, &state);
  gboolean ret = FALSE;
  if (rc != AWS_OP_SUCCESS)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to sign S3 request: %s",
                 aws_error_str (aws_last_error ()));
  else if (!state.done)
    /* With explicit credentials aws-c-auth completes inline. If that ever
     * changes, fail loudly; @state must not be touched after we return. */
    g_error ("aws_sign_request_aws did not complete synchronously");
  else if (state.error_code != 0)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to sign S3 request: %s",
                 aws_error_str (state.error_code));
  else
    ret = TRUE;

  aws_signable_destroy (signable);
  aws_http_message_release (msg);
  return ret;
}

gboolean
_ostree_aws_sign_get (OstreeAwsCredentials *creds, const char *url, const char *region,
                      GPtrArray *headers, GError **error)
{
  return _ostree_aws_sign_get_at (creds, url, region, g_get_real_time () / G_USEC_PER_SEC, headers,
                                  error);
}
