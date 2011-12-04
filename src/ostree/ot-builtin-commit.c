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

#include <gio/gunixoutputstream.h>

#include <glib/gi18n.h>

static char *metadata_text_path;
static char *metadata_bin_path;
static char *subject;
static char *body;
static char *parent;
static char *branch;
static gboolean tar;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &body, "Full description", "body" },
  { "metadata-variant-text", 0, 0, G_OPTION_ARG_FILENAME, &metadata_text_path, "File containing g_variant_print() output", "path" },
  { "metadata-variant", 0, 0, G_OPTION_ARG_FILENAME, &metadata_bin_path, "File containing serialized variant, in host endianness", "path" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &branch, "Branch", "branch" },
  { "parent", 'p', 0, G_OPTION_ARG_STRING, &parent, "Parent commit", "commit" },
  { "tar", 0, 0, G_OPTION_ARG_NONE, &tar, "Given argument is a tar file", NULL },
  { NULL }
};

gboolean
ostree_builtin_commit (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  char *argpath = NULL;
  GFile *arg = NULL;
  GChecksum *commit_checksum = NULL;
  GVariant *metadata = NULL;
  GMappedFile *metadata_mappedf = NULL;
  GFile *metadata_f = NULL;

  context = g_option_context_new ("[ARG] - Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc > 1)
    argpath = g_strdup (argv[1]);
  else
    argpath = g_get_current_dir ();

  if (g_str_has_suffix (argpath, "/"))
    argpath[strlen (argpath) - 1] = '\0';

  if (!*argpath)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty argument");
      goto out;
    }

  arg = ot_gfile_new_for_path (argpath);

  if (metadata_text_path || metadata_bin_path)
    {
      metadata_mappedf = g_mapped_file_new (metadata_text_path ? metadata_text_path : metadata_bin_path, FALSE, error);
      if (!metadata_mappedf)
        goto out;
      if (metadata_text_path)
        {
          metadata = g_variant_parse (G_VARIANT_TYPE ("a{sv}"),
                                      g_mapped_file_get_contents (metadata_mappedf),
                                      g_mapped_file_get_contents (metadata_mappedf) + g_mapped_file_get_length (metadata_mappedf),
                                      NULL, error);
          if (!metadata)
            goto out;
        }
      else if (metadata_bin_path)
        {
          metadata_f = ot_gfile_new_for_path (metadata_bin_path);
          if (!ot_util_variant_map (metadata_f, G_VARIANT_TYPE ("a{sv}"), &metadata, error))
            goto out;
        }
      else
        g_assert_not_reached ();
    }

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (!branch)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A branch must be specified with --branch");
      goto out;
    }

  if (!subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A subject must be specified with --subject");
      goto out;
    }

  if (!tar)
    {
      if (!ostree_repo_commit_directory (repo, branch, parent, subject, body, metadata,
                                         arg, &commit_checksum, NULL, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_commit_tarfile (repo, branch, parent, subject, body, metadata,
                                       arg, &commit_checksum, NULL, error))
        goto out;
    }

  ret = TRUE;
  g_print ("%s\n", g_checksum_get_string (commit_checksum));
 out:
  g_free (argpath);
  g_clear_object (&arg);
  if (metadata_mappedf)
    g_mapped_file_unref (metadata_mappedf);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  ot_clear_checksum (&commit_checksum);
  return ret;
}
