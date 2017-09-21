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
 *
 * The ostree_async_progress_get_status() and ostree_async_progress_set_status()
 * methods get and set a well-known `status` key of type %G_VARIANT_TYPE_STRING.
 * This key may be accessed using the other #OstreeAsyncProgress methods, but it
 * must always have the correct type.
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

/**
 * ostree_async_progress_get:
 * @self: an #OstreeAsyncProgress
 * @...: key name, format string, #GVariant return locations, …, followed by %NULL
 *
 * Get the values corresponding to zero or more keys from the
 * #OstreeAsyncProgress. Each key is specified in @... as the key name, followed
 * by a #GVariant format string, followed by the necessary arguments for that
 * format string, just as for g_variant_get(). After those arguments is the
 * next key name. The varargs list must be %NULL-terminated.
 *
 * Each format string must make deep copies of its value, as the values stored
 * in the #OstreeAsyncProgress may be freed from another thread after this
 * function returns.
 *
 * This operation is thread-safe, and all the keys are queried atomically.
 *
 * |[<!-- language="C" -->
 * guint32 outstanding_fetches;
 * guint64 bytes_received;
 * g_autofree gchar *status = NULL;
 * g_autoptr(GVariant) refs_variant = NULL;
 *
 * ostree_async_progress_get (progress,
 *                            "outstanding-fetches", "u", &outstanding_fetches,
 *                            "bytes-received", "t", &bytes_received,
 *                            "status", "s", &status,
 *                            "refs", "@a{ss}", &refs_variant,
 *                            NULL);
 * ]|
 *
 * Since: 2017.6
 */
void
ostree_async_progress_get (OstreeAsyncProgress *self,
                           ...)
{
  va_list ap;
  const char *key, *format_string;

  g_mutex_lock (&self->lock);
  va_start (ap, self);

  for (key = va_arg (ap, const char *), format_string = va_arg (ap, const char *);
       key != NULL;
       key = va_arg (ap, const char *), format_string = va_arg (ap, const char *))
    {
      GVariant *variant;

      g_assert (format_string != NULL);

      variant = g_hash_table_lookup (self->values, GUINT_TO_POINTER (g_quark_from_string (key)));
      g_assert (variant != NULL);
      g_assert (g_variant_check_format_string (variant, format_string, TRUE));

      g_variant_get_va (variant, format_string, NULL, &ap);
    }

  va_end (ap);
  g_mutex_unlock (&self->lock);
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

/**
 * ostree_async_progress_set_status:
 * @self: an #OstreeAsyncProgress
 * @status: (nullable): new status string, or %NULL to clear the status
 *
 * Set the human-readable status string for the #OstreeAsyncProgress. This
 * operation is thread-safe. %NULL may be passed to clear the status.
 *
 * This is a convenience function to set the well-known `status` key.
 *
 * Since: 2017.6
 */
void
ostree_async_progress_set_status (OstreeAsyncProgress       *self,
                                  const char                *status)
{
  ostree_async_progress_set_variant (self, "status",
                                     g_variant_new_string ((status != NULL) ? status : ""));
}

/**
 * ostree_async_progress_get_status:
 * @self: an #OstreeAsyncProgress
 *
 * Get the human-readable status string from the #OstreeAsyncProgress. This
 * operation is thread-safe. The retuned value may be %NULL if no status is
 * set.
 *
 * This is a convenience function to get the well-known `status` key.
 *
 * Returns: (transfer full) (nullable): the current status, or %NULL if none is set
 * Since: 2017.6
 */
char *
ostree_async_progress_get_status (OstreeAsyncProgress       *self)
{
  g_autoptr(GVariant) rval = ostree_async_progress_get_variant (self, "status");
  const gchar *status = (rval != NULL) ? g_variant_get_string (rval, NULL) : NULL;
  if (status != NULL && *status == '\0')
    status = NULL;
  return g_strdup (status);
}

/**
 * ostree_async_progress_set:
 * @self: an #OstreeAsyncProgress
 * @...: key name, format string, #GVariant parameters, …, followed by %NULL
 *
 * Set the values for zero or more keys in the #OstreeAsyncProgress. Each key is
 * specified in @... as the key name, followed by a #GVariant format string,
 * followed by the necessary arguments for that format string, just as for
 * g_variant_new(). After those arguments is the next key name. The varargs list
 * must be %NULL-terminated.
 *
 * g_variant_ref_sink() will be called as appropriate on the #GVariant
 * parameters, so they may be floating.
 *
 * This operation is thread-safe, and all the keys are set atomically.
 *
 * |[<!-- language="C" -->
 * guint32 outstanding_fetches = 15;
 * guint64 bytes_received = 1000;
 *
 * ostree_async_progress_set (progress,
 *                            "outstanding-fetches", "u", outstanding_fetches,
 *                            "bytes-received", "t", bytes_received,
 *                            "status", "s", "Updated status",
 *                            "refs", "@a{ss}", g_variant_new_parsed ("@a{ss} {}"),
 *                            NULL);
 * ]|
 *
 * Since: 2017.6
 */
void
ostree_async_progress_set (OstreeAsyncProgress *self,
                           ...)
{
  va_list ap;
  const char *key, *format_string;
  gboolean changed;

  g_mutex_lock (&self->lock);

  if (self->dead)
    goto out;

  changed = FALSE;

  va_start (ap, self);

  for (key = va_arg (ap, const char *), format_string = va_arg (ap, const char *);
       key != NULL;
       key = va_arg (ap, const char *), format_string = va_arg (ap, const char *))
    {
      GVariant *orig_value;
      g_autoptr(GVariant) new_value = NULL;
      gpointer qkey = GUINT_TO_POINTER (g_quark_from_string (key));

      new_value = g_variant_ref_sink (g_variant_new_va (format_string, NULL, &ap));

      if (g_hash_table_lookup_extended (self->values, qkey, NULL, (gpointer *) &orig_value) &&
          g_variant_equal (orig_value, new_value))
        continue;

      g_hash_table_replace (self->values, qkey, g_steal_pointer (&new_value));
      changed = TRUE;
    }

  va_end (ap);

  if (changed)
    ensure_callback_locked (self);

out:
  g_mutex_unlock (&self->lock);
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
