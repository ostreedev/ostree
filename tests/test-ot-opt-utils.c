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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include "ostree-mutable-tree.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ot-opt-utils.h"
#include "libglnx.h"

static GString *printerr_str = NULL;

static void
util_usage_error_printerr (const gchar *string)
{
  if (printerr_str == NULL)
    printerr_str = g_string_new (NULL);
  g_string_append (printerr_str, string);
}

static void
test_ot_util_usage_error (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("[TEST]");
  GPrintFunc old_printerr = g_set_printerr_handler (util_usage_error_printerr);

  ot_util_usage_error (context, "find_me", &error);

  g_assert_nonnull (strstr (printerr_str->str, "[TEST]"));
  g_assert_nonnull (strstr (error->message, "find_me"));
  g_clear_error (&error);

  g_set_printerr_handler (old_printerr);
  g_string_free (printerr_str, TRUE);
  printerr_str = NULL;
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ot-opt-utils/ot-util-usage-error", test_ot_util_usage_error);
  return g_test_run();
}
