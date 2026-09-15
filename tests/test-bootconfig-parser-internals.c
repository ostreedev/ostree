/*
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"
#define _OSTREE_PUBLIC
#include "../src/libostree/ostree-autocleanups.h"
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
  /* A bootconfig with no x-prefixed keys should return NULL */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Fedora Linux 43");
  ostree_bootconfig_parser_set (parser, "version", "1");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc rw quiet");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz-6.8.0");
  ostree_bootconfig_parser_set (parser, "initrd", "/initramfs-6.8.0.img");

  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_null (extra);

  g_object_unref (parser);
}

static void
test_extra_keys_variant_with_extension_keys (void)
{
  /* Standard keys should be excluded, only extension keys returned */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Fedora Linux 43");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc rw");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz-6.8.0");
  ostree_bootconfig_parser_set (parser, "x-options-source-tuned", "nohz=full isolcpus=1-3");
  ostree_bootconfig_parser_set (parser, "x-options-source-dracut", "rd.driver.pre=vfio-pci");

  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_nonnull (extra);

  GVariant *extra_owned = g_variant_ref_sink (extra);

  /* Should be a{ss} with exactly 2 entries */
  g_assert_true (g_variant_is_of_type (extra_owned, G_VARIANT_TYPE ("a{ss}")));
  g_assert_cmpuint (g_variant_n_children (extra_owned), ==, 2);

  /* Verify the contents */
  GVariantIter iter;
  const char *key, *val;
  gboolean found_tuned = FALSE, found_dracut = FALSE;
  g_variant_iter_init (&iter, extra_owned);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &val))
    {
      if (g_str_equal (key, "x-options-source-tuned"))
        {
          g_assert_cmpstr (val, ==, "nohz=full isolcpus=1-3");
          found_tuned = TRUE;
        }
      else if (g_str_equal (key, "x-options-source-dracut"))
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
test_extra_keys_variant_standard_excluded (void)
{
  /* Standard BLS keys (title, version, options, linux, initrd, devicetree)
   * should be excluded. All other keys should be preserved.
   */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (parser, "title", "Test");
  ostree_bootconfig_parser_set (parser, "version", "1.0");
  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc");
  ostree_bootconfig_parser_set (parser, "linux", "/vmlinuz");
  ostree_bootconfig_parser_set (parser, "initrd", "/initramfs.img");
  ostree_bootconfig_parser_set (parser, "devicetree", "/dtb");

  /* Only standard keys -- should return NULL */
  GVariant *extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_null (extra);

  /* Add non-standard keys -- all should be preserved */
  ostree_bootconfig_parser_set (parser, "my-custom-key", "some-value");
  ostree_bootconfig_parser_set (parser, "x-options-source-tuned", "nohz=full");

  extra = _ostree_bootconfig_parser_get_extra_keys_variant (parser);
  g_assert_nonnull (extra);
  GVariant *extra_owned = g_variant_ref_sink (extra);

  g_assert_cmpuint (g_variant_n_children (extra_owned), ==, 2);

  GVariantIter iter;
  const char *key, *val;
  gboolean found_custom = FALSE, found_tuned = FALSE;
  g_variant_iter_init (&iter, extra_owned);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &val))
    {
      if (g_str_equal (key, "my-custom-key"))
        {
          g_assert_cmpstr (val, ==, "some-value");
          found_custom = TRUE;
        }
      else if (g_str_equal (key, "x-options-source-tuned"))
        {
          g_assert_cmpstr (val, ==, "nohz=full");
          found_tuned = TRUE;
        }
      else
        g_assert_not_reached ();
    }
  g_assert_true (found_custom);
  g_assert_true (found_tuned);

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
  ostree_bootconfig_parser_set (original, "x-options-source-tuned", "nohz=full isolcpus=1-3");

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

  /* Verify the extension key survived the roundtrip */
  g_assert_cmpstr (ostree_bootconfig_parser_get (restored, "x-options-source-tuned"), ==,
                   "nohz=full isolcpus=1-3");
  /* Standard keys should also be present */
  g_assert_cmpstr (ostree_bootconfig_parser_get (restored, "options"), ==,
                   "root=UUID=abc rw nohz=full isolcpus=1-3");

  g_variant_unref (extra_owned);
  g_object_unref (original);
  g_object_unref (restored);
}

