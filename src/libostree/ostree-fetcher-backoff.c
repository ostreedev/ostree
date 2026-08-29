/*
 * Copyright (C) 2026 Red Hat, Inc.
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

#include "config.h"

#include "ostree-fetcher-backoff.h"

guint
_ostree_fetcher_retry_backoff_max_ms (guint base_ms, guint n_retries_done)
{
  /* The fetcher has no backoff yet: every retry is immediate. The following
   * commit replaces this with a capped exponential backoff. */
  (void)base_ms;
  (void)n_retries_done;
  return 0;
}
