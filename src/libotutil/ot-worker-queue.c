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

#include <string.h>

struct OtWorkerQueue {
  GMutex mutex;
  GCond cond;
  GQueue queue;

  volatile gint holds;

  char *thread_name;
  
  gboolean destroyed;

  GThread *worker;

  OtWorkerQueueFunc work_func;
  OtWorkerQueueFunc work_data;
  
  GMainContext *idle_context;
  OtWorkerQueueIdleFunc idle_callback;
  gpointer idle_data;
};

static gpointer
ot_worker_queue_thread_main (gpointer user_data);

OtWorkerQueue *
ot_worker_queue_new (const char          *thread_name,
                     OtWorkerQueueFunc    func,
                     gpointer             data)
{
  OtWorkerQueue *queue = g_new0 (OtWorkerQueue, 1);
  g_mutex_init (&queue->mutex);
  g_cond_init (&queue->cond);
  g_queue_init (&queue->queue);

  queue->thread_name = g_strdup (thread_name);
  queue->work_func = func;
  queue->work_data = data;
  
  return queue;
}

void
ot_worker_queue_start (OtWorkerQueue  *queue)
{
  queue->worker = g_thread_new (queue->thread_name, ot_worker_queue_thread_main, queue);
}

void
ot_worker_queue_hold (OtWorkerQueue  *queue)
{
  g_atomic_int_inc (&queue->holds);
}

void
ot_worker_queue_release (OtWorkerQueue  *queue)
{
  g_atomic_int_add (&queue->holds, -1);
}

void
ot_worker_queue_push (OtWorkerQueue *queue,
                            gpointer            data)
{
  g_mutex_lock (&queue->mutex);
  g_queue_push_head (&queue->queue, data);
  g_cond_signal (&queue->cond);
  g_mutex_unlock (&queue->mutex);
}

static gboolean
invoke_idle_callback (gpointer user_data)
{
  OtWorkerQueue *queue = user_data;
  queue->idle_callback (queue->idle_data);
  return FALSE;
}

static gpointer
ot_worker_queue_thread_main (gpointer user_data)
{
  OtWorkerQueue *queue = user_data;

  while (TRUE)
    {
      gpointer item;

      g_mutex_lock (&queue->mutex);

      while (!g_queue_peek_tail_link (&queue->queue))
        {
          if (queue->idle_callback && g_atomic_int_get (&queue->holds) == 0)
            g_main_context_invoke (queue->idle_context,
                                   invoke_idle_callback,
                                   queue);
          g_cond_wait (&queue->cond, &queue->mutex);
        }

      item = g_queue_pop_tail (&queue->queue);

      g_mutex_unlock (&queue->mutex);

      if (!item)
        break;

      queue->work_func (item, queue->work_data);
    }

  return NULL;
}

void
ot_worker_queue_set_idle_callback (OtWorkerQueue *queue,
                                   GMainContext  *context,
                                   OtWorkerQueueIdleFunc idle_callback,
                                   gpointer       data)
{
  g_assert (!queue->worker);
  if (!context)
    context = g_main_context_default ();
  queue->idle_context = g_main_context_ref (context);
  queue->idle_callback = idle_callback;
  queue->idle_data = data;
}

void
ot_worker_queue_unref (OtWorkerQueue *queue)
{
  if (queue->worker)
    {
      ot_worker_queue_push (queue, NULL);
      g_thread_join (queue->worker);
    }

  g_free (queue->thread_name);

  g_main_context_unref (queue->idle_context);
  g_mutex_clear (&queue->mutex);
  g_cond_clear (&queue->cond);
  g_queue_clear (&queue->queue);
  g_free (queue);
}
