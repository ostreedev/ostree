/*
 * Copyright (C) 2015 Red Hat, Inc.
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "libglnx.h"
#include <gpgme.h>

#include "ostree-gpg-verify-result-private.h"

#define assert_no_gpg_error(err, filename) \
  G_STMT_START \
  { \
    if (err != GPG_ERR_NO_ERROR) \
      { \
        g_autoptr (GString) string = g_string_new ("assertion failed "); \
        g_string_append_printf (string, "%s: %s ", gpgme_strsource (err), gpgme_strerror (err)); \
        g_string_append (string, filename ? filename : ""); \
        g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, string->str); \
      } \
  } \
  G_STMT_END

#define assert_str_contains(s1, s2) \
  G_STMT_START \
  { \
    const char *__s1 = (s1), *__s2 = (s2); \
    if (strstr (__s1, __s2) == NULL) \
      { \
        g_autoptr (GString) string \
            = g_string_new ("assertion failed (" #s1 " contains " #s2 "): "); \
        g_autofree char *__es1 = g_strescape (__s1, NULL); \
        g_autofree char *__es2 = g_strescape (__s2, NULL); \
        g_string_append_printf (string, "(\"%s\", \"%s\")", __es1, __es2); \
        g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, string->str); \
      } \
  } \
  G_STMT_END

typedef struct
{
  OstreeGpgVerifyResult *result;
} TestFixture;

static OstreeGpgSignatureAttr some_attributes[] = {
  OSTREE_GPG_SIGNATURE_ATTR_VALID,       OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED, OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING, OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP,
};

static void
test_fixture_setup (TestFixture *fixture, gconstpointer user_data)
{
  const char *const *sig_files = user_data;
  gpgme_error_t gpg_error;
  gpgme_data_t data_buffer;
  gpgme_data_t signature_buffer;
  OstreeGpgVerifyResult *result;
  g_autofree char *homedir = NULL;
  GError *local_error = NULL;

  /* Mimic what OstreeGpgVerifier does to create OstreeGpgVerifyResult.
   * We don't use OstreeGpgVerifier directly because we don't need the
   * multiple-keyring workaround and because we want the trust database
   * taken into account, which contains additional data like revocation
   * certificates for certain test cases. */

  homedir = g_test_build_filename (G_TEST_DIST, "tests/gpg-verify-data", NULL);
  gboolean is_ok = g_setenv ("GNUPGHOME", homedir, TRUE);
  g_assert (is_ok == TRUE);

  result = g_initable_new (OSTREE_TYPE_GPG_VERIFY_RESULT, NULL, &local_error, NULL);
  g_assert_no_error (local_error);

  g_autofree char *data_filename = g_build_filename (homedir, "lgpl2", NULL);
  gpg_error = gpgme_data_new_from_file (&data_buffer, data_filename, 1);
  assert_no_gpg_error (gpg_error, data_filename);

  if (sig_files == NULL)
    {
      /* No signature files specified, use full lgpl2.sig file */
      g_autofree char *filename = g_build_filename (homedir, "lgpl2.sig", NULL);
      gpg_error = gpgme_data_new_from_file (&signature_buffer, filename, 1);
      assert_no_gpg_error (gpg_error, filename);
    }
  else
    {
      /* Read all the specified files into the signature buffer */
      gpg_error = gpgme_data_new (&signature_buffer);
      assert_no_gpg_error (gpg_error, NULL);

      for (const char *const *name = sig_files; *name != NULL; name++)
        {
          g_autofree char *path = g_build_filename (homedir, *name, NULL);
          g_autoptr (GFile) sig_file = g_file_new_for_path (path);

          g_autofree char *contents = NULL;
          gsize len;
          g_assert_true (
              g_file_load_contents (sig_file, NULL, &contents, &len, NULL, &local_error));
          g_assert_no_error (local_error);

          char *cur = contents;
          while (len > 0)
            {
              ssize_t written = gpgme_data_write (signature_buffer, cur, len);
              if (written == -1)
                assert_no_gpg_error (gpgme_error_from_syserror (), path);
              cur += written;
              len -= written;
            }
        }

      gpgme_data_seek (signature_buffer, 0, SEEK_SET);
    }

  gpg_error = gpgme_op_verify (result->context, signature_buffer, data_buffer, NULL);
  assert_no_gpg_error (gpg_error, NULL);

  result->details = gpgme_op_verify_result (result->context);
  gpgme_result_ref (result->details);

  gpgme_data_release (data_buffer);
  gpgme_data_release (signature_buffer);

  fixture->result = result;
}