static void
test_extra_keys_parse_write_roundtrip (void)
{
  /* Test that x-prefixed keys survive a parse -> write -> parse roundtrip
   * via the BLS file format.
   */
  const char *bls_content = "title Fedora Linux 43\n"
                            "version 6.8.0-300.fc40.x86_64\n"
                            "linux /vmlinuz-6.8.0\n"
                            "initrd /initramfs-6.8.0.img\n"
                            "options root=UUID=abc rw nohz=full\n"
                            "x-options-source-tuned nohz=full\n";

  /* Write the BLS content to a temp file */
  g_autofree char *tmpdir = g_dir_make_tmp ("ostree-test-XXXXXX", NULL);
  g_assert_nonnull (tmpdir);
  g_autofree char *tmpfile = g_build_filename (tmpdir, "ostree-test.conf", NULL);
  g_assert_true (g_file_set_contents (tmpfile, bls_content, -1, NULL));

  /* Parse */
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  g_assert_true (ostree_bootconfig_parser_parse_at (parser, AT_FDCWD, tmpfile, NULL, NULL));

  /* The x-prefixed key should have been parsed */
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser, "x-options-source-tuned"), ==,
                   "nohz=full");
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser, "options"), ==,
                   "root=UUID=abc rw nohz=full");

  /* Write it back out */
  g_autofree char *outfile = g_build_filename (tmpdir, "ostree-test-out.conf", NULL);
  g_assert_true (ostree_bootconfig_parser_write_at (parser, AT_FDCWD, outfile, NULL, NULL));

  /* Parse the output and verify the key survived */
  OstreeBootconfigParser *parser2 = ostree_bootconfig_parser_new ();
  g_assert_true (ostree_bootconfig_parser_parse_at (parser2, AT_FDCWD, outfile, NULL, NULL));
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser2, "x-options-source-tuned"), ==,
                   "nohz=full");
  g_assert_cmpstr (ostree_bootconfig_parser_get (parser2, "options"), ==,
                   "root=UUID=abc rw nohz=full");

  g_object_unref (parser);
  g_object_unref (parser2);
  (void)unlink (tmpfile);
  (void)unlink (outfile);
  (void)rmdir (tmpdir);
}

/* Helpers for the select-staged-extra-keys tests below */
static GVariant *
make_extra (const char *k1, const char *v1, const char *k2, const char *v2)
{
  g_auto (GVariantBuilder) builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&builder, "{ss}", k1, v1);
  if (k2)
    g_variant_builder_add (&builder, "{ss}", k2, v2);
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static const char *
extra_lookup (GVariant *extra, const char *key)
{
  const char *val = NULL;
  g_assert_true (g_variant_lookup (extra, key, "&s", &val));
  return val;
}

static gboolean
extra_contains (GVariant *extra, const char *key)
{
  const char *val = NULL;
  return g_variant_lookup (extra, key, "&s", &val);
}

/* Simulate the booted deployment's bootconfig as loaded from disk */
static OstreeBootconfigParser *
make_booted_bootconfig (const char *extra_key, const char *extra_val)
{
  g_autofree char *tmpdir = g_dir_make_tmp ("ostree-test-XXXXXX", NULL);
  g_assert_nonnull (tmpdir);
  g_autofree char *tmpfile = g_build_filename (tmpdir, "ostree-1.conf", NULL);
  g_autoptr (GString) bls = g_string_new ("title Fedora Linux 43\n"
                                          "version 6.8.0-300.fc40.x86_64\n"
                                          "linux /vmlinuz-6.8.0\n"
                                          "initrd /initramfs-6.8.0.img\n"
                                          "options root=UUID=abc rw\n");
  if (extra_key)
    g_string_append_printf (bls, "%s %s\n", extra_key, extra_val);
  g_assert_true (g_file_set_contents (tmpfile, bls->str, -1, NULL));

  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();
  g_assert_true (ostree_bootconfig_parser_parse_at (parser, AT_FDCWD, tmpfile, NULL, NULL));
  (void)unlink (tmpfile);
  (void)rmdir (tmpdir);
  return parser;
}

static void
test_extra_keys_modified_flag (void)
{
  /* Parsing from disk does not count as a modification; setting a standard
   * key does not either; setting an extension key does.  clone() carries
   * the flag, clear() resets it.
   */
  g_autoptr (OstreeBootconfigParser) parser
      = make_booted_bootconfig ("x-options-source-tuned", "nohz=full");
  g_assert_false (_ostree_bootconfig_parser_get_extra_keys_modified (parser));

  ostree_bootconfig_parser_set (parser, "options", "root=UUID=abc rw quiet");
  g_assert_false (_ostree_bootconfig_parser_get_extra_keys_modified (parser));

  ostree_bootconfig_parser_set (parser, "x-options-source-tuned", "nohz=on");
  g_assert_true (_ostree_bootconfig_parser_get_extra_keys_modified (parser));

  g_autoptr (OstreeBootconfigParser) clone = ostree_bootconfig_parser_clone (parser);
  g_assert_true (_ostree_bootconfig_parser_get_extra_keys_modified (clone));

  _ostree_bootconfig_parser_clear_extra_keys_modified (parser);
  g_assert_false (_ostree_bootconfig_parser_get_extra_keys_modified (parser));
  /* The clone is independent */
  g_assert_true (_ostree_bootconfig_parser_get_extra_keys_modified (clone));

  /* The keys ostree rebuilds at finalization are standard, not extension */
  g_autoptr (OstreeBootconfigParser) fresh = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (fresh, "aboot", "/x/aboot.img");
  ostree_bootconfig_parser_set (fresh, "abootcfg", "/x/aboot.cfg");
  ostree_bootconfig_parser_set (fresh, "fdtdir", "/x/dtb");
  ostree_bootconfig_parser_set (fresh, "devicetree", "/x/foo.dtb");
  g_assert_false (_ostree_bootconfig_parser_get_extra_keys_modified (fresh));
  g_assert_null (_ostree_bootconfig_parser_get_extra_keys_variant (fresh));
}

