/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

static char* opt_cache_dir;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = {
  { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_cache_dir, "Use custom cache dir", NULL },
  { NULL }
};

gboolean
ot_remote_builtin_refs (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *remote_name;
  gboolean ret = FALSE;
  g_autoptr(GHashTable) refs = NULL;

  context = g_option_context_new ("NAME");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  if (opt_cache_dir)
    {
      if (!ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
        goto out;
    }

  remote_name = argv[1];

  if (!ostree_repo_remote_list_refs (repo, remote_name, &refs, cancellable, error))
    goto out;
  else
    {
      g_autoptr(GList) ordered_keys = NULL;
      GList *iter = NULL;

      ordered_keys = g_hash_table_get_keys (refs);
      ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

      for (iter = ordered_keys; iter; iter = iter->next)
        {
          g_print ("%s:%s\n", remote_name, (const char *) iter->data);
        }
    }

  ret = TRUE;
out:
  return ret;
}