static void
test_fixture_teardown (TestFixture *fixture, gconstpointer user_data)
{
  g_clear_object (&fixture->result);
}

static void
test_check_counts (TestFixture *fixture, gconstpointer user_data)
{
  guint count_all;
  guint count_valid;

  count_all = ostree_gpg_verify_result_count_all (fixture->result);
  count_valid = ostree_gpg_verify_result_count_valid (fixture->result);

  g_assert_cmpint (count_all, ==, 5);
  g_assert_cmpint (count_valid, ==, 1);
}

static void
test_signature_lookup (TestFixture *fixture, gconstpointer user_data)
{
  /* Checking the signature with the revoked key for this case. */
  guint expected_signature_index = 2;

  /* Lowercase letters to ensure OstreeGpgVerifyResult handles it. */
  const char *fingerprint = "68dcc2db4bec5811c2573590bd9d2a44b7f541a6";

  guint signature_index;
  gboolean signature_found;

  /* Lookup full fingerprint. */
  signature_index = 999999;
  signature_found
      = ostree_gpg_verify_result_lookup (fixture->result, fingerprint, &signature_index);
  g_assert_true (signature_found);
  g_assert_cmpint (signature_index, ==, expected_signature_index);

  /* Lookup abbreviated key ID. */
  signature_index = 999999;
  signature_found
      = ostree_gpg_verify_result_lookup (fixture->result, fingerprint + 32, &signature_index);
  g_assert_true (signature_found);
  g_assert_cmpint (signature_index, ==, expected_signature_index);

  /* Bogus fingerprint, index should remain unchanged. */
  signature_index = expected_signature_index = 999999;
  fingerprint = "CAFEBABECAFEBABECAFEBABECAFEBABECAFEBABE";
  signature_found
      = ostree_gpg_verify_result_lookup (fixture->result, fingerprint, &signature_index);
  g_assert_false (signature_found);
  g_assert_cmpint (signature_index, ==, expected_signature_index);
}

static void
test_attribute_basics (TestFixture *fixture, gconstpointer user_data)
{
  guint n_signatures, ii;

  n_signatures = ostree_gpg_verify_result_count_valid (fixture->result);

  for (ii = 0; ii < n_signatures; ii++)
    {
      g_autoptr (GVariant) tuple = NULL;
      const char *attr_string;
      const char *type_string;
      gboolean key_missing;

      tuple = ostree_gpg_verify_result_get_all (fixture->result, ii);

      type_string = g_variant_get_type_string (tuple);
      g_assert_cmpstr (type_string, ==, "(bbbbbsxxsssssxx)");

      /* Check attributes which should be common to all signatures. */

      g_variant_get_child (tuple, OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME, "&s", &attr_string);
      g_assert_cmpstr (attr_string, ==, "RSA");

      g_variant_get_child (tuple, OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME, "&s", &attr_string);
      g_assert_cmpstr (attr_string, ==, "SHA1");

      g_variant_get_child (tuple, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING, "b", &key_missing);

      g_variant_get_child (tuple, OSTREE_GPG_SIGNATURE_ATTR_USER_NAME, "&s", &attr_string);
      if (key_missing)
        g_assert_cmpstr (attr_string, ==, "[unknown name]");
      else
        g_assert_cmpstr (attr_string, ==, "J. Random User");

      g_variant_get_child (tuple, OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL, "&s", &attr_string);
      if (key_missing)
        g_assert_cmpstr (attr_string, ==, "[unknown email]");
      else
        g_assert_cmpstr (attr_string, ==, "testcase@redhat.com");
    }
}