static void
test_select_staged_no_sources (void)
{
  /* Nothing anywhere: no merge deployment, nothing previously staged */
  g_assert_null (_ostree_bootconfig_parser_select_staged_extra_keys (NULL, NULL));

  /* Merge deployment without extension keys and nothing staged */
  g_autoptr (OstreeBootconfigParser) booted = make_booted_bootconfig (NULL, NULL);
  g_assert_null (_ostree_bootconfig_parser_select_staged_extra_keys (booted, NULL));
}

static void
test_select_staged_first_staging (void)
{
  /* First staging of the boot by an unaware consumer: inherit the on-disk
   * keys of the merge deployment.
   */
  g_autoptr (OstreeBootconfigParser) booted
      = make_booted_bootconfig ("x-options-source-tuned", "nohz=full");
  g_autoptr (GVariant) extra = _ostree_bootconfig_parser_select_staged_extra_keys (booted, NULL);
  g_assert_nonnull (extra);
  g_assert_false (g_variant_is_floating (extra));
  g_assert_cmpuint (g_variant_n_children (extra), ==, 1);
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-tuned"), ==, "nohz=full");
}

static void
test_select_staged_aware_consumer_wins (void)
{
  /* The #3609 regression: bootc staged a tombstone for dracut earlier in
   * this boot, then re-adds dracut.  Its in-memory update on the merge
   * deployment must win over the stale previously staged tombstone.
   */
  g_autoptr (OstreeBootconfigParser) booted
      = make_booted_bootconfig ("x-options-source-tuned", "nohz=full");
  g_autoptr (GVariant) staged
      = make_extra ("x-options-source-tuned", "nohz=full", "x-options-source-dracut", "");

  ostree_bootconfig_parser_set (booted, "x-options-source-tuned", "nohz=full");
  ostree_bootconfig_parser_set (booted, "x-options-source-dracut", "rd.driver.pre=vfio-pci");

  g_autoptr (GVariant) extra = _ostree_bootconfig_parser_select_staged_extra_keys (booted, staged);
  g_assert_nonnull (extra);
  g_assert_cmpuint (g_variant_n_children (extra), ==, 2);
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-dracut"), ==, "rd.driver.pre=vfio-pci");
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-tuned"), ==, "nohz=full");
}

static void
test_select_staged_aware_consumer_same_as_disk (void)
{
  /* The aware consumer sets a value identical to what is on disk, after a
   * different value was staged earlier this boot.  A "did it change vs.
   * disk" heuristic would wrongly pick the staged value; the modified flag
   * must make the caller's set win.
   */
  g_autoptr (OstreeBootconfigParser) booted
      = make_booted_bootconfig ("x-options-source-tuned", "nohz=full");
  g_autoptr (GVariant) staged = make_extra ("x-options-source-tuned", "nohz=on", NULL, NULL);

  ostree_bootconfig_parser_set (booted, "x-options-source-tuned", "nohz=full");

  g_autoptr (GVariant) extra = _ostree_bootconfig_parser_select_staged_extra_keys (booted, staged);
  g_assert_nonnull (extra);
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-tuned"), ==, "nohz=full");
}

