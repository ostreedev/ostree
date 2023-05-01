/*
 * Copyright © 2020 Endless OS Foundation LLC
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
 *  - Philip Withnall <pwithnall@endlessos.org>
 */

#pragma once

#ifndef __GI_SCANNER__

#include <glib.h>

G_BEGIN_DECLS

GDateTime *_ostree_parse_rfc2616_date_time (const char *buf, size_t len);

G_END_DECLS

#endif
