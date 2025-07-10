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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/bootconfig-parser/tries/valid", test_parse_tries_valid);
  g_test_add_func ("/bootconfig-parser/tries/invalid", test_parse_tries_invalid);
  return g_test_run ();
}
