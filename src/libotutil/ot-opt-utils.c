/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>

#include <string.h>

#include "otutil.h"

void
ot_util_usage_error (GOptionContext *context, const char *message, GError **error)
{
  gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", help);
  g_free (help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}