static void
test_valid_signature (TestFixture *fixture, gconstpointer user_data)
{
  guint signature_index = 0;
  g_autoptr (GVariant) tuple = NULL;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_expired;
  gboolean key_revoked;
  gboolean key_missing;
  gint64 key_exp_timestamp;

  tuple = ostree_gpg_verify_result_get (fixture->result, signature_index, some_attributes,
                                        G_N_ELEMENTS (some_attributes));

  g_variant_get (tuple, "(bbbbbx)", &valid, &sig_expired, &key_expired, &key_revoked, &key_missing,
                 &key_exp_timestamp);

  g_assert_true (valid);
  g_assert_false (sig_expired);
  g_assert_false (key_expired);
  g_assert_false (key_revoked);
  g_assert_false (key_missing);
  g_assert_cmpint (key_exp_timestamp, ==, 0);
}

static void
test_expired_key (TestFixture *fixture, gconstpointer user_data)
{
  guint signature_index = 1;
  g_autoptr (GVariant) tuple = NULL;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_expired;
  gboolean key_revoked;
  gboolean key_missing;
  gint64 key_exp_timestamp;

  tuple = ostree_gpg_verify_result_get (fixture->result, signature_index, some_attributes,
                                        G_N_ELEMENTS (some_attributes));

  g_variant_get (tuple, "(bbbbbx)", &valid, &sig_expired, &key_expired, &key_revoked, &key_missing,
                 &key_exp_timestamp);

  g_assert_false (valid);
  g_assert_false (sig_expired);
  g_assert_true (key_expired);
  g_assert_false (key_revoked);
  g_assert_false (key_missing);
  g_assert_cmpint (key_exp_timestamp, ==, 1426782201);
}

static void
test_revoked_key (TestFixture *fixture, gconstpointer user_data)
{
  guint signature_index = 2;
  g_autoptr (GVariant) tuple = NULL;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_expired;
  gboolean key_revoked;
  gboolean key_missing;
  gint64 key_exp_timestamp;

  tuple = ostree_gpg_verify_result_get (fixture->result, signature_index, some_attributes,
                                        G_N_ELEMENTS (some_attributes));

  g_variant_get (tuple, "(bbbbbx)", &valid, &sig_expired, &key_expired, &key_revoked, &key_missing,
                 &key_exp_timestamp);

  g_assert_false (valid);
  g_assert_false (sig_expired);
  g_assert_false (key_expired);
  g_assert_true (key_revoked);
  g_assert_false (key_missing);
  g_assert_cmpint (key_exp_timestamp, ==, 0);
}

static void
test_missing_key (TestFixture *fixture, gconstpointer user_data)
{
  guint signature_index = 3;
  g_autoptr (GVariant) tuple = NULL;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_expired;
  gboolean key_revoked;
  gboolean key_missing;
  gint64 key_exp_timestamp;

  tuple = ostree_gpg_verify_result_get (fixture->result, signature_index, some_attributes,
                                        G_N_ELEMENTS (some_attributes));

  g_variant_get (tuple, "(bbbbbx)", &valid, &sig_expired, &key_expired, &key_revoked, &key_missing,
                 &key_exp_timestamp);

  g_assert_false (valid);
  g_assert_false (sig_expired);
  g_assert_false (key_expired);
  g_assert_false (key_revoked);
  g_assert_true (key_missing);
  g_assert_cmpint (key_exp_timestamp, ==, 0);
}

static void
test_expired_signature (TestFixture *fixture, gconstpointer user_data)
{
  guint signature_index = 4;
  g_autoptr (GVariant) tuple = NULL;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_expired;
  gboolean key_revoked;
  gboolean key_missing;
  gint64 key_exp_timestamp;

  tuple = ostree_gpg_verify_result_get (fixture->result, signature_index, some_attributes,
                                        G_N_ELEMENTS (some_attributes));

  g_variant_get (tuple, "(bbbbbx)", &valid, &sig_expired, &key_expired, &key_revoked, &key_missing,
                 &key_exp_timestamp);

  g_assert_false (valid);
  g_assert_true (sig_expired);
  g_assert_false (key_expired);
  g_assert_false (key_revoked);
  g_assert_false (key_missing);
  g_assert_cmpint (key_exp_timestamp, ==, 0);
}

static void
test_require_valid_signature (TestFixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  gboolean res = ostree_gpg_verify_result_require_valid_signature (fixture->result, &error);
  g_assert_true (res);
  g_assert_no_error (error);
}

