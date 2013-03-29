/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "otutil.h"

#include <glib-unix.h>
#include <string.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <unistd.h>

struct OtWaitableQueue {
  volatile gint refcount;
  GMutex mutex;
  int fd;
  gboolean read_empty;
  GQueue queue;
};

OtWaitableQueue *
ot_waitable_queue_new (void)
{
  OtWaitableQueue *queue = g_new0 (OtWaitableQueue, 1);
  queue->refcount = 1;
  g_mutex_init (&queue->mutex);
  g_queue_init (&queue->queue);

  queue->fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  g_assert (queue->fd >= 0);
  
  return queue;
}

void
ot_waitable_queue_push (OtWaitableQueue *queue,
                        gpointer         data)
{
  const guint64 val = 1;
  int rval;

  g_mutex_lock (&queue->mutex);
  g_queue_push_head (&queue->queue, data);
  do 
    rval = write (queue->fd, &val, sizeof (val));
  while (G_UNLIKELY (rval == -1 && errno == EINTR));
  queue->read_empty = FALSE;
  g_mutex_unlock (&queue->mutex);
}

gboolean
ot_waitable_queue_pop (OtWaitableQueue *queue,
                       gpointer        *out_data)
{
  gpointer ret = NULL;
  gboolean empty = TRUE;
  int rval;
  guint64 val;

  g_mutex_lock (&queue->mutex);
  if (g_queue_peek_tail_link (&queue->queue) != NULL)
    {
      ret = g_queue_pop_tail (&queue->queue);
      empty = FALSE;
    }
  else if (!queue->read_empty)
    {
      do
        rval = read (queue->fd, &val, sizeof (val));
      while (G_UNLIKELY (rval == -1 && errno == EINTR));
      queue->read_empty = TRUE;
    }
  g_mutex_unlock (&queue->mutex);

  *out_data = ret;
  return !empty;
}

void
ot_waitable_queue_ref (OtWaitableQueue *queue)
{
  g_atomic_int_inc (&queue->refcount);
}

void
ot_waitable_queue_unref (OtWaitableQueue *queue)
{
  if (!g_atomic_int_dec_and_test (&queue->refcount))
    return;
  g_mutex_clear (&queue->mutex);
  g_queue_clear (&queue->queue);
  (void) close (queue->fd);
  g_free (queue);
}

GSource *
ot_waitable_queue_create_source (OtWaitableQueue   *queue)
{
  return g_unix_fd_source_new (queue->fd, G_IO_IN);
}
