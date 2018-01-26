/* This file declares a stub function that is only exported
 * to pacify ABI checkers - no one could really have used it.
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "ostree-dummy-enumtypes.h"

/* Exported for backwards compat - see 
 * https://bugzilla.gnome.org/show_bug.cgi?id=764131
 */
GType
ostree_fetcher_config_flags_get_type (void)
{
  return G_TYPE_INVALID;
}
