/*
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"
#define _OSTREE_PUBLIC
#include "../src/libostree/ostree-bootconfig-parser.c"

static void
test_parse_tries_valid (void)
{
  guint64 left, done;
  parse_bootloader_tries ("foo", &left, &done);
  g_assert_cmpuint (left, ==, 0);
  g_assert_cmpuint (done, ==, 0);

  parse_bootloader_tries ("foo+1", &left, &done);
  g_assert_cmpuint (left, ==, 1);
  g_assert_cmpuint (done, ==, 0);

  parse_bootloader_tries ("foo+1-2", &left, &done);
  g_assert_cmpuint (left, ==, 1);
  g_assert_cmpuint (done, ==, 2);

  parse_bootloader_tries ("foo+1-2.conf", &left, &done);
  g_assert_cmpuint (left, ==, 1);
  g_assert_cmpuint (done, ==, 2);
}

static void
test_parse_tries_invalid (void)
{
  guint64 left, done;

  parse_bootloader_tries ("foo+1-", &left, &done);
  g_assert_cmpuint (left, ==, 0);
  g_assert_cmpuint (done, ==, 0);

  parse_bootloader_tries ("foo+-1", &left, &done);
  g_assert_cmpuint (left, ==, 0);
  g_assert_cmpuint (done, ==, 0);

  parse_bootloader_tries ("foo+1-a", &left, &done);
  g_assert_cmpuint (left, ==, 0);
  g_assert_cmpuint (done, ==, 0);

  parse_bootloader_tries ("foo+a-1", &left, &done);
  g_assert_cmpuint (left, ==, 0);
  g_assert_cmpuint (done, ==, 0);
}

static void
test_comments_empty (void)
{
  /* A bootconfig with no magic comments should return NULL from get_comments_variant */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Fedora Linux 40");
  ostree_bootconfig_parser_set (parser, "version", "1");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc rw quiet");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz-6.8.0");
  ostree_bootconfig_parser_set (parser, "initrd", "/initramfs-6.8.0.img");

  GVariant *comments = _ostree_bootconfig_parser_get_comments_variant (parser);
  g_assert_null (comments);

  /* get_comment should also return NULL */
  g_assert_null (ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned"));

  g_object_unref (parser);
}

static void
test_comments_set_and_get (void)
{
  /* Test setting and getting magic comments via the private API */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();

  /* Set a comment */
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-tuned",
                                        "nohz=full isolcpus=1-3");
  const char *val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");

  /* Set a second comment from a different source */
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-dracut",
                                        "rd.driver.pre=vfio-pci");
  val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-dracut");
  g_assert_cmpstr (val, ==, "rd.driver.pre=vfio-pci");

  /* First comment should still be there */
  val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");

  /* Replace the first comment */
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-tuned",
                                        "nohz=on rcu_nocbs=2-7");
  val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=on rcu_nocbs=2-7");

  /* Non-existent source returns NULL */
  g_assert_null (
      ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-nonexistent"));

  g_object_unref (parser);
}

static void
test_comments_variant_with_entries (void)
{
  /* Test that get_comments_variant returns all set comments */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-tuned",
                                        "nohz=full isolcpus=1-3");
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-dracut",
                                        "rd.driver.pre=vfio-pci");

  GVariant *comments = _ostree_bootconfig_parser_get_comments_variant (parser);
  g_assert_nonnull (comments);
  GVariant *comments_owned = g_variant_ref_sink (comments);

  g_assert_true (g_variant_is_of_type (comments_owned, G_VARIANT_TYPE ("a{ss}")));
  g_assert_cmpuint (g_variant_n_children (comments_owned), ==, 2);

  GVariantIter iter;
  const char *key, *val;
  gboolean found_tuned = FALSE, found_dracut = FALSE;
  g_variant_iter_init (&iter, comments_owned);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &val))
    {
      if (g_str_equal (key, "x-ostree-options-source-tuned"))
        {
          g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");
          found_tuned = TRUE;
        }
      else if (g_str_equal (key, "x-ostree-options-source-dracut"))
        {
          g_assert_cmpstr (val, ==, "rd.driver.pre=vfio-pci");
          found_dracut = TRUE;
        }
      else
        {
          g_assert_not_reached ();
        }
    }
  g_assert_true (found_tuned);
  g_assert_true (found_dracut);

  g_variant_unref (comments_owned);
  g_object_unref (parser);
}

