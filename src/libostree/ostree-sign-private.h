/*
 * Copyright © 2023 Endless OS Foundation LLC
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
 *
 * Authors:
 *  - Dan Nicholson <dbn@endlessos.org>
 */

#pragma once

#include <glib.h>

#include "ostree-sign.h"
#include "ostree-types.h"

G_BEGIN_DECLS

gboolean _ostree_sign_summary_at (OstreeSign *self, OstreeRepo *repo, int dir_fd, GVariant *keys,
                                  GCancellable *cancellable, GError **error);

G_END_DECLS
