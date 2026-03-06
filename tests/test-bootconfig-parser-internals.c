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
test_extra_keys_variant_empty (void)
{
  /* A bootconfig with no allowlisted keys should return NULL */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Fedora Linux 40");
  ostree_bootconfig_parser_set (parser, "version", "1");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc rw quiet");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz-6.8.0");
  ostree_bootconfig_parser_set (parser, "initrd", "/initramfs-6.8.0.img");

  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_null (extra);

  g_object_unref (parser);
}

static void
test_extra_keys_variant_with_custom (void)
{
  /* Only keys matching the allowlist (ostree-source-*) should be returned */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Fedora Linux 40");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc rw");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz-6.8.0");
  ostree_bootconfig_parser_set (parser, "ostree-source-tuned", "nohz=full isolcpus=1-3");
  ostree_bootconfig_parser_set (parser, "ostree-source-dracut", "rd.driver.pre=vfio-pci");

  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_nonnull (extra);

  /* Wrap it so it gets freed */
  GVariant *extra_owned = g_variant_ref_sink (extra);

  /* Should be a{ss} with exactly 2 entries */
  g_assert_true (g_variant_is_of_type (extra_owned, G_VARIANT_TYPE ("a{ss}")));
  g_assert_cmpuint (g_variant_n_children (extra_owned), ==, 2);

  /* Verify the contents - iterate and check both keys exist */
  GVariantIter iter;
  const char *key, *val;
  gboolean found_tuned = FALSE, found_dracut = FALSE;
  g_variant_iter_init (&iter, extra_owned);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &val))
    {
      if (g_str_equal (key, "ostree-source-tuned"))
        {
          g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");
          found_tuned = TRUE;
        }
      else if (g_str_equal (key, "ostree-source-dracut"))
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

  g_variant_unref (extra_owned);
  g_object_unref (parser);
}

static void
test_extra_keys_variant_non_allowlisted (void)
{
  /* Keys that don't match the allowlist (ostree-source-*) should be excluded,
   * even if they are non-standard custom keys.
   */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Test");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz");
  ostree_bootconfig_parser_set (parser, "my-custom-key", "some-value");
  ostree_bootconfig_parser_set (parser, "vendor-extension", "data");

  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_null (extra);

  /* Now add one allowlisted key — only it should appear */
  ostree_bootconfig_parser_set (parser, "ostree-source-tuned", "nohz=full");

  extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_nonnull (extra);
  GVariant *extra_owned = g_variant_ref_sink (extra);

  g_assert_cmpuint (g_variant_n_children (extra_owned), ==, 1);
  const char *key, *val;
  GVariantIter iter;
  g_variant_iter_init (&iter, extra_owned);
  g_assert_true (g_variant_iter_next (&iter, "{&s&s}", &key, &val));
  g_assert_cmpstr (key, ==, "ostree-source-tuned");
  g_assert_cmpstr (val, ==, "nohz=full");

  g_variant_unref (extra_owned);
  g_object_unref (parser);
}

static void
test_extra_keys_roundtrip (void)
{
  /* Test that extra keys can be serialized to a variant and restored */
  OstreeBootconfigParser *original = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (original, "options", "root=UUID=abc rw");
  ostree_bootconfig_parser_set (original, "linux", "/vmlinuz");
  ostree_bootconfig_parser_set (original, "ostree-source-tuned", "nohz=full isolcpus=1-3");

  /* Serialize */
  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (original);
  g_assert_nonnull (extra);
  GVariant *extra_owned = g_variant_ref_sink (extra);

  /* Create a new parser (simulating deserialization) with only standard keys */
  OstreeBootconfigParser *restored = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (restored, "options", "root=UUID=abc rw nohz=full isolcpus=1-3");

  /* Restore extra keys from variant */
  GVariantIter iter;
  const char *key, *value;
  g_variant_iter_init (&iter, extra_owned);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &value))
    ostree_bootconfig_parser_set (restored, key, value);

  /* Verify the custom key survived the roundtrip */
  g_assert_cmpstr (ostree_bootconfig_parser_get (restored, "ostree-source-tuned"), ==,
                   "nohz=full isolcpus=1-3");
  /* Standard keys should also be present */
  g_assert_cmpstr (ostree_bootconfig_parser_get (restored, "options"), ==,
                   "root=UUID=abc rw nohz=full isolcpus=1-3");

  g_variant_unref (extra_owned);
  g_object_unref (original);
  g_object_unref (restored);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/bootconfig-parser/tries/valid", test_parse_tries_valid);
  g_test_add_func ("/bootconfig-parser/tries/invalid", test_parse_tries_invalid);
  g_test_add_func ("/bootconfig-parser/extra-keys/empty", test_extra_keys_variant_empty);
  g_test_add_func ("/bootconfig-parser/extra-keys/with-custom",
                   test_extra_keys_variant_with_custom);
  g_test_add_func ("/bootconfig-parser/extra-keys/non-allowlisted",
                   test_extra_keys_variant_non_allowlisted);
  g_test_add_func ("/bootconfig-parser/extra-keys/roundtrip", test_extra_keys_roundtrip);
  return g_test_run ();
}
