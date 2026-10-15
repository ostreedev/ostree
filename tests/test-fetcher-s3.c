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

  return g_test_run ();
}
