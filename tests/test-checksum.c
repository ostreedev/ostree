/*
 * Copyright (C) 2015 Red Hat, Inc.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ostree-core-private.h"

static void
test_ostree_parse_delta_name (void)
{
  {
    g_autofree char *from = NULL;
    g_autofree char *to = NULL;
    g_assert (_ostree_parse_delta_name ("30d13b73cfe1e6988ffc345eac905f82a18def8ef1f0666fc392019e9eac388d", &from, &to, NULL));
    g_assert_cmpstr (to, ==, "30d13b73cfe1e6988ffc345eac905f82a18def8ef1f0666fc392019e9eac388d");
    g_assert_null (from);
  }

  {
    g_autofree char *from = NULL;
    g_autofree char *to = NULL;
    g_assert (_ostree_parse_delta_name ("30d13b73cfe1e6988ffc345eac905f82a18def8ef1f0666fc392019e9eac388d-5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03", &from, &to, NULL));
    g_assert_cmpstr (from, ==, "30d13b73cfe1e6988ffc345eac905f82a18def8ef1f0666fc392019e9eac388d");
    g_assert_cmpstr (to, ==, "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03");
  }

  {
    g_autofree char *from = NULL;
    g_autofree char *to = NULL;
    g_assert (!_ostree_parse_delta_name ("", &from, &to, NULL));
    g_assert_null (from);
    g_assert_null (to);
  }

  {
    g_autofree char *from = NULL;
    g_autofree char *to = NULL;
    g_assert (!_ostree_parse_delta_name ("GARBAGE", &from, &to, NULL));
    g_assert_null (from);
    g_assert_null (to);
  }

  {
    g_autofree char *from = NULL;
    g_autofree char *to = NULL;
    g_assert (!_ostree_parse_delta_name ("GARBAGE-5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03", &from, &to, NULL));
    g_assert_null (from);
    g_assert_null (to);
  }

  {
    g_autofree char *from = NULL;
    g_autofree char *to = NULL;
    g_assert (!_ostree_parse_delta_name ("30d13b73cfe1e6988ffc345eac905f82a18def8ef1f0666fc392019e9eac388d-GARBAGE", &from, &to, NULL));
    g_assert_null (from);
    g_assert_null (to);
  }
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ostree_parse_delta_name", test_ostree_parse_delta_name);
  return g_test_run();
}