static void
test_comments_parse_from_string (void)
{
  /* Test that magic comments are parsed from BLS file content */
  const char *bls_content = "title Fedora Linux 40\n"
                            "version 6.8.0-300.fc40.x86_64\n"
                            "linux /vmlinuz-6.8.0\n"
                            "initrd /initramfs-6.8.0.img\n"
                            "options root=UUID=abc rw nohz=full\n"
                            "# x-ostree-options-source-tuned nohz=full isolcpus=1-3\n"
                            "# This is a regular comment that should be dropped\n"
                            "# x-ostree-options-source-dracut rd.driver.pre=vfio-pci\n";

  /* Write the content to a temp file and parse it */
  g_autofree char *tmppath = NULL;
  g_autoptr (GError) error = NULL;
  int fd = g_file_open_tmp ("bls-XXXXXX.conf", &tmppath, &error);
  g_assert_no_error (error);

  gsize len = strlen (bls_content);
  g_assert_cmpint (write (fd, bls_content, len), ==, (gssize)len);
  close (fd);

  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  gboolean ret = ostree_bootconfig_parser_parse_at (parser, AT_FDCWD, tmppath, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* Standard keys should be parsed */
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser, "title"), ==, "Fedora Linux 40");

  /* Magic comments should be preserved */
  const char *val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");

  val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-dracut");
  g_assert_cmpstr (val, ==, "rd.driver.pre=vfio-pci");

  /* Regular comments should NOT be preserved */
  GVariant *comments = _ostree_bootconfig_parser_get_comments_variant (parser);
  g_assert_nonnull (comments);
  GVariant *comments_owned = g_variant_ref_sink (comments);
  g_assert_cmpuint (g_variant_n_children (comments_owned), ==, 2);
  g_variant_unref (comments_owned);

  unlink (tmppath);
  g_object_unref (parser);
}

static void
test_comments_write_roundtrip (void)
{
  /* Test that magic comments survive a parse → write → parse roundtrip */
  const char *bls_content = "title Fedora Linux 40\n"
                            "linux /vmlinuz-6.8.0\n"
                            "initrd /initramfs-6.8.0.img\n"
                            "options root=UUID=abc rw\n"
                            "# x-ostree-options-source-tuned nohz=full isolcpus=1-3\n";

  /* Write initial content */
  g_autofree char *tmppath = NULL;
  g_autoptr (GError) error = NULL;
  int fd = g_file_open_tmp ("bls-roundtrip-XXXXXX.conf", &tmppath, &error);
  g_assert_no_error (error);
  gsize len = strlen (bls_content);
  g_assert_cmpint (write (fd, bls_content, len), ==, (gssize)len);
  close (fd);

  /* Parse */
  OstreeBootconfigParser *parser1 = ostree_bootconfig_parser_new ();
  gboolean ret = ostree_bootconfig_parser_parse_at (parser1, AT_FDCWD, tmppath, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* Write to a new file */
  g_autofree char *tmppath2 = NULL;
  fd = g_file_open_tmp ("bls-roundtrip2-XXXXXX.conf", &tmppath2, &error);
  g_assert_no_error (error);
  close (fd);

  ret = ostree_bootconfig_parser_write_at (parser1, AT_FDCWD, tmppath2, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* Parse the written file */
  OstreeBootconfigParser *parser2 = ostree_bootconfig_parser_new ();
  ret = ostree_bootconfig_parser_parse_at (parser2, AT_FDCWD, tmppath2, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* Verify the magic comment survived */
  const char *val = ostree_bootconfig_parser_get_comment (parser2, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");

  /* Verify standard keys survived */
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser2, "title"), ==, "Fedora Linux 40");
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser2, "options"), ==, "root=UUID=abc rw");

  unlink (tmppath);
  unlink (tmppath2);
  g_object_unref (parser1);
  g_object_unref (parser2);
}

