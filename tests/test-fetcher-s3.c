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

#include "libglnx.h"
#include "ostree-aws.h"
#include "ostree-fetcher-s3.h"
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* The same flags _ostree_fetcher_uri_parse() uses. */
static OstreeS3Location *
parse (const char *str, GError **error)
{
  g_autoptr (GUri) uri = g_uri_parse (
      str, G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED | G_URI_FLAGS_SCHEME_NORMALIZE, error);
  if (uri == NULL)
    return NULL;
  return _ostree_s3_location_parse (uri, error);
}

static void
assert_url (const char *s3, const char *region, const char *subpath, const char *expected)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeS3Location) loc = parse (s3, &error);
  g_assert_no_error (error);
  g_autofree char *url = _ostree_s3_location_to_request_url (loc, region, subpath);
  g_assert_cmpstr (url, ==, expected);
}

static void
assert_rejected (const char *s3, const char *needle)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeS3Location) loc = parse (s3, &error);
  g_assert_null (loc);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  if (strstr (error->message, needle) == NULL)
    g_error ("expected '%s' in error for %s, got: %s", needle, s3, error->message);
}

static void
test_parse_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeS3Location) loc = parse (
      "s3://my-bucket/some/repo?region=us-gov-west-1&profile=dev&endpoint=https://h:9000/", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (loc->bucket, ==, "my-bucket");
  g_assert_cmpstr (loc->key_prefix, ==, "some/repo");
  g_assert_cmpstr (loc->region, ==, "us-gov-west-1");
  g_assert_cmpstr (loc->profile, ==, "dev");
  g_assert_cmpstr (loc->endpoint, ==, "https://h:9000");

  g_autoptr (OstreeS3Location) bare = parse ("s3://my-bucket", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (bare->key_prefix, ==, "");
  g_assert_null (bare->region);
  g_assert_null (bare->profile);
  g_assert_null (bare->endpoint);
}

static void
test_parse_rejects (void)
{
  assert_rejected ("s3://my-bucket/repo?regoin=us-east-1", "Unknown parameter 'regoin'");
  assert_rejected ("s3://my-bucket/repo?region=us-east-1&region=us-west-2", "more than once");
  assert_rejected ("s3://my-bucket/repo?region=", "is empty");
  assert_rejected ("s3://my-bucket/repo?region=US_EAST", "region may only contain");
  assert_rejected ("s3://my-bucket/repo?region=a.evil.example", "region may only contain");
  assert_rejected ("s3://my-bucket/repo#frag", "fragment");
  assert_rejected ("s3://key:secret@my-bucket/repo", "credentials are not allowed");
  assert_rejected ("s3://my-bucket:9000/repo", "port is not allowed");
  assert_rejected ("s3:///repo", "bucket");
  assert_rejected ("s3://my-bucket/repo?endpoint=ftp://h", "endpoint must look like");
  assert_rejected ("s3://my-bucket/repo?endpoint=http://h/sub/path", "endpoint must look like");
  assert_rejected ("s3://my-bucket/repo?endpoint=http://u@h", "endpoint must look like");
  /* an encoded slash would silently become a path separator */
  assert_rejected ("s3://my-bucket/re%2Fpo", "%2F");
  assert_rejected ("s3://my-bucket/re%2fpo", "%2F");
}

static void
test_url_addressing (void)
{
  assert_url ("s3://my-bucket/repo", "us-west-2", "objects/ab/cd.filez",
              "https://my-bucket.s3.us-west-2.amazonaws.com/repo/objects/ab/cd.filez");
  /* GovCloud uses the same hostname form */
  assert_url ("s3://my-bucket/repo", "us-gov-west-1", "config",
              "https://my-bucket.s3.us-gov-west-1.amazonaws.com/repo/config");
  /* Endpoint override is always path-style, whatever the bucket looks like */
  assert_url ("s3://b1/repo?endpoint=http://127.0.0.1:9000&region=us-east-1", "us-east-1",
              "summary", "http://127.0.0.1:9000/b1/repo/summary");
  assert_url ("s3://my-bucket/repo?endpoint=https://vpce.example.com", NULL, "summary",
              "https://vpce.example.com/my-bucket/repo/summary");
  /* IPv6 endpoints keep their brackets */
  assert_url ("s3://b1/repo?endpoint=http://[::1]:9000", NULL, "config",
              "http://[::1]:9000/b1/repo/config");
}

/* Legacy bucket names must not produce a broken hostname. */
static void
test_url_legacy_buckets (void)
{
  assert_url ("s3://my.dotted.bucket/repo", "us-east-1", "config",
              "https://s3.us-east-1.amazonaws.com/my.dotted.bucket/repo/config");
  assert_url ("s3://my_bucket/repo", "us-east-1", "config",
              "https://s3.us-east-1.amazonaws.com/my_bucket/repo/config");
  assert_url ("s3://ab/repo", "us-east-1", "config",
              "https://s3.us-east-1.amazonaws.com/ab/repo/config");
}

/* Every key character is encoded exactly once. */
static void
test_url_key_encoding (void)
{
  const char *base = "https://my-bucket.s3.us-east-1.amazonaws.com/repo/";
  struct
  {
    const char *subpath;
    const char *encoded;
  } cases[] = {
    { "deltas/ab/c+d/superblock", "deltas/ab/c%2Bd/superblock" },
    { "a b", "a%20b" },
    { "100%.txt", "100%25.txt" },
    { "tilde~dash-dot.under_", "tilde~dash-dot.under_" },
    { "caf\xc3\xa9", "caf%C3%A9" },
    { "q?x=1&y", "q%3Fx%3D1%26y" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *expected = g_strconcat (base, cases[i].encoded, NULL);
      assert_url ("s3://my-bucket/repo", "us-east-1", cases[i].subpath, expected);
    }
  /* An encoded prefix in the s3:// URL is decoded, then encoded once */
  assert_url ("s3://my-bucket/my%20repo/100%25", "us-east-1", "config",
              "https://my-bucket.s3.us-east-1.amazonaws.com/my%20repo/100%25/config");
}

/* Slash variations all mean the same location. */
static void
test_url_slashes (void)
{
  const char *expected = "https://my-bucket.s3.us-east-1.amazonaws.com/repo/config";
  assert_url ("s3://my-bucket/repo", "us-east-1", "config", expected);
  assert_url ("s3://my-bucket/repo/", "us-east-1", "config", expected);
  assert_url ("s3://my-bucket//repo//", "us-east-1", "/config", expected);
  assert_url ("s3://my-bucket/repo", "us-east-1", "//config", expected);
  /* The URI parser removes dot segments, encoded or not, so none reach the
   * request (where curl would remove them after signing). */
  assert_url ("s3://my-bucket/./repo", "us-east-1", "config", expected);
  assert_url ("s3://my-bucket/x/%2E%2E/repo", "us-east-1", "config", expected);

  assert_url ("s3://my-bucket", "us-east-1", NULL, "https://my-bucket.s3.us-east-1.amazonaws.com/");
  assert_url ("s3://my-bucket/", "us-east-1", "", "https://my-bucket.s3.us-east-1.amazonaws.com/");
  assert_url ("s3://my-bucket", "us-east-1", "config",
              "https://my-bucket.s3.us-east-1.amazonaws.com/config");
}

static void
test_error_body (void)
{
  g_autofree char *code = NULL;
  g_autofree char *message = NULL;
  g_autofree char *region = NULL;

  const char *denied = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<Error><Code>AccessDenied</Code><Message>Access &amp; Denied</Message>"
                       "<RequestId>X</RequestId></Error>";
  g_assert_true (_ostree_s3_parse_error_body (denied, strlen (denied), &code, &message, &region));
  g_assert_cmpstr (code, ==, "AccessDenied");
  g_assert_cmpstr (message, ==, "Access & Denied");
  g_assert_null (region);
  g_clear_pointer (&code, g_free);
  g_clear_pointer (&message, g_free);

  const char *redirect = "<Error><Code>PermanentRedirect</Code><Message>m</Message>"
                         "<Endpoint>b.s3.us-west-2.amazonaws.com</Endpoint>"
                         "<Region>us-west-2</Region></Error>";
  g_assert_true (
      _ostree_s3_parse_error_body (redirect, strlen (redirect), &code, &message, &region));
  g_assert_cmpstr (code, ==, "PermanentRedirect");
  g_assert_cmpstr (region, ==, "us-west-2");
  g_clear_pointer (&code, g_free);
  g_clear_pointer (&message, g_free);
  g_clear_pointer (&region, g_free);

  /* NULL out params are allowed */
  g_assert_true (_ostree_s3_parse_error_body (denied, strlen (denied), NULL, NULL, NULL));
}

/* Whatever a proxy or captive portal sends back must not be
 * mistaken for, or crash on, an S3 error document. */
static void
test_error_body_not_s3 (void)
{
  const char *bodies[] = {
    "",
    "Service Unavailable",
    "<html><body><h1>502 Bad Gateway</h1></body></html>",
    "<Error><Code>AccessDenied</Code><Mess", /* truncated */
    "<Other><Code>AccessDenied</Code></Other>",
    "\xff\xfe\x00garbage",
  };
  for (guint i = 0; i < G_N_ELEMENTS (bodies); i++)
    {
      char *code = (char *)0x1;
      g_assert_false (
          _ostree_s3_parse_error_body (bodies[i], strlen (bodies[i]), &code, NULL, NULL));
      /* out params are untouched on failure */
      g_assert_true (code == (char *)0x1);
    }
}

/* Credentials that arrive already close to expiry must be
 * used, not refetched in a loop. */
static void
test_credentials_usable (void)
{
  const guint64 now = 1000000;
  /* never expire */
  g_assert_true (_ostree_s3_credentials_usable (G_MAXUINT64, 0, now));
  /* plenty of time left */
  g_assert_true (_ostree_s3_credentials_usable (now + 3600, now - 1800, now));
  /* inside the refresh window and fetched long ago: refresh */
  g_assert_false (_ostree_s3_credentials_usable (now + 299, now - 1800, now));
  /* inside the refresh window but only just fetched: use them */
  g_assert_true (_ostree_s3_credentials_usable (now + 120, now - 5, now));
  g_assert_true (_ostree_s3_credentials_usable (now + 120, now, now));
  /* ... until the minimum refetch interval has passed */
  g_assert_false (_ostree_s3_credentials_usable (now + 120, now - 60, now));
  /* already expired is never usable, however fresh */
  g_assert_false (_ostree_s3_credentials_usable (now, now, now));
  g_assert_false (_ostree_s3_credentials_usable (now - 1, now, now));
  /* a clock that went backwards must not make stale credentials usable */
  g_assert_false (_ostree_s3_credentials_usable (now + 120, now + 500, now));
}

/* ---- ostree-aws ---- */

static const char *
find_header (GPtrArray *headers, const char *name)
{
  const gsize len = strlen (name);
  for (guint i = 0; i < headers->len; i++)
    {
      const char *h = headers->pdata[i];
      if (g_ascii_strncasecmp (h, name, len) == 0 && h[len] == ':')
        return h + len + 2;
    }
  return NULL;
}

/* Expected signatures were computed with an independent SigV4
 * implementation (Python hashlib/hmac), not with aws-c-auth. */
static void
test_sign (void)
{
  const guint64 t = 1369353600; /* 20130524T000000Z */
  g_autoptr (GError) error = NULL;

  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_credentials_new_static (
      "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", NULL);
  g_assert_cmpuint (_ostree_aws_credentials_get_expiry (creds), ==, G_MAXUINT64);
  g_autoptr (GPtrArray) headers = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (creds,
                           "https://examplebucket.s3.amazonaws.com/repo/deltas/ab/c%2Bd/superblock",
                           "us-east-1", t, headers, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (headers, "Authorization"), ==,
                   "AWS4-HMAC-SHA256 "
                   "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                   "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
                   "Signature=1f1b4657c75f09877e88547ee17c26163a6c952895a7b120ef92e311633ed4e7");
  g_assert_cmpstr (find_header (headers, "x-amz-date"), ==, "20130524T000000Z");
  g_assert_cmpstr (find_header (headers, "x-amz-content-sha256"), ==, "UNSIGNED-PAYLOAD");
  g_assert_null (find_header (headers, "x-amz-security-token"));

  g_autoptr (OstreeAwsCredentials) temp_creds = _ostree_aws_credentials_new_static (
      "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "TOKENEXAMPLE");
  g_autoptr (GPtrArray) temp_headers = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (temp_creds, "https://examplebucket.s3.amazonaws.com/repo/config",
                           "us-east-1", t, temp_headers, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (temp_headers, "Authorization"), ==,
                   "AWS4-HMAC-SHA256 "
                   "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                   "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-security-token, "
                   "Signature=d1670c1ba4ed45429c40b210bd45785d7a9fd2181a243a18cba3c25d55fa9c8f");
  g_assert_cmpstr (find_header (temp_headers, "x-amz-security-token"), ==, "TOKENEXAMPLE");
}

/* A non-default port is part of the signed host header. */
static void
test_sign_port (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_credentials_new_static ("AK", "SK", NULL);
  g_autoptr (GPtrArray) with_port = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) default_port = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) no_port = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (creds, "http://127.0.0.1:9000/b/k", "us-east-1", 1, with_port, &error);
  g_assert_no_error (error);
  _ostree_aws_sign_get_at (creds, "http://127.0.0.1:80/b/k", "us-east-1", 1, default_port, &error);
  g_assert_no_error (error);
  _ostree_aws_sign_get_at (creds, "http://127.0.0.1/b/k", "us-east-1", 1, no_port, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (default_port, "Authorization"), ==,
                   find_header (no_port, "Authorization"));
  g_assert_cmpstr (find_header (with_port, "Authorization"), !=,
                   find_header (no_port, "Authorization"));

  /* An IPv6 host is signed with its brackets, as curl sends it. The expected
   * value comes from an independent SigV4 implementation. */
  g_autoptr (GPtrArray) ipv6 = g_ptr_array_new_with_free_func (g_free);
  _ostree_aws_sign_get_at (creds, "http://[::1]:9000/b/k", "us-east-1", 1, ipv6, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (find_header (ipv6, "Authorization"), ==,
                   "AWS4-HMAC-SHA256 Credential=AK/19700101/us-east-1/s3/aws4_request, "
                   "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
                   "Signature=0ad15a262c6dd98edd8c96884cc8604ee931d64885b48d2b6bf3ed786959868c");
}

static void
clear_aws_env (const char *tmpdir)
{
  static const char *const names[] = { "AWS_REGION",
                                       "AWS_DEFAULT_REGION",
                                       "AWS_PROFILE",
                                       "AWS_ACCESS_KEY_ID",
                                       "AWS_SECRET_ACCESS_KEY",
                                       "AWS_SESSION_TOKEN",
                                       "AWS_ROLE_ARN",
                                       "AWS_WEB_IDENTITY_TOKEN_FILE",
                                       "AWS_CONTAINER_CREDENTIALS_FULL_URI",
                                       "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    g_unsetenv (names[i]);
  g_autofree char *config = g_build_filename (tmpdir, "config", NULL);
  g_autofree char *credentials = g_build_filename (tmpdir, "credentials", NULL);
  g_setenv ("AWS_CONFIG_FILE", config, TRUE);
  g_setenv ("AWS_SHARED_CREDENTIALS_FILE", credentials, TRUE);
  g_setenv ("AWS_EC2_METADATA_DISABLED", "true", TRUE);
  g_unlink (config);
  g_unlink (credentials);
}

static void
test_resolve_region (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmpdir = g_dir_make_tmp ("test-fetcher-s3-XXXXXX", &error);
  g_assert_no_error (error);
  clear_aws_env (tmpdir);
  g_autofree char *config = g_build_filename (tmpdir, "config", NULL);

  /* nothing configured */
  g_autofree char *none = _ostree_aws_resolve_region (NULL, NULL, TRUE, &error);
  g_assert_null (none);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "?region="));
  g_clear_error (&error);

  /* profile */
  g_file_set_contents (config,
                       "[default]\nregion = eu-west-1\n\n[profile gov]\nregion = us-gov-east-1\n",
                       -1, &error);
  g_assert_no_error (error);
  g_autofree char *from_default = _ostree_aws_resolve_region (NULL, NULL, TRUE, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_default, ==, "eu-west-1");
  g_autofree char *from_named = _ostree_aws_resolve_region (NULL, "gov", TRUE, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_named, ==, "us-gov-east-1");
  g_setenv ("AWS_PROFILE", "gov", TRUE);
  g_autofree char *from_env_profile = _ostree_aws_resolve_region (NULL, NULL, TRUE, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_env_profile, ==, "us-gov-east-1");

  /* AWS_DEFAULT_REGION beats the profile */
  g_setenv ("AWS_DEFAULT_REGION", "ap-south-1", TRUE);
  g_autofree char *from_default_env = _ostree_aws_resolve_region (NULL, NULL, TRUE, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_default_env, ==, "ap-south-1");

  /* AWS_REGION beats AWS_DEFAULT_REGION */
  g_setenv ("AWS_REGION", "us-gov-west-1", TRUE);
  g_autofree char *from_region_env = _ostree_aws_resolve_region (NULL, NULL, TRUE, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_region_env, ==, "us-gov-west-1");

  /* the URL beats everything */
  g_autofree char *from_url = _ostree_aws_resolve_region ("us-east-2", NULL, TRUE, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (from_url, ==, "us-east-2");

  g_unlink (config);
  g_rmdir (tmpdir);
}

static void
on_creds (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GAsyncResult **out = user_data;
  *out = g_object_ref (result);
  g_main_context_wakeup (NULL);
}

static OstreeAwsCredentials *
get_creds_sync (const char *region, gboolean force, GError **error)
{
  g_autoptr (GAsyncResult) result = NULL;
  _ostree_aws_get_credentials_async (NULL, region, force, NULL, on_creds, &result);
  /* Must never complete before returning to the main loop. */
  g_assert_null (result);
  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);
  return _ostree_aws_get_credentials_finish (result, error);
}

static void
test_credentials_chain (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *tmpdir = g_dir_make_tmp ("test-fetcher-s3-XXXXXX", &error);
  g_assert_no_error (error);
  clear_aws_env (tmpdir);

  /* Each case uses its own region so it gets its own cached chain. */
  g_autoptr (OstreeAwsCredentials) missing = get_creds_sync ("test-none-1", FALSE, &error);
  g_assert_null (missing);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_nonnull (strstr (error->message, "No AWS credentials available"));
  g_assert_nonnull (strstr (error->message, "profile 'default'"));
  g_clear_error (&error);

  g_setenv ("AWS_ACCESS_KEY_ID", "AKIDFIRST", TRUE);
  g_setenv ("AWS_SECRET_ACCESS_KEY", "secret", TRUE);
  g_autoptr (OstreeAwsCredentials) first = get_creds_sync ("test-env-1", FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);
  g_assert_cmpuint (_ostree_aws_credentials_get_expiry (first), ==, G_MAXUINT64);

  /* Cached: a changed environment is not seen without force_refresh ... */
  g_autoptr (GPtrArray) h_first = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) h_cached = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) h_forced = g_ptr_array_new_with_free_func (g_free);
  g_setenv ("AWS_ACCESS_KEY_ID", "AKIDSECOND", TRUE);
  g_autoptr (OstreeAwsCredentials) cached = get_creds_sync ("test-env-1", FALSE, &error);
  g_assert_no_error (error);
  /* ... and is seen with it. */
  g_autoptr (OstreeAwsCredentials) forced = get_creds_sync ("test-env-1", TRUE, &error);
  g_assert_no_error (error);

  const char *url = "https://b.s3.us-east-1.amazonaws.com/k";
  g_assert_true (_ostree_aws_sign_get_at (first, url, "us-east-1", 1, h_first, &error));
  g_assert_true (_ostree_aws_sign_get_at (cached, url, "us-east-1", 1, h_cached, &error));
  g_assert_true (_ostree_aws_sign_get_at (forced, url, "us-east-1", 1, h_forced, &error));
  g_assert_nonnull (strstr (find_header (h_first, "Authorization"), "Credential=AKIDFIRST/"));
  g_assert_nonnull (strstr (find_header (h_cached, "Authorization"), "Credential=AKIDFIRST/"));
  g_assert_nonnull (strstr (find_header (h_forced, "Authorization"), "Credential=AKIDSECOND/"));

  g_rmdir (tmpdir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/s3/parse/fields", test_parse_fields);
  g_test_add_func ("/s3/parse/rejects", test_parse_rejects);
  g_test_add_func ("/s3/url/addressing", test_url_addressing);
  g_test_add_func ("/s3/url/legacy-buckets", test_url_legacy_buckets);
  g_test_add_func ("/s3/url/key-encoding", test_url_key_encoding);
  g_test_add_func ("/s3/url/slashes", test_url_slashes);
  g_test_add_func ("/s3/error-body/s3", test_error_body);
  g_test_add_func ("/s3/error-body/not-s3", test_error_body_not_s3);
  g_test_add_func ("/s3/credentials-usable", test_credentials_usable);
  g_test_add_func ("/aws/sign", test_sign);
  g_test_add_func ("/aws/sign-port", test_sign_port);
  g_test_add_func ("/aws/resolve-region", test_resolve_region);
  g_test_add_func ("/aws/credentials-chain", test_credentials_chain);

  return g_test_run ();
}
