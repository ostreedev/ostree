/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libglnx.h>

#include "ostree-autocleanups.h"
#include "ostree-core.h"
#include "ostree-core-private.h"
#include "ostree-ref.h"

G_DEFINE_BOXED_TYPE (OstreeCollectionRef, ostree_collection_ref,
                     ostree_collection_ref_dup, ostree_collection_ref_free)

/**
 * ostree_collection_ref_new:
 * @collection_id: (nullable): a collection ID, or %NULL for a plain ref
 * @ref_name: a ref name
 *
 * Create a new #OstreeCollectionRef containing (@collection_id, @ref_name). If
 * @collection_id is %NULL, this is equivalent to a plain ref name string (not a
 * refspec; no remote name is included), which can be used for non-P2P
 * operations.
 *
 * Returns: (transfer full): a new #OstreeCollectionRef
 * Since: 2017.8
 */
OstreeCollectionRef *
ostree_collection_ref_new (const gchar *collection_id,
                           const gchar *ref_name)
{
  g_autoptr(OstreeCollectionRef) collection_ref = NULL;

  g_return_val_if_fail (collection_id == NULL ||
                        ostree_validate_collection_id (collection_id, NULL), NULL);
  g_return_val_if_fail (ostree_validate_rev (ref_name, NULL), NULL);

  collection_ref = g_new0 (OstreeCollectionRef, 1);
  collection_ref->collection_id = g_strdup (collection_id);
  collection_ref->ref_name = g_strdup (ref_name);

  return g_steal_pointer (&collection_ref);
}

/**
 * ostree_collection_ref_dup:
 * @ref: an #OstreeCollectionRef
 *
 * Create a copy of the given @ref.
 *
 * Returns: (transfer full): a newly allocated copy of @ref
 * Since: 2017.8
 */
OstreeCollectionRef *
ostree_collection_ref_dup (const OstreeCollectionRef *ref)
{
  g_return_val_if_fail (ref != NULL, NULL);

  return ostree_collection_ref_new (ref->collection_id, ref->ref_name);
}

/**
 * ostree_collection_ref_free:
 * @ref: (transfer full): an #OstreeCollectionRef
 *
 * Free the given @ref.
 *
 * Since: 2017.8
 */
void
ostree_collection_ref_free (OstreeCollectionRef *ref)
{
  g_return_if_fail (ref != NULL);

  g_free (ref->collection_id);
  g_free (ref->ref_name);
  g_free (ref);
}

/**
 * ostree_collection_ref_hash:
 * @ref: an #OstreeCollectionRef
 *
 * Hash the given @ref. This function is suitable for use with #GHashTable.
 * @ref must be non-%NULL.
 *
 * Returns: hash value for @ref
 * Since: 2017.8
 */
guint
ostree_collection_ref_hash (gconstpointer ref)
{
  const OstreeCollectionRef *_ref = ref;

  if (_ref->collection_id != NULL)
    return g_str_hash (_ref->collection_id) ^ g_str_hash (_ref->ref_name);
  else
    return g_str_hash (_ref->ref_name);
}

/**
 * ostree_collection_ref_equal:
 * @ref1: an #OstreeCollectionRef
 * @ref2: another #OstreeCollectionRef
 *
 * Compare @ref1 and @ref2 and return %TRUE if they have the same collection ID and
 * ref name, and %FALSE otherwise. Both @ref1 and @ref2 must be non-%NULL.
 *
 * Returns: %TRUE if @ref1 and @ref2 are equal, %FALSE otherwise
 * Since: 2017.8
 */
gboolean
ostree_collection_ref_equal (gconstpointer ref1,
                             gconstpointer ref2)
{
  const OstreeCollectionRef *_ref1 = ref1, *_ref2 = ref2;

  return (g_strcmp0 (_ref1->collection_id, _ref2->collection_id) == 0 &&
          g_strcmp0 (_ref1->ref_name, _ref2->ref_name) == 0);
}

/**
 * ostree_collection_ref_dupv:
 * @refs: (array zero-terminated=1): %NULL-terminated array of #OstreeCollectionRefs
 *
 * Copy an array of #OstreeCollectionRefs, including deep copies of all its
 * elements. @refs must be %NULL-terminated; it may be empty, but must not be
 * %NULL.
 *
 * Returns: (transfer full) (array zero-terminated=1): a newly allocated copy of @refs
 * Since: 2017.8
 */
OstreeCollectionRef **
ostree_collection_ref_dupv (const OstreeCollectionRef * const *refs)
{
  gsize i,  n_refs = g_strv_length ((gchar **) refs);  /* hack */
  g_auto(OstreeCollectionRefv) new_refs = NULL;

  g_return_val_if_fail (refs != NULL, NULL);

  new_refs = g_new0 (OstreeCollectionRef*, n_refs + 1);

  for (i = 0; i < n_refs; i++)
    new_refs[i] = ostree_collection_ref_dup (refs[i]);
  new_refs[i] = NULL;

  return g_steal_pointer (&new_refs);
}

/**
 * ostree_collection_ref_freev:
 * @refs: (transfer full) (array zero-terminated=1): an array of #OstreeCollectionRefs
 *
 * Free the given array of @refs, including freeing all its elements. @refs
 * must be %NULL-terminated; it may be empty, but must not be %NULL.
 *
 * Since: 2017.8
 */
void
ostree_collection_ref_freev (OstreeCollectionRef **refs)
{
  gsize i;

  g_return_if_fail (refs != NULL);

  for (i = 0; refs[i] != NULL; i++)
    ostree_collection_ref_free (refs[i]);
  g_free (refs);
}
