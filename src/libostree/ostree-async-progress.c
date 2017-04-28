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

#include "libglnx.h"

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
  GHashTable *values;  /* (element-type uint GVariant) */

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
  g_hash_table_unref (self->values);
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
  self->values = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) g_variant_unref);
}

/**
 * ostree_async_progress_get_variant:
 * @self: an #OstreeAsyncProgress
 * @key: a key to look up
 *
 * Look up a key in the #OstreeAsyncProgress and return the #GVariant associated
 * with it. The lookup is thread-safe.
 *
 * Returns: (transfer full) (nullable): value for the given @key, or %NULL if
 *    it was not set
 * Since: 2017.6
 */
GVariant *
ostree_async_progress_get_variant (OstreeAsyncProgress *self,
                                   const char          *key)
{
  GVariant *rval;

  g_return_val_if_fail (OSTREE_IS_ASYNC_PROGRESS (self), NULL);
  g_return_val_if_fail (key != NULL, NULL);

  g_mutex_lock (&self->lock);
  rval = g_hash_table_lookup (self->values, GUINT_TO_POINTER (g_quark_from_string (key)));
  if (rval != NULL)
    g_variant_ref (rval);
  g_mutex_unlock (&self->lock);

  return rval;
}

guint
ostree_async_progress_get_uint (OstreeAsyncProgress       *self,
                                const char                *key)
{
  g_autoptr(GVariant) rval = ostree_async_progress_get_variant (self, key);
  return (rval != NULL) ? g_variant_get_uint32 (rval) : 0;
}

guint64
ostree_async_progress_get_uint64 (OstreeAsyncProgress       *self,
                                  const char                *key)
{
  g_autoptr(GVariant) rval = ostree_async_progress_get_variant (self, key);
  return (rval != NULL) ? g_variant_get_uint64 (rval) : 0;
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

/**
 * ostree_async_progress_set_variant:
 * @self: an #OstreeAsyncProgress
 * @key: a key to set
 * @value: the value to assign to @key
 *
 * Assign a new @value to the given @key, replacing any existing value. The
 * operation is thread-safe. @value may be a floating reference;
 * g_variant_ref_sink() will be called on it.
 *
 * Any watchers of the #OstreeAsyncProgress will be notified of the change if
 * @value differs from the existing value for @key.
 *
 * Since: 2017.6
 */
void
ostree_async_progress_set_variant (OstreeAsyncProgress *self,
                                   const char          *key,
                                   GVariant            *value)
{
  GVariant *orig_value;
  g_autoptr(GVariant) new_value = g_variant_ref_sink (value);
  gpointer qkey = GUINT_TO_POINTER (g_quark_from_string (key));

  g_return_if_fail (OSTREE_IS_ASYNC_PROGRESS (self));
  g_return_if_fail (key != NULL);
  g_return_if_fail (value != NULL);

  g_mutex_lock (&self->lock);

  if (self->dead)
    goto out;

  if (g_hash_table_lookup_extended (self->values, qkey, NULL, (gpointer *) &orig_value))
    {
      if (g_variant_equal (orig_value, new_value))
        goto out;
    }
  g_hash_table_replace (self->values, qkey, g_steal_pointer (&new_value));
  ensure_callback_locked (self);

 out:
  g_mutex_unlock (&self->lock);
}

void
ostree_async_progress_set_uint (OstreeAsyncProgress       *self,
                                const char                *key,
                                guint                      value)
{
  ostree_async_progress_set_variant (self, key, g_variant_new_uint32 (value));
}

void
ostree_async_progress_set_uint64 (OstreeAsyncProgress       *self,
                                  const char                *key,
                                  guint64                    value)
{
  ostree_async_progress_set_variant (self, key, g_variant_new_uint64 (value));
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
