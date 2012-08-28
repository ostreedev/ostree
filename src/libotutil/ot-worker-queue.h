/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>.
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

#ifndef __OSTREE_WORKER_QUEUE_H__
#define __OSTREE_WORKER_QUEUE_H__

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct OtWorkerQueue OtWorkerQueue;

typedef void (*OtWorkerQueueFunc) (gpointer data,
                                   gpointer user_data);
typedef void (*OtWorkerQueueIdleFunc) (gpointer user_data);

OtWorkerQueue *ot_worker_queue_new (const char         *thread_name,
                                    OtWorkerQueueFunc   func,
                                    gpointer            data);

void ot_worker_queue_start (OtWorkerQueue  *queue);

void ot_worker_queue_hold (OtWorkerQueue  *queue);
void ot_worker_queue_release (OtWorkerQueue  *queue);

void ot_worker_queue_set_idle_callback (OtWorkerQueue          *queue,
                                        GMainContext           *context,
                                        OtWorkerQueueIdleFunc   idle_callback,
                                        gpointer                data);

void ot_worker_queue_push (OtWorkerQueue      *queue,
                           gpointer            data);

void ot_worker_queue_unref (OtWorkerQueue *queue);

G_END_DECLS

#endif
