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

#include <errno.h>

#include "ostree-fetcher-backoff.h"

/* Following curl, retries start at a 1 second delay and double each time. curl
 * caps at 10 minutes; we cap tighter since pulls are frequently interactive. */
#define OSTREE_FETCHER_RETRY_BASE_MS 1000
#define OSTREE_FETCHER_RETRY_CAP_MS 30000

/* The upper bound in milliseconds on the backoff delay before retry number
 * @n_retries_done (0-indexed): a capped exponential, min(CAP, base_ms << n).
 * The caller draws the actual delay from [0, this] (full jitter), so that many
 * clients retrying against one mirror do not do so in lockstep. */
guint
_ostree_fetcher_retry_backoff_max_ms (guint base_ms, guint n_retries_done)
{
  guint64 delay_ms = base_ms;
  for (guint i = 0; i < n_retries_done && delay_ms < OSTREE_FETCHER_RETRY_CAP_MS; i++)
    delay_ms <<= 1;
  return delay_ms > OSTREE_FETCHER_RETRY_CAP_MS ? OSTREE_FETCHER_RETRY_CAP_MS : (guint)delay_ms;
}

/* Wait before a retry, interrupting the wait promptly if @cancellable fires. */
void
_ostree_fetcher_retry_backoff_wait (guint n_retries_done, GCancellable *cancellable)
{
  guint base_ms = OSTREE_FETCHER_RETRY_BASE_MS;
  const char *test_base_ms = g_getenv ("OSTREE_FETCHER_TEST_BACKOFF_MS");
  if (test_base_ms != NULL)
    base_ms = (guint)g_ascii_strtoull (test_base_ms, NULL, 10);

  guint max_ms = _ostree_fetcher_retry_backoff_max_ms (base_ms, n_retries_done);
  if (max_ms == 0)
    return;
  guint delay_ms = g_random_int_range (0, (gint32)max_ms + 1);
  if (delay_ms == 0)
    return;

  GPollFD pollfd;
  if (!g_cancellable_make_pollfd (cancellable, &pollfd))
    {
      g_usleep ((gulong)delay_ms * 1000);
      return;
    }

  gint64 deadline = g_get_monotonic_time () + (gint64)delay_ms * G_TIME_SPAN_MILLISECOND;
  for (;;)
    {
      gint64 remaining_us = deadline - g_get_monotonic_time ();
      if (remaining_us <= 0)
        break;
      gint n = g_poll (&pollfd, 1, (gint)(remaining_us / 1000));
      if (n > 0)
        break; /* the cancellable fd signalled */
      if (n < 0 && errno != EINTR)
        break;
    }
  g_cancellable_release_fd (cancellable);
}