static void
test_require_valid_signature_expired_key (TestFixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  gboolean res = ostree_gpg_verify_result_require_valid_signature (fixture->result, &error);
  g_assert_false (res);
  g_assert_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_EXPIRED_KEY);
  assert_str_contains (error->message, "Key expired");
}

static void
test_require_valid_signature_revoked_key (TestFixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  gboolean res = ostree_gpg_verify_result_require_valid_signature (fixture->result, &error);
  g_assert_false (res);
  g_assert_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_REVOKED_KEY);
  assert_str_contains (error->message, "Key revoked");
}

static void
test_require_valid_signature_missing_key (TestFixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  gboolean res = ostree_gpg_verify_result_require_valid_signature (fixture->result, &error);
  g_assert_false (res);
  g_assert_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_MISSING_KEY);
  assert_str_contains (error->message, "public key not found");
}

static void
test_require_valid_signature_expired_signature (TestFixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  gboolean res = ostree_gpg_verify_result_require_valid_signature (fixture->result, &error);
  g_assert_false (res);
  g_assert_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_EXPIRED_SIGNATURE);
  assert_str_contains (error->message, "Signature expired");
}

static void
test_require_valid_signature_expired_missing_key (TestFixture *fixture, gconstpointer user_data)
{
  GError *error = NULL;
  gboolean res = ostree_gpg_verify_result_require_valid_signature (fixture->result, &error);
  g_assert_false (res);

  /*
   * The error will be for the last signature, which is for a missing key, but
   * the message should show both issues.
   */
  g_assert_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_MISSING_KEY);
  assert_str_contains (error->message, "Key expired");
  assert_str_contains (error->message, "public key not found");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  (void)gpgme_check_version (NULL);

  g_test_add ("/gpg-verify-result/check-counts", TestFixture, NULL, test_fixture_setup,
              test_check_counts, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/signature-lookup", TestFixture, NULL, test_fixture_setup,
              test_signature_lookup, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/attribute-basics", TestFixture, NULL, test_fixture_setup,
              test_attribute_basics, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/valid-signature", TestFixture, NULL, test_fixture_setup,
              test_valid_signature, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/expired-key", TestFixture, NULL, test_fixture_setup,
              test_expired_key, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/revoked-key", TestFixture, NULL, test_fixture_setup,
              test_revoked_key, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/missing-key", TestFixture, NULL, test_fixture_setup,
              test_missing_key, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/expired-signature", TestFixture, NULL, test_fixture_setup,
              test_expired_signature, test_fixture_teardown);

  g_test_add ("/gpg-verify-result/require-valid-signature", TestFixture, NULL, test_fixture_setup,
              test_require_valid_signature, test_fixture_teardown);

  const char *expired_key_files[] = { "lgpl2.sig1", NULL };
  g_test_add ("/gpg-verify-result/require-valid-signature-expired-key", TestFixture,
              expired_key_files, test_fixture_setup, test_require_valid_signature_expired_key,
              test_fixture_teardown);

  const char *revoked_key_files[] = { "lgpl2.sig2", NULL };
  g_test_add ("/gpg-verify-result/require-valid-signature-revoked-key", TestFixture,
              revoked_key_files, test_fixture_setup, test_require_valid_signature_revoked_key,
              test_fixture_teardown);

  const char *missing_key_files[] = { "lgpl2.sig3", NULL };
  g_test_add ("/gpg-verify-result/require-valid-signature-missing-key", TestFixture,
              missing_key_files, test_fixture_setup, test_require_valid_signature_missing_key,
              test_fixture_teardown);

  const char *expired_signature_files[] = { "lgpl2.sig4", NULL };
  g_test_add ("/gpg-verify-result/require-valid-signature-expired-signature", TestFixture,
              expired_signature_files, test_fixture_setup,
              test_require_valid_signature_expired_signature, test_fixture_teardown);

  const char *expired_missing_key_files[] = { "lgpl2.sig1", "lgpl2.sig3", NULL };
  g_test_add ("/gpg-verify-result/require-valid-signature-expired-missing-key", TestFixture,
              expired_missing_key_files, test_fixture_setup,
              test_require_valid_signature_expired_missing_key, test_fixture_teardown);

  return g_test_run ();
}
