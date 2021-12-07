/* This file declares a stub function that is only exported
 * to pacify ABI checkers - no one could really have used it.
 *
 * Copyright (C) 2016 Red Hat, Inc.
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
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
