/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

static gboolean quiet;

static GOptionEntry options[] = {
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { NULL }
};

gboolean
ostree_builtin_run_triggers (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  OstreeCheckout *checkout = NULL;
  const char *dir;

  context = g_option_context_new ("DIR - Run trigger scripts for directory");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 1)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "DIR must be specified");
      goto out;
    }

  dir = argv[1];

  checkout = ostree_checkout_new (repo, dir);
  if (!ostree_checkout_run_triggers (checkout, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  g_clear_object (&checkout);
  return ret;
}