static void
test_comments_staging_roundtrip (void)
{
  /* Test that comments can be serialized to a variant and restored,
   * simulating the staged deployment roundtrip.
   */
  OstreeBootconfigParser *original = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (original, "options", "root=UUID=abc rw");
  ostree_bootconfig_parser_set (original, "linux", "/vmlinuz");
  ostree_bootconfig_parser_set_comment (original, "x-ostree-options-source-tuned",
                                        "nohz=full isolcpus=1-3");

  /* Serialize to variant (what ostree_sysroot_stage_tree_with_options does) */
  GVariant *comments = _ostree_bootconfig_parser_get_comments_variant (original);
  g_assert_nonnull (comments);
  GVariant *comments_owned = g_variant_ref_sink (comments);

  /* Create a new parser (simulating _ostree_sysroot_reload_staged) */
  OstreeBootconfigParser *restored = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (restored, "options", "root=UUID=abc rw nohz=full isolcpus=1-3");

  /* Restore comments from variant */
  GVariantIter iter;
  const char *key, *value;
  g_variant_iter_init (&iter, comments_owned);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &value))
    ostree_bootconfig_parser_set_comment (restored, key, value);

  /* Verify the comment survived the roundtrip */
  const char *val
      = ostree_bootconfig_parser_get_comment (restored, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");

  /* Standard keys should also be present */
  g_assert_cmpstr (ostree_bootconfig_parser_get (restored, "options"), ==,
                   "root=UUID=abc rw nohz=full isolcpus=1-3");

  g_variant_unref (comments_owned);
  g_object_unref (original);
  g_object_unref (restored);
}

static void
test_comments_empty_value (void)
{
  /* Test the "clear all kargs from source" flow: setting a comment with
   * NULL or empty value should produce a key-only comment entry.
   * This is the path TuneD takes when disabling a profile — it removes
   * its owned kargs but must keep the source marker so rpm-ostree knows
   * the source has no kargs left.
   */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();

  /* Set a comment with a value first */
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-tuned",
                                        "nohz=full isolcpus=1-3");
  const char *val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");

  /* Now clear it by setting empty value — simulates "disable source" */
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-tuned", "");
  val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_nonnull (val);
  g_assert_cmpstr (val, ==, "");

  /* Setting with NULL should also produce a key-only entry */
  ostree_bootconfig_parser_set_comment (parser, "x-ostree-options-source-tuned", NULL);
  val = ostree_bootconfig_parser_get_comment (parser, "x-ostree-options-source-tuned");
  g_assert_nonnull (val);
  g_assert_cmpstr (val, ==, "");

  /* The variant should include the key with an empty string value */
  GVariant *comments = _ostree_bootconfig_parser_get_comments_variant (parser);
  g_assert_nonnull (comments);
  GVariant *comments_owned = g_variant_ref_sink (comments);
  g_assert_cmpuint (g_variant_n_children (comments_owned), ==, 1);

  GVariantIter iter;
  const char *key, *v;
  g_variant_iter_init (&iter, comments_owned);
  g_assert_true (g_variant_iter_next (&iter, "{&s&s}", &key, &v));
  g_assert_cmpstr (key, ==, "x-ostree-options-source-tuned");
  g_assert_cmpstr (v, ==, "");

  g_variant_unref (comments_owned);
  g_object_unref (parser);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/bootconfig-parser/tries/valid", test_parse_tries_valid);
  g_test_add_func ("/bootconfig-parser/tries/invalid", test_parse_tries_invalid);
  g_test_add_func ("/bootconfig-parser/comments/empty", test_comments_empty);
  g_test_add_func ("/bootconfig-parser/comments/set-and-get", test_comments_set_and_get);
  g_test_add_func ("/bootconfig-parser/comments/variant-with-entries",
                   test_comments_variant_with_entries);
  g_test_add_func ("/bootconfig-parser/comments/parse-from-string",
                   test_comments_parse_from_string);
  g_test_add_func ("/bootconfig-parser/comments/write-roundtrip", test_comments_write_roundtrip);
  g_test_add_func ("/bootconfig-parser/comments/staging-roundtrip",
                   test_comments_staging_roundtrip);
  g_test_add_func ("/bootconfig-parser/comments/empty-value", test_comments_empty_value);
  return g_test_run ();
}
