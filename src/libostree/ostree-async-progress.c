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

#include "ostree-async-progress.h"

/**
 * SECTION:ostree-async-progress
 * @title: Progress notification system for asynchronous operations
 * @short_description: Values representing progress
 *
 * For many asynchronous operations, it's desirable for callers to be
 * able to watch their status as they progress.  For example, an user
 * interface calling an asynchronous download operation will want to
 * be able to see the total number of bytes downloaded.
 *
 * This class provides a mechanism for callees of asynchronous
 * operations to communicate back with callers.  It transparently
 * handles thread safety, ensuring that the progress change
 * notification occurs in the thread-default context of the calling
 * operation.
 */

#if GLIB_SIZEOF_VOID_P == 8
#define _OSTREE_HAVE_LP64 1
#else
#define _OSTREE_HAVE_LP64 0
#endif

enum {
  CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct OstreeAsyncProgress
{
  GObject parent_instance;

  GMutex lock;
  GMainContext *maincontext;
  GSource *idle_source;
  GHashTable *uint_values;
  GHashTable *uint64_values;

  gboolean dead;

  char *status;
};

G_DEFINE_TYPE (OstreeAsyncProgress, ostree_async_progress, G_TYPE_OBJECT)

static void
ostree_async_progress_finalize (GObject *object)
{
  OstreeAsyncProgress *self;

  self = OSTREE_ASYNC_PROGRESS (object);

  g_mutex_clear (&self->lock);
  g_clear_pointer (&self->maincontext, g_main_context_unref);
  g_clear_pointer (&self->idle_source, g_source_unref);
  g_hash_table_unref (self->uint_values);
  g_hash_table_unref (self->uint64_values);
  g_free (self->status);

  G_OBJECT_CLASS (ostree_async_progress_parent_class)->finalize (object);
}

static void
ostree_async_progress_class_init (OstreeAsyncProgressClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_async_progress_finalize;

  /**
   * OstreeAsyncProgress::changed:
   * @self: Self
   *
   * Emitted when @self has been changed.
   **/
  signals[CHANGED] =
    g_signal_new ("changed",
		  OSTREE_TYPE_ASYNC_PROGRESS,
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (OstreeAsyncProgressClass, changed),
		  NULL, NULL,
		  NULL,
		  G_TYPE_NONE, 0);
}

static void
ostree_async_progress_init (OstreeAsyncProgress *self)
{
  g_mutex_init (&self->lock);
  self->maincontext = g_main_context_ref_thread_default ();
  self->uint_values = g_hash_table_new (NULL, NULL);
#if _OSTREE_HAVE_LP64
  self->uint64_values = g_hash_table_new (NULL, NULL);
#else
  self->uint64_values = g_hash_table_new_full (NULL, NULL,
                                               NULL, g_free);
#endif
}

guint
ostree_async_progress_get_uint (OstreeAsyncProgress       *self,
                                const char                *key)
{
  guint rval;
  g_mutex_lock (&self->lock);
  rval = GPOINTER_TO_UINT (g_hash_table_lookup (self->uint_values,
                                                GUINT_TO_POINTER (g_quark_from_string (key))));
  g_mutex_unlock (&self->lock);
  return rval;
}

guint64
ostree_async_progress_get_uint64 (OstreeAsyncProgress       *self,
                                  const char                *key)
{
#if _OSTREE_HAVE_LP64
  guint64 rval;
  g_mutex_lock (&self->lock);
  rval = (guint64) g_hash_table_lookup (self->uint64_values, GUINT_TO_POINTER (g_quark_from_string (key)));
  g_mutex_unlock (&self->lock);
  return rval;
#else
  guint64 *rval;
  g_mutex_lock (&self->lock);
  rval = g_hash_table_lookup (self->uint64_values, (gpointer)g_quark_from_string (key));
  g_mutex_unlock (&self->lock);
  if (rval)
    return *rval;
  return 0;
#endif
}

static gboolean
idle_invoke_async_progress (gpointer user_data)
{
  OstreeAsyncProgress *self = user_data;

  g_mutex_lock (&self->lock);
  self->idle_source = NULL;
  g_mutex_unlock (&self->lock);

  g_signal_emit (self, signals[CHANGED], 0);

  return FALSE;
}

static void
ensure_callback_locked (OstreeAsyncProgress *self)
{
  if (self->idle_source)
    return;
  self->idle_source = g_idle_source_new ();
  g_source_set_callback (self->idle_source, idle_invoke_async_progress, self, NULL);
  g_source_attach (self->idle_source, self->maincontext);
}

void
ostree_async_progress_set_status (OstreeAsyncProgress       *self,
                                  const char                *status)
{
  g_mutex_lock (&self->lock);
  if (!self->dead)
    {
      g_free (self->status);
      self->status = g_strdup (status);
      ensure_callback_locked (self);
    }
  g_mutex_unlock (&self->lock);
}

char *
ostree_async_progress_get_status (OstreeAsyncProgress       *self)
{
  char *ret;
  g_mutex_lock (&self->lock);
  ret = g_strdup (self->status);
  g_mutex_unlock (&self->lock);
  return ret;
}

static void
update_key (OstreeAsyncProgress   *self,
            GHashTable            *hash,
            const char            *key,
            gpointer               value)
{
  gpointer orig_value;
  gpointer qkey = GUINT_TO_POINTER (g_quark_from_string (key));

  g_mutex_lock (&self->lock);

  if (self->dead)
    goto out;

  if (g_hash_table_lookup_extended (hash, qkey, NULL, &orig_value))
    {
      if (orig_value == value)
        goto out;
    }
  g_hash_table_replace (hash, qkey, value);
  ensure_callback_locked (self);

 out:
  g_mutex_unlock (&self->lock);
}

void
ostree_async_progress_set_uint (OstreeAsyncProgress       *self,
                                const char                *key,
                                guint                      value)
{
  update_key (self, self->uint_values, key, GUINT_TO_POINTER (value));
}

void
ostree_async_progress_set_uint64 (OstreeAsyncProgress       *self,
                                  const char                *key,
                                  guint64                    value)
{
  gpointer valuep;

#if _OSTREE_HAVE_LP64
  valuep = (gpointer)value;
#else
  {
    guint64 *boxed = g_malloc (sizeof (guint64));
    *boxed = value;
    valuep = boxed;
  }
#endif
  update_key (self, self->uint64_values, key, valuep);
}

/**
 * ostree_async_progress_new:
 *
 * Returns: (transfer full): A new progress object
 */
OstreeAsyncProgress *
ostree_async_progress_new (void)
{
  return (OstreeAsyncProgress*)g_object_new (OSTREE_TYPE_ASYNC_PROGRESS, NULL);
}


OstreeAsyncProgress *
ostree_async_progress_new_and_connect (void (*changed) (OstreeAsyncProgress *self, gpointer user_data),
                                       gpointer user_data)
{
  OstreeAsyncProgress *ret = ostree_async_progress_new ();
  g_signal_connect (ret, "changed", G_CALLBACK (changed), user_data);
  return ret;
}

/**
 * ostree_async_progress_finish:
 * @self: Self
 *
 * Process any pending signals, ensuring the main context is cleared
 * of sources used by this object.  Also ensures that no further
 * events will be queued.
 */
void
ostree_async_progress_finish (OstreeAsyncProgress *self)
{
  gboolean emit_changed = FALSE;

  g_mutex_lock (&self->lock);
  if (!self->dead)
    {
      self->dead = TRUE;
      if (self->idle_source)
        {
          g_source_destroy (self->idle_source);
          self->idle_source = NULL;
          emit_changed = TRUE;
        }
    }
  g_mutex_unlock (&self->lock);

  if (emit_changed)
    g_signal_emit (self, signals[CHANGED], 0);
}
