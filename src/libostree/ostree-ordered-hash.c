/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ostree-ordered-hash.h"
#include "libgsystem.h"

OstreeOrderedHash *
_ostree_ordered_hash_new (void)
{
  OstreeOrderedHash *ret;
  ret = g_new0 (OstreeOrderedHash, 1);
  ret->order = g_ptr_array_new_with_free_func (g_free);
  ret->table = g_hash_table_new (g_str_hash, g_str_equal);
  return ret;
}

void
_ostree_ordered_hash_free (OstreeOrderedHash *ohash)
{
  if (!ohash)
    return;
  g_ptr_array_unref (ohash->order);
  g_hash_table_unref (ohash->table);
  g_free (ohash);
}

void
_ostree_ordered_hash_cleanup (void *loc)
{
  _ostree_ordered_hash_free (*((OstreeOrderedHash**)loc));
}

void
_ostree_ordered_hash_replace_key_take (OstreeOrderedHash   *ohash,
                                  char            *key,
                                  const char      *value)
{
  gboolean existed;

  existed = g_hash_table_remove (ohash->table, key);
  if (!existed)
    g_ptr_array_add (ohash->order, key);
  g_hash_table_insert (ohash->table, key, (char*)value);
}

void
_ostree_ordered_hash_replace_key (OstreeOrderedHash  *ohash,
                             const char     *key,
                             const char     *val)
{
  GString *buf;
  gsize keylen;
  char *valp;
  char *valblock;
  
  buf = g_string_new (key);
  keylen = buf->len;
  g_string_append_c (buf, '\0');
  g_string_append (buf, val);
  valblock = g_string_free (buf, FALSE);
  valp = valblock + keylen + 1;

  _ostree_ordered_hash_replace_key_take (ohash, valblock, valp);
}
