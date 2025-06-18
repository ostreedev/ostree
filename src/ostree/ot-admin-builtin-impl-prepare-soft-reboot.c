/*
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include <stdlib.h>

#include "ostree.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

#include "ostree-cmd-private.h"

gboolean
ot_admin_builtin_impl_prepare_soft_reboot (int argc, char **argv,
                                           OstreeCommandInvocation *invocation,
                                           GCancellable *cancellable, GError **error)
{
  if (!ostree_cmd__private__ ()->ostree_prepare_soft_reboot (error))
    return FALSE;

  return TRUE;
}