static void
test_select_staged_unaware_consumer_inherits_staged (void)
{
  /* Cross-consumer: bootc staged tuned + dracut earlier this boot, and the
   * booted entry only carries an old tombstone.  rpm-ostree now re-stages
   * without touching extension keys: it must inherit the previously staged
   * set, not the on-disk tombstone (which is what made PR #3611's TMT test
   * fail at boot 5).
   */
  g_autoptr (OstreeBootconfigParser) booted
      = make_booted_bootconfig ("x-options-source-crosstest", "");
  g_autoptr (GVariant) staged = make_extra ("x-options-source-tuned", "nohz=on rcu_nocbs=2-7",
                                            "x-options-source-dracut", "rd.driver.pre=vfio-pci");

  /* rpm-ostree does touch "options" -- a standard key -- which must not
   * count as an extension key modification.
   */
  ostree_bootconfig_parser_set (booted, "options", "root=UUID=abc rw localkarg=fromrpm");

  g_autoptr (GVariant) extra = _ostree_bootconfig_parser_select_staged_extra_keys (booted, staged);
  g_assert_nonnull (extra);
  g_assert_true (extra == staged);
  g_assert_cmpuint (g_variant_n_children (extra), ==, 2);
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-tuned"), ==, "nohz=on rcu_nocbs=2-7");
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-dracut"), ==, "rd.driver.pre=vfio-pci");
  g_assert_false (extra_contains (extra, "x-options-source-crosstest"));
}

static void
test_select_staged_unaware_consumer_no_merge_deployment (void)
{
  /* No merge deployment at all, but something was staged earlier */
  g_autoptr (GVariant) staged = make_extra ("x-options-source-tuned", "nohz=on", NULL, NULL);
  g_autoptr (GVariant) extra = _ostree_bootconfig_parser_select_staged_extra_keys (NULL, staged);
  g_assert_true (extra == staged);
}

static void
test_select_staged_restored_is_not_modified (void)
{
  /* Restoring bootconfig-extra onto a deployment (as
   * _ostree_sysroot_reload_staged() does) then clearing the flag means a
   * later staging with that deployment as merge deployment is treated as
   * unaware: it should prefer the previously staged data.
   */
  g_autoptr (OstreeBootconfigParser) restored = ostree_bootconfig_parser_new ();
  ostree_bootconfig_parser_set (restored, "options", "root=UUID=abc rw");
  ostree_bootconfig_parser_set (restored, "x-options-source-tuned", "nohz=full");
  _ostree_bootconfig_parser_clear_extra_keys_modified (restored);

  g_autoptr (GVariant) staged = make_extra ("x-options-source-tuned", "nohz=on", NULL, NULL);
  g_autoptr (GVariant) extra
      = _ostree_bootconfig_parser_select_staged_extra_keys (restored, staged);
  g_assert_cmpstr (extra_lookup (extra, "x-options-source-tuned"), ==, "nohz=on");

  /* ...and a consumer write after the restore flips it back to authoritative */
  ostree_bootconfig_parser_set (restored, "x-options-source-tuned", "nohz=off");
  g_autoptr (GVariant) extra2
      = _ostree_bootconfig_parser_select_staged_extra_keys (restored, staged);
  g_assert_cmpstr (extra_lookup (extra2, "x-options-source-tuned"), ==, "nohz=off");
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/bootconfig-parser/tries/valid", test_parse_tries_valid);
  g_test_add_func ("/bootconfig-parser/tries/invalid", test_parse_tries_invalid);
  g_test_add_func ("/bootconfig-parser/extra-keys/empty", test_extra_keys_variant_empty);
  g_test_add_func ("/bootconfig-parser/extra-keys/with-extension-keys",
                   test_extra_keys_variant_with_extension_keys);
  g_test_add_func ("/bootconfig-parser/extra-keys/standard-excluded",
                   test_extra_keys_variant_standard_excluded);
  g_test_add_func ("/bootconfig-parser/extra-keys/roundtrip", test_extra_keys_roundtrip);
  g_test_add_func ("/bootconfig-parser/extra-keys/parse-write-roundtrip",
                   test_extra_keys_parse_write_roundtrip);
  g_test_add_func ("/bootconfig-parser/extra-keys/modified-flag", test_extra_keys_modified_flag);
  g_test_add_func ("/bootconfig-parser/select-staged/no-sources", test_select_staged_no_sources);
  g_test_add_func ("/bootconfig-parser/select-staged/first-staging",
                   test_select_staged_first_staging);
  g_test_add_func ("/bootconfig-parser/select-staged/aware-consumer-wins",
                   test_select_staged_aware_consumer_wins);
  g_test_add_func ("/bootconfig-parser/select-staged/aware-consumer-same-as-disk",
                   test_select_staged_aware_consumer_same_as_disk);
  g_test_add_func ("/bootconfig-parser/select-staged/unaware-consumer-inherits-staged",
                   test_select_staged_unaware_consumer_inherits_staged);
  g_test_add_func ("/bootconfig-parser/select-staged/unaware-consumer-no-merge-deployment",
                   test_select_staged_unaware_consumer_no_merge_deployment);
  g_test_add_func ("/bootconfig-parser/select-staged/restored-is-not-modified",
                   test_select_staged_restored_is_not_modified);
  return g_test_run ();
}
