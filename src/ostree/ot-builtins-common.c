/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ot-builtins-common.h"
#include "otutil.h"

void
ot_common_pull_progress (OstreeAsyncProgress       *progress,
                         gpointer                   user_data)
{
  GSConsole *console = user_data;
  GString *buf;
  gs_free char *status = NULL;
  guint outstanding_fetches;
  guint outstanding_writes;
  guint n_scanned_metadata;

  if (!console)
    return;

  buf = g_string_new ("");

  status = ostree_async_progress_get_status (progress);
  outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
  outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");
  n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
  if (status)
    {
      g_string_append (buf, status);
    }
  else if (outstanding_fetches)
    {
      guint64 bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");
      guint fetched = ostree_async_progress_get_uint (progress, "fetched");
      guint requested = ostree_async_progress_get_uint (progress, "requested");
      guint64 bytes_sec = (g_get_monotonic_time () - ostree_async_progress_get_uint64 (progress, "start-time")) / G_USEC_PER_SEC;
      gs_free char *formatted_bytes_transferred =
        g_format_size_full (bytes_transferred, 0);
      gs_free char *formatted_bytes_sec = NULL;

      if (!bytes_sec) // Ignore first second
        formatted_bytes_sec = g_strdup ("-");
      else
        {
          bytes_sec = bytes_transferred / bytes_sec;
          formatted_bytes_sec = g_format_size (bytes_sec);
        }

      g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                              (guint)((((double)fetched) / requested) * 100),
                              fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  gs_console_begin_status_line (console, buf->str, NULL, NULL);
  
  g_string_free (buf, TRUE);
  
}
