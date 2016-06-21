/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>

#include <string.h>

#include "otutil.h"
#include "libglnx.h"

#ifdef HAVE_LIBSYSTEMD
#define SD_JOURNAL_SUPPRESS_LOCATION
#include <systemd/sd-journal.h>
#endif
#include <glib-unix.h>

/**
 * ot_log_structured:
 * @message: Text message to send 
 * @keys: (allow-none) (array zero-terminated=1) (element-type utf8): Optional structured data
 * 
 * Log structured data in an operating-system specific fashion.  The
 * parameter @opts should be an array of UTF-8 KEY=VALUE strings.
 * This function does not support binary data.  See
 * http://www.freedesktop.org/software/systemd/man/systemd.journal-fields.html
 * for more information about fields that can be used on a systemd
 * system.
 */
static void
ot_log_structured (const char *message,
                   const char *const *keys)
{
#ifdef HAVE_LIBSYSTEMD
    const char *const*iter;
    g_autofree char *msgkey = NULL;
    guint i, n_opts;
    struct iovec *iovs;

    for (n_opts = 0, iter = keys; *iter; iter++, n_opts++)
        ;

    n_opts++; /* Add one for MESSAGE= */
    iovs = g_alloca (sizeof (struct iovec) * n_opts);
    
    for (i = 0, iter = keys; *iter; iter++, i++) {
        iovs[i].iov_base = (char*)keys[i];
        iovs[i].iov_len = strlen (keys[i]);
    }
    g_assert(i == n_opts-1);
    msgkey = g_strconcat ("MESSAGE=", message, NULL);
    iovs[i].iov_base = msgkey;
    iovs[i].iov_len = strlen (msgkey);
    
    // The code location isn't useful since we're wrapping
    sd_journal_sendv (iovs, n_opts);
#else
    g_print ("%s\n", message);
#endif
}

/**
 * ot_stdout_is_journal:
 *
 * Use this function when you want your code to behave differently
 * depeneding on whether your program was started as a systemd unit,
 * or e.g. interactively at a terminal.
 *
 * Returns: %TRUE if stdout is (probably) connnected to the systemd journal
 */
static gboolean
ot_stdout_is_journal (void)
{
  static gsize initialized;
  static gboolean stdout_is_socket;

  if (g_once_init_enter (&initialized))
    {
      guint64 pid = (guint64) getpid ();
      g_autofree char *fdpath = g_strdup_printf ("/proc/%" G_GUINT64_FORMAT "/fd/1", pid);
      char buf[1024];
      ssize_t bytes_read;

      if ((bytes_read = readlink (fdpath, buf, sizeof(buf) - 1)) != -1)
        {
          buf[bytes_read] = '\0';
          stdout_is_socket = g_str_has_prefix (buf, "socket:");
        }
      else
        stdout_is_socket = FALSE;

      g_once_init_leave (&initialized, TRUE);
    }

  return stdout_is_socket;
}

/**
 * gs_log_structured_print:
 * @message: A message to log
 * @keys: (allow-none) (array zero-terminated=1) (element-type utf8): Optional structured data
 *
 * Like gs_log_structured(), but also print to standard output (if it
 * is not already connected to the system log).
 */
static void
ot_log_structured_print (const char *message,
                         const char *const *keys)
{
  ot_log_structured (message, keys);

#ifdef HAVE_LIBSYSTEMD
  if (!ot_stdout_is_journal ())
    g_print ("%s\n", message);
#endif
}

/**
 * ot_log_structured_print_id_v:
 * @message_id: A unique MESSAGE_ID
 * @format: A format string
 *
 * The provided @message_id is a unique MESSAGE_ID (see <ulink url="http://www.freedesktop.org/software/systemd/man/systemd.journal-fields.html"> for more information).
 *
 * This function otherwise acts as ot_log_structured_print(), taking
 * @format as a format string.
 */
void
ot_log_structured_print_id_v (const char *message_id,
                              const char *format,
                              ...)
{
  const char *key0 = glnx_strjoina ("MESSAGE_ID=", message_id);
  const char *keys[] = { key0, NULL };
  g_autofree char *msg = NULL;
  va_list args;

  va_start (args, format);
  msg = g_strdup_vprintf (format, args);
  va_end (args);

  ot_log_structured_print (msg, (const char *const *)keys);
}
