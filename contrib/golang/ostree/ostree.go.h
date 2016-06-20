#include <ostree.h>
#include <string.h>

static void
_ostree_repo_checkout_options_init_docker_union (OstreeRepoCheckoutOptions *opts)
{
  memset (opts, 0, sizeof (*opts));
  opts->mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  opts->overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  opts->disable_fsync = 1;
  opts->process_whiteouts = 1;
}

static const char *
_g_variant_lookup_string (GVariant *v, const char *key)
{
  const char *r;
  if (g_variant_lookup (v, key, "&s", &r))
    return r;
  return NULL;
}
