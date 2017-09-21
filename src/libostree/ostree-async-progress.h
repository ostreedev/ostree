/*
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

#pragma once

#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_ASYNC_PROGRESS         (ostree_async_progress_get_type ())
#define OSTREE_ASYNC_PROGRESS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_ASYNC_PROGRESS, OstreeAsyncProgress))
#define OSTREE_ASYNC_PROGRESS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_ASYNC_PROGRESS, OstreeAsyncProgressClass))
#define OSTREE_IS_ASYNC_PROGRESS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_ASYNC_PROGRESS))
#define OSTREE_IS_ASYNC_PROGRESS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_ASYNC_PROGRESS))
#define OSTREE_ASYNC_PROGRESS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_ASYNC_PROGRESS, OstreeAsyncProgressClass))

typedef struct OstreeAsyncProgress   OstreeAsyncProgress;
typedef struct OstreeAsyncProgressClass   OstreeAsyncProgressClass;

struct OstreeAsyncProgressClass
{
  GObjectClass parent_class;

  void (*changed) (OstreeAsyncProgress *self, gpointer user_data);
};

_OSTREE_PUBLIC
GType   ostree_async_progress_get_type (void) G_GNUC_CONST;

_OSTREE_PUBLIC
OstreeAsyncProgress *ostree_async_progress_new (void);

_OSTREE_PUBLIC
OstreeAsyncProgress *ostree_async_progress_new_and_connect (void (*changed) (OstreeAsyncProgress *self, gpointer user_data), gpointer user_data);

_OSTREE_PUBLIC
char *ostree_async_progress_get_status (OstreeAsyncProgress       *self);

_OSTREE_PUBLIC
void ostree_async_progress_get (OstreeAsyncProgress *self,
                                ...) G_GNUC_NULL_TERMINATED;

_OSTREE_PUBLIC
guint ostree_async_progress_get_uint (OstreeAsyncProgress       *self,
                                      const char                *key);
_OSTREE_PUBLIC
guint64 ostree_async_progress_get_uint64 (OstreeAsyncProgress       *self,
                                          const char                *key);
_OSTREE_PUBLIC
GVariant *ostree_async_progress_get_variant (OstreeAsyncProgress *self,
                                             const char          *key);

_OSTREE_PUBLIC
void ostree_async_progress_set_status (OstreeAsyncProgress       *self,
                                       const char                *status);

_OSTREE_PUBLIC
void ostree_async_progress_set (OstreeAsyncProgress *self,
                                ...) G_GNUC_NULL_TERMINATED;

_OSTREE_PUBLIC
void ostree_async_progress_set_uint (OstreeAsyncProgress       *self,
                                     const char                *key,
                                     guint                      value);
_OSTREE_PUBLIC
void ostree_async_progress_set_uint64 (OstreeAsyncProgress       *self,
                                       const char                *key,
                                       guint64                    value);
_OSTREE_PUBLIC
void ostree_async_progress_set_variant (OstreeAsyncProgress *self,
                                        const char          *key,
                                        GVariant            *value);

_OSTREE_PUBLIC
void ostree_async_progress_finish (OstreeAsyncProgress *self);

G_END_DECLS
