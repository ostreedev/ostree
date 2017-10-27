/*
 * Copyright (C) 2017 Alexander Larsson <alexl@redhat.com>.
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
 * Author: Alexander Larsson <alexl@redhat.com>.
 */

#include "config.h"

#include "ot-variant-builder.h"
#include "libglnx/libglnx.h"

/*****************************************************************************************
 * This code is copied from gvariant in glib. With the following copyright:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright © 2007, 2008 Ryan Lortie
 * Copyright © 2010 Codethink Limited
 *****************************************************************************************/

typedef struct _GVariantTypeInfo GVariantTypeInfo;

#define G_VARIANT_TYPE_INFO_CHAR_MAYBE      'm'
#define G_VARIANT_TYPE_INFO_CHAR_ARRAY      'a'
#define G_VARIANT_TYPE_INFO_CHAR_TUPLE      '('
#define G_VARIANT_TYPE_INFO_CHAR_DICT_ENTRY '{'
#define G_VARIANT_TYPE_INFO_CHAR_VARIANT    'v'
#define g_variant_type_info_get_type_char(info) \
  (g_variant_type_info_get_type_string(info)[0])

struct _GVariantTypeInfo
{
  gsize fixed_size;
  guchar alignment;
  guchar container_class;
};

typedef struct
{
  GVariantTypeInfo *type_info;

  gsize i, a;
  gint8 b, c;

  guint8 ending_type;
} GVariantMemberInfo;

#define G_VARIANT_MEMBER_ENDING_FIXED   0
#define G_VARIANT_MEMBER_ENDING_LAST    1
#define G_VARIANT_MEMBER_ENDING_OFFSET  2

typedef struct
{
  GVariantTypeInfo info;

  gchar *type_string;
  gint ref_count;
} ContainerInfo;

typedef struct
{
  ContainerInfo container;

  GVariantTypeInfo *element;
} ArrayInfo;

typedef struct
{
  ContainerInfo container;

  GVariantMemberInfo *members;
  gsize n_members;
} TupleInfo;

/* Hard-code the base types in a constant array */
static const GVariantTypeInfo g_variant_type_info_basic_table[24] = {
#define fixed_aligned(x)  x, x - 1
#define not_a_type             0,
#define unaligned         0, 0
#define aligned(x)        0, x - 1
  /* 'b' */ { fixed_aligned(1) },   /* boolean */
  /* 'c' */ { not_a_type },
  /* 'd' */ { fixed_aligned(8) },   /* double */
  /* 'e' */ { not_a_type },
  /* 'f' */ { not_a_type },
  /* 'g' */ { unaligned        },   /* signature string */
  /* 'h' */ { fixed_aligned(4) },   /* file handle (int32) */
  /* 'i' */ { fixed_aligned(4) },   /* int32 */
  /* 'j' */ { not_a_type },
  /* 'k' */ { not_a_type },
  /* 'l' */ { not_a_type },
  /* 'm' */ { not_a_type },
  /* 'n' */ { fixed_aligned(2) },   /* int16 */
  /* 'o' */ { unaligned        },   /* object path string */
  /* 'p' */ { not_a_type },
  /* 'q' */ { fixed_aligned(2) },   /* uint16 */
  /* 'r' */ { not_a_type },
  /* 's' */ { unaligned        },   /* string */
  /* 't' */ { fixed_aligned(8) },   /* uint64 */
  /* 'u' */ { fixed_aligned(4) },   /* uint32 */
  /* 'v' */ { aligned(8)       },   /* variant */
  /* 'w' */ { not_a_type },
  /* 'x' */ { fixed_aligned(8) },   /* int64 */
  /* 'y' */ { fixed_aligned(1) },   /* byte */
#undef fixed_aligned
#undef not_a_type
#undef unaligned
#undef aligned
};

static GRecMutex g_variant_type_info_lock;
static GHashTable *g_variant_type_info_table;

static GVariantTypeInfo * g_variant_type_info_ref (GVariantTypeInfo *info);
static void g_variant_type_info_unref (GVariantTypeInfo *info);
static GVariantTypeInfo * g_variant_type_info_get (const GVariantType *type);

#define GV_ARRAY_INFO_CLASS 'a'
static ArrayInfo *
GV_ARRAY_INFO (GVariantTypeInfo *info)
{
  return (ArrayInfo *) info;
}

static void
array_info_free (GVariantTypeInfo *info)
{
  ArrayInfo *array_info;

  g_assert (info->container_class == GV_ARRAY_INFO_CLASS);
  array_info = (ArrayInfo *) info;

  g_variant_type_info_unref (array_info->element);
  g_slice_free (ArrayInfo, array_info);
}

static ContainerInfo *
array_info_new (const GVariantType *type)
{
  ArrayInfo *info;

  info = g_slice_new (ArrayInfo);
  info->container.info.container_class = GV_ARRAY_INFO_CLASS;

  info->element = g_variant_type_info_get (g_variant_type_element (type));
  info->container.info.alignment = info->element->alignment;
  info->container.info.fixed_size = 0;

  return (ContainerInfo *) info;
}

/* == tuple == */
#define GV_TUPLE_INFO_CLASS 'r'
static TupleInfo *
GV_TUPLE_INFO (GVariantTypeInfo *info)
{
  return (TupleInfo *) info;
}

static void
tuple_info_free (GVariantTypeInfo *info)
{
  TupleInfo *tuple_info;
  gint i;

  g_assert (info->container_class == GV_TUPLE_INFO_CLASS);
  tuple_info = (TupleInfo *) info;

  for (i = 0; i < tuple_info->n_members; i++)
    g_variant_type_info_unref (tuple_info->members[i].type_info);

  g_slice_free1 (sizeof (GVariantMemberInfo) * tuple_info->n_members,
                 tuple_info->members);
  g_slice_free (TupleInfo, tuple_info);
}

static void
tuple_allocate_members (const GVariantType  *type,
                        GVariantMemberInfo **members,
                        gsize               *n_members)
{
  const GVariantType *item_type;
  gsize i = 0;

  *n_members = g_variant_type_n_items (type);
  *members = g_slice_alloc (sizeof (GVariantMemberInfo) * *n_members);

  item_type = g_variant_type_first (type);
  while (item_type)
    {
      GVariantMemberInfo *member = &(*members)[i++];

      member->type_info = g_variant_type_info_get (item_type);
      item_type = g_variant_type_next (item_type);

      if (member->type_info->fixed_size)
        member->ending_type = G_VARIANT_MEMBER_ENDING_FIXED;
      else if (item_type == NULL)
        member->ending_type = G_VARIANT_MEMBER_ENDING_LAST;
      else
        member->ending_type = G_VARIANT_MEMBER_ENDING_OFFSET;
    }

  g_assert (i == *n_members);
}

/* this is g_variant_type_info_query for a given member of the tuple.
 * before the access is done, it is ensured that the item is within
 * range and %FALSE is returned if not.
 */
static gboolean
tuple_get_item (TupleInfo          *info,
                GVariantMemberInfo *item,
                gsize              *d,
                gsize              *e)
{
  if (&info->members[info->n_members] == item)
    return FALSE;

  *d = item->type_info->alignment;
  *e = item->type_info->fixed_size;
  return TRUE;
}

/* Read the documentation for #GVariantMemberInfo in gvarianttype.h
 * before attempting to understand this.
 *
 * This function adds one set of "magic constant" values (for one item
 * in the tuple) to the table.
 *
 * The algorithm in tuple_generate_table() calculates values of 'a', 'b'
 * and 'c' for each item, such that the procedure for finding the item
 * is to start at the end of the previous variable-sized item, add 'a',
 * then round up to the nearest multiple of 'b', then add 'c'.
 * Note that 'b' is stored in the usual "one less than" form.  ie:
 *
 *   start = ROUND_UP(prev_end + a, (b + 1)) + c;
 *
 * We tweak these values a little to allow for a slightly easier
 * computation and more compact storage.
 */
static void
tuple_table_append (GVariantMemberInfo **items,
                    gsize                i,
                    gsize                a,
                    gsize                b,
                    gsize                c)
{
  GVariantMemberInfo *item = (*items)++;

  /* We can shift multiples of the alignment size from 'c' into 'a'.
   * As long as we're shifting whole multiples, it won't affect the
   * result.  This means that we can take the "aligned" portion off of
   * 'c' and add it into 'a'.
   *
   *  Imagine (for sake of clarity) that ROUND_10 rounds up to the
   *  nearest 10.  It is clear that:
   *
   *   ROUND_10(a) + c == ROUND_10(a + 10*(c / 10)) + (c % 10)
   *
   * ie: remove the 10s portion of 'c' and add it onto 'a'.
   *
   * To put some numbers on it, imagine we start with a = 34 and c = 27:
   *
   *  ROUND_10(34) + 27 = 40 + 27 = 67
   *
   * but also, we can split 27 up into 20 and 7 and do this:
   *
   *  ROUND_10(34 + 20) + 7 = ROUND_10(54) + 7 = 60 + 7 = 67
   *                ^^    ^
   * without affecting the result.  We do that here.
   *
   * This reduction in the size of 'c' means that we can store it in a
   * gchar instead of a gsize.  Due to how the structure is packed, this
   * ends up saving us 'two pointer sizes' per item in each tuple when
   * allocating using GSlice.
   */
  a += ~b & c;    /* take the "aligned" part of 'c' and add to 'a' */
  c &= b;         /* chop 'c' to contain only the unaligned part */


  /* Finally, we made one last adjustment.  Recall:
   *
   *   start = ROUND_UP(prev_end + a, (b + 1)) + c;
   *
   * Forgetting the '+ c' for the moment:
   *
   *   ROUND_UP(prev_end + a, (b + 1));
   *
   * we can do a "round up" operation by adding 1 less than the amount
   * to round up to, then rounding down.  ie:
   *
   *   #define ROUND_UP(x, y)    ROUND_DOWN(x + (y-1), y)
   *
   * Of course, for rounding down to a power of two, we can just mask
   * out the appropriate number of low order bits:
   *
   *   #define ROUND_DOWN(x, y)  (x & ~(y - 1))
   *
   * Which gives us
   *
   *   #define ROUND_UP(x, y)    (x + (y - 1) & ~(y - 1))
   *
   * but recall that our alignment value 'b' is already "one less".
   * This means that to round 'prev_end + a' up to 'b' we can just do:
   *
   *   ((prev_end + a) + b) & ~b
   *
   * Associativity, and putting the 'c' back on:
   *
   *   (prev_end + (a + b)) & ~b + c
   *
   * Now, since (a + b) is constant, we can just add 'b' to 'a' now and
   * store that as the number to add to prev_end.  Then we use ~b as the
   * number to take a bitwise 'and' with.  Finally, 'c' is added on.
   *
   * Note, however, that all the low order bits of the 'aligned' value
   * are masked out and that all of the high order bits of 'c' have been
   * "moved" to 'a' (in the previous step).  This means that there are
   * no overlapping bits in the addition -- so we can do a bitwise 'or'
   * equivalently.
   *
   * This means that we can now compute the start address of a given
   * item in the tuple using the algorithm given in the documentation
   * for #GVariantMemberInfo:
   *
   *   item_start = ((prev_end + a) & b) | c;
   */

  item->i = i;
  item->a = a + b;
  item->b = ~b;
  item->c = c;
}

static gsize
tuple_align (gsize offset,
             guint alignment)
{
  return offset + ((-offset) & alignment);
}

/* This function is the heart of the algorithm for calculating 'i', 'a',
 * 'b' and 'c' for each item in the tuple.
 *
 * Imagine we want to find the start of the "i" in the type "(su(qx)ni)".
 * That's a string followed by a uint32, then a tuple containing a
 * uint16 and a int64, then an int16, then our "i".  In order to get to
 * our "i" we:
 *
 * Start at the end of the string, align to 4 (for the uint32), add 4.
 * Align to 8, add 16 (for the tuple).  Align to 2, add 2 (for the
 * int16).  Then we're there.  It turns out that, given 3 simple rules,
 * we can flatten this iteration into one addition, one alignment, then
 * one more addition.
 *
 * The loop below plays through each item in the tuple, querying its
 * alignment and fixed_size into 'd' and 'e', respectively.  At all
 * times the variables 'a', 'b', and 'c' are maintained such that in
 * order to get to the current point, you add 'a', align to 'b' then add
 * 'c'.  'b' is kept in "one less than" form.  For each item, the proper
 * alignment is applied to find the values of 'a', 'b' and 'c' to get to
 * the start of that item.  Those values are recorded into the table.
 * The fixed size of the item (if applicable) is then added on.
 *
 * These 3 rules are how 'a', 'b' and 'c' are modified for alignment and
 * addition of fixed size.  They have been proven correct but are
 * presented here, without proof:
 *
 *  1) in order to "align to 'd'" where 'd' is less than or equal to the
 *     largest level of alignment seen so far ('b'), you align 'c' to
 *     'd'.
 *  2) in order to "align to 'd'" where 'd' is greater than the largest
 *     level of alignment seen so far, you add 'c' aligned to 'b' to the
 *     value of 'a', set 'b' to 'd' (ie: increase the 'largest alignment
 *     seen') and reset 'c' to 0.
 *  3) in order to "add 'e'", just add 'e' to 'c'.
 */
static void
tuple_generate_table (TupleInfo *info)
{
  GVariantMemberInfo *items = info->members;
  gsize i = -1, a = 0, b = 0, c = 0, d, e;

  /* iterate over each item in the tuple.
   *   'd' will be the alignment of the item (in one-less form)
   *   'e' will be the fixed size (or 0 for variable-size items)
   */
  while (tuple_get_item (info, items, &d, &e))
    {
      /* align to 'd' */
      if (d <= b)
        c = tuple_align (c, d);                   /* rule 1 */
      else
        a += tuple_align (c, b), b = d, c = 0;    /* rule 2 */

      /* the start of the item is at this point (ie: right after we
       * have aligned for it).  store this information in the table.
       */
      tuple_table_append (&items, i, a, b, c);

      /* "move past" the item by adding in its size. */
      if (e == 0)
        /* variable size:
         *
         * we'll have an offset stored to mark the end of this item, so
         * just bump the offset index to give us a new starting point
         * and reset all the counters.
         */
        i++, a = b = c = 0;
      else
        /* fixed size */
        c += e;                                   /* rule 3 */
    }
}

static void
tuple_set_base_info (TupleInfo *info)
{
  GVariantTypeInfo *base = &info->container.info;

  if (info->n_members > 0)
    {
      GVariantMemberInfo *m;

      /* the alignment requirement of the tuple is the alignment
       * requirement of its largest item.
       */
      base->alignment = 0;
      for (m = info->members; m < &info->members[info->n_members]; m++)
        /* can find the max of a list of "one less than" powers of two
         * by 'or'ing them
         */
        base->alignment |= m->type_info->alignment;

      m--; /* take 'm' back to the last item */

      /* the structure only has a fixed size if no variable-size
       * offsets are stored and the last item is fixed-sized too (since
       * an offset is never stored for the last item).
       */
      if (m->i == -1 && m->type_info->fixed_size)
        /* in that case, the fixed size can be found by finding the
         * start of the last item (in the usual way) and adding its
         * fixed size.
         *
         * if a tuple has a fixed size then it is always a multiple of
         * the alignment requirement (to make packing into arrays
         * easier) so we round up to that here.
         */
        base->fixed_size =
          tuple_align (((m->a & m->b) | m->c) + m->type_info->fixed_size,
                       base->alignment);
      else
        /* else, the tuple is not fixed size */
        base->fixed_size = 0;
    }
  else
    {
      /* the empty tuple: '()'.
       *
       * has a size of 1 and an no alignment requirement.
       *
       * It has a size of 1 (not 0) for two practical reasons:
       *
       *  1) So we can determine how many of them are in an array
       *     without dividing by zero or without other tricks.
       *
       *  2) Even if we had some trick to know the number of items in
       *     the array (as GVariant did at one time) this would open a
       *     potential denial of service attack: an attacker could send
       *     you an extremely small array (in terms of number of bytes)
       *     containing trillions of zero-sized items.  If you iterated
       *     over this array you would effectively infinite-loop your
       *     program.  By forcing a size of at least one, we bound the
       *     amount of computation done in response to a message to a
       *     reasonable function of the size of that message.
       */
      base->alignment = 0;
      base->fixed_size = 1;
    }
}

static ContainerInfo *
tuple_info_new (const GVariantType *type)
{
  TupleInfo *info;

  info = g_slice_new (TupleInfo);
  info->container.info.container_class = GV_TUPLE_INFO_CLASS;

  tuple_allocate_members (type, &info->members, &info->n_members);
  tuple_generate_table (info);
  tuple_set_base_info (info);

  return (ContainerInfo *) info;
}

static const GVariantMemberInfo *
g_variant_type_info_member_info (GVariantTypeInfo *info,
                                 gsize             index)
{
  TupleInfo *tuple_info = GV_TUPLE_INFO (info);

  if (index < tuple_info->n_members)
    return &tuple_info->members[index];

  return NULL;
}

static GVariantTypeInfo *
g_variant_type_info_element (GVariantTypeInfo *info)
{
  return GV_ARRAY_INFO (info)->element;
}

static GVariantTypeInfo *
g_variant_type_info_ref (GVariantTypeInfo *info)
{
  if (info->container_class)
    {
      ContainerInfo *container = (ContainerInfo *) info;

      g_assert_cmpint (container->ref_count, >, 0);
      g_atomic_int_inc (&container->ref_count);
    }

  return info;
}

static void
g_variant_type_info_unref (GVariantTypeInfo *info)
{
  if (info->container_class)
    {
      ContainerInfo *container = (ContainerInfo *) info;

      g_rec_mutex_lock (&g_variant_type_info_lock);
      if (g_atomic_int_dec_and_test (&container->ref_count))
        {
          g_hash_table_remove (g_variant_type_info_table,
                               container->type_string);
          if (g_hash_table_size (g_variant_type_info_table) == 0)
            {
              g_hash_table_unref (g_variant_type_info_table);
              g_variant_type_info_table = NULL;
            }
          g_rec_mutex_unlock (&g_variant_type_info_lock);

          g_free (container->type_string);

          if (info->container_class == GV_ARRAY_INFO_CLASS)
            array_info_free (info);

          else if (info->container_class == GV_TUPLE_INFO_CLASS)
            tuple_info_free (info);

          else
            g_assert_not_reached ();
        }
      else
        g_rec_mutex_unlock (&g_variant_type_info_lock);
    }
}

static GVariantTypeInfo *
g_variant_type_info_get (const GVariantType *type)
{
  char type_char;

  type_char = g_variant_type_peek_string (type)[0];

  if (type_char == G_VARIANT_TYPE_INFO_CHAR_MAYBE ||
      type_char == G_VARIANT_TYPE_INFO_CHAR_ARRAY ||
      type_char == G_VARIANT_TYPE_INFO_CHAR_TUPLE ||
      type_char == G_VARIANT_TYPE_INFO_CHAR_DICT_ENTRY)
    {
      GVariantTypeInfo *info;
      gchar *type_string;

      type_string = g_variant_type_dup_string (type);

      g_rec_mutex_lock (&g_variant_type_info_lock);

      if (g_variant_type_info_table == NULL)
        g_variant_type_info_table = g_hash_table_new (g_str_hash,
                                                      g_str_equal);
      info = g_hash_table_lookup (g_variant_type_info_table, type_string);

      if (info == NULL)
        {
          ContainerInfo *container;

          if (type_char == G_VARIANT_TYPE_INFO_CHAR_MAYBE ||
              type_char == G_VARIANT_TYPE_INFO_CHAR_ARRAY)
            {
              container = array_info_new (type);
            }
          else /* tuple or dict entry */
            {
              container = tuple_info_new (type);
            }

          info = (GVariantTypeInfo *) container;
          container->type_string = type_string;
          container->ref_count = 1;

          g_hash_table_insert (g_variant_type_info_table, type_string, info);
          type_string = NULL;
        }
      else
        g_variant_type_info_ref (info);

      g_rec_mutex_unlock (&g_variant_type_info_lock);
      g_free (type_string);

      return info;
    }
  else
    {
      const GVariantTypeInfo *info;
      int index;

      index = type_char - 'b';
      g_assert (G_N_ELEMENTS (g_variant_type_info_basic_table) == 24);
      g_assert_cmpint (0, <=, index);
      g_assert_cmpint (index, <, 24);

      info = g_variant_type_info_basic_table + index;

      return (GVariantTypeInfo *) info;
    }
}

static inline void
gvs_write_unaligned_le (guchar *bytes,
                        gsize   value,
                        guint   size)
{
  union
  {
    guchar bytes[GLIB_SIZEOF_SIZE_T];
    gsize integer;
  } tmpvalue;

  tmpvalue.integer = GSIZE_TO_LE (value);
  memcpy (bytes, &tmpvalue.bytes, size);
}

static guint
gvs_get_offset_size (gsize size)
{
  if (size > G_MAXUINT32)
    return 8;

  else if (size > G_MAXUINT16)
    return 4;

  else if (size > G_MAXUINT8)
    return 2;

  else if (size > 0)
    return 1;

  return 0;
}

static gsize
gvs_calculate_total_size (gsize body_size,
                          gsize offsets)
{
  if (body_size + 1 * offsets <= G_MAXUINT8)
    return body_size + 1 * offsets;

  if (body_size + 2 * offsets <= G_MAXUINT16)
    return body_size + 2 * offsets;

  if (body_size + 4 * offsets <= G_MAXUINT32)
    return body_size + 4 * offsets;

  return body_size + 8 * offsets;
}


/*****************************************************************************************
 *  End of glib code
 *****************************************************************************************/

typedef struct _OtVariantBuilderInfo OtVariantBuilderInfo;

struct _OtVariantBuilderInfo {
  OtVariantBuilderInfo *parent;
  OtVariantBuilder *builder;
  GVariantType *type;
  GVariantTypeInfo *type_info;
  guint64 offset;
  int n_children;
  GArray *child_ends;

  /* type constraint explicitly specified by 'type'.
   * for tuple types, this moves along as we add more items.
   */
  const GVariantType *expected_type;

  /* type constraint implied by previous array item.
   */
  const GVariantType *prev_item_type;
  GVariantType *prev_item_type_base;

  /* constraints on the number of children.  max = -1 for unlimited. */
  gsize min_items;
  gsize max_items;

  /* set to '1' if all items in the container will have the same type
   * (ie: maybe, array, variant) '0' if not (ie: tuple, dict entry)
   */
  guint uniform_item_types : 1;
} ;

struct _OtVariantBuilder {
  gint ref_count;
  int fd;

  /* This is only useful for the topmost builder and points to the top
   * of the builder stack. Public APIs take the topmost builder reference
   * and use this to find the currently active builder */
  OtVariantBuilderInfo *head;
};

static OtVariantBuilderInfo *
ot_variant_builder_info_new (OtVariantBuilder *builder, const GVariantType *type)
{
  OtVariantBuilderInfo *info;

  info = (OtVariantBuilderInfo *) g_slice_new0 (OtVariantBuilderInfo);

  g_return_val_if_fail (g_variant_type_is_container (type), NULL);

  info->builder = builder;
  info->type = g_variant_type_copy (type);
  info->type_info = g_variant_type_info_get (type);
  info->offset = 0;
  info->n_children = 0;
  info->child_ends = g_array_new (FALSE, TRUE, sizeof (guint64));

  switch (*(const gchar *) type)
    {
    case G_VARIANT_CLASS_VARIANT:
      info->uniform_item_types = TRUE;
      info->expected_type = NULL;
      info->min_items = 1;
      info->max_items = 1;
      break;

    case G_VARIANT_CLASS_ARRAY:
      info->uniform_item_types = TRUE;
      info->expected_type =
        g_variant_type_element (info->type);
      info->min_items = 0;
      info->max_items = -1;
      break;

    case G_VARIANT_CLASS_MAYBE:
      info->uniform_item_types = TRUE;
      info->expected_type =
        g_variant_type_element (info->type);
      info->min_items = 0;
      info->max_items = 1;
      break;

    case G_VARIANT_CLASS_DICT_ENTRY:
      info->uniform_item_types = FALSE;
      info->expected_type =
        g_variant_type_key (info->type);
      info->min_items = 2;
      info->max_items = 2;
      break;

    case 'r': /* G_VARIANT_TYPE_TUPLE was given */
      info->uniform_item_types = FALSE;
      info->expected_type = NULL;
      info->min_items = 0;
      info->max_items = -1;
      break;

    case G_VARIANT_CLASS_TUPLE: /* a definite tuple type was given */
      info->expected_type =
        g_variant_type_first (info->type);
      info->min_items = g_variant_type_n_items (type);
      info->max_items = info->min_items;
      info->uniform_item_types = FALSE;
      break;

    default:
      g_assert_not_reached ();
   }

  return info;
}

static void
ot_variant_builder_info_free (OtVariantBuilderInfo *info)
{
  if (info->parent)
    ot_variant_builder_info_free (info);

  g_variant_type_free (info->type);
  g_array_unref (info->child_ends);
  g_free (info->prev_item_type_base);

  g_slice_free (OtVariantBuilderInfo, info);
}

OtVariantBuilder *
ot_variant_builder_new (const GVariantType *type,
                        int fd)
{
  OtVariantBuilder *builder;

  builder = (OtVariantBuilder *) g_slice_new0 (OtVariantBuilder);

  g_return_val_if_fail (g_variant_type_is_container (type), NULL);

  builder->head = ot_variant_builder_info_new (builder, type);
  builder->ref_count = 1;
  builder->fd = fd;

  return builder;
}

void
ot_variant_builder_unref (OtVariantBuilder *builder)
{
  if (--builder->ref_count)
    return;

  ot_variant_builder_info_free (builder->head);

  g_slice_free (OtVariantBuilder, builder);
}

OtVariantBuilder *
ot_variant_builder_ref (OtVariantBuilder *builder)
{
  builder->ref_count++;
  return builder;
}

/* This is called before adding a child to the container.  It updates
   the internal state and does the needed alignment */
static gboolean
ot_variant_builder_pre_add (OtVariantBuilderInfo *info,
                            const GVariantType *type,
                            GError         **error)
{
  guint alignment = 0;

  if (!info->uniform_item_types)
    {
      /* advance our expected type pointers */
      if (info->expected_type)
        info->expected_type =
          g_variant_type_next (info->expected_type);

      if (info->prev_item_type)
        info->prev_item_type =
          g_variant_type_next (info->prev_item_type);
    }
  else
    {
      g_free (info->prev_item_type_base);
      info->prev_item_type_base = (GVariantType *)g_strdup ((char *)type);
      info->prev_item_type = info->prev_item_type_base;
    }

  if (g_variant_type_is_tuple (info->type) ||
      g_variant_type_is_dict_entry (info->type))
    {
      const GVariantMemberInfo *member_info;

      member_info = g_variant_type_info_member_info (info->type_info, info->n_children);
      alignment = member_info->type_info->alignment;
    }
  else if (g_variant_type_is_array (info->type))
    {
      GVariantTypeInfo *element_info = g_variant_type_info_element (info->type_info);

      alignment = element_info->alignment;
    }
  else if (g_variant_type_is_variant (info->type))
    {
      alignment = info->type_info->alignment;
    }
  else
    return glnx_throw (error, "adding to type %s not supported", (char *)info->type);

  while (info->offset & alignment)
    {
      if (glnx_loop_write (info->builder->fd, "\0", 1) < 0)
        return glnx_throw_errno (error);
      info->offset++;
    }

  return TRUE;
}

static void
ot_variant_builder_add_child_end (OtVariantBuilderInfo *info)
{
  guint64 v = info->offset;
  g_array_append_val (info->child_ends, v);
}

/* This is called after adding a child to the container. It
   updates offset, n_children and keeps track of an offset
   table if needed */

static gboolean
ot_variant_builder_post_add (OtVariantBuilderInfo *info,
                             const GVariantType *type,
                             guint64 bytes_added,
                             GError         **error)
{
  info->offset += bytes_added;

  if (g_variant_type_is_tuple (info->type) ||
      g_variant_type_is_dict_entry (info->type))
    {
      const GVariantMemberInfo *member_info;

      member_info = g_variant_type_info_member_info (info->type_info, info->n_children);
      if (member_info->ending_type == G_VARIANT_MEMBER_ENDING_OFFSET)
        ot_variant_builder_add_child_end (info);
    }
  else if (g_variant_type_is_array (info->type))
    {
      GVariantTypeInfo *element_info = g_variant_type_info_element (info->type_info);

      if (!element_info->fixed_size)
        ot_variant_builder_add_child_end (info);
    }
  else if (g_variant_type_is_variant (info->type))
    {
      /* Zero separate */
      if (glnx_loop_write (info->builder->fd, "\0", 1) < 0)
        return glnx_throw_errno (error);

      if (glnx_loop_write (info->builder->fd, (char *)type, strlen ((char *)type)) < 0)
        return glnx_throw_errno (error);

      info->offset += 1 + strlen ((char *)type);
    }
  else
    return glnx_throw (error, "adding to type %s not supported", (char *)info->type);

  info->n_children++;

  return TRUE;
}

gboolean
ot_variant_builder_add_from_fd (OtVariantBuilder    *builder,
                                const GVariantType  *type,
                                int                  fd,
                                guint64              size,
                                GError             **error)
{
  OtVariantBuilderInfo *info = builder->head;

  g_return_val_if_fail (info->n_children < info->max_items,
                        FALSE);
  g_return_val_if_fail (!info->expected_type ||
                        g_variant_type_is_subtype_of (type,
                                                      info->expected_type),
                        FALSE);
  g_return_val_if_fail (!info->prev_item_type ||
                        g_variant_type_is_subtype_of (info->prev_item_type,
                                                      type),
                        FALSE);

  if (!ot_variant_builder_pre_add (info, type, error))
    return FALSE;

  if (glnx_regfile_copy_bytes (fd, builder->fd, size) < 0)
    return glnx_throw_errno (error);

  if (!ot_variant_builder_post_add (info, type, size, error))
    return FALSE;

  return TRUE;
}

gboolean
ot_variant_builder_add_value (OtVariantBuilder *builder,
                              GVariant        *value,
                              GError         **error)
{
  OtVariantBuilderInfo *info = builder->head;
  gconstpointer data;
  gsize data_size;
  /* We ref-sink value, just like g_variant_builder_add_value does */
  g_autoptr(GVariant) keep_around_until_return G_GNUC_UNUSED = g_variant_ref_sink (value);

  g_return_val_if_fail (info->n_children < info->max_items,
                        FALSE);
  g_return_val_if_fail (!info->expected_type ||
                        g_variant_is_of_type (value,
                                              info->expected_type),
                        FALSE);
  g_return_val_if_fail (!info->prev_item_type ||
                        g_variant_is_of_type (value,
                                              info->prev_item_type),
                        FALSE);

  if (!ot_variant_builder_pre_add (info, g_variant_get_type (value), error))
    return FALSE;

  data = g_variant_get_data (value);
  data_size = g_variant_get_size (value);

  if (data)
    {
      if (glnx_loop_write (builder->fd, data, data_size) < 0)
        return glnx_throw_errno (error);
    }

  if (!ot_variant_builder_post_add (info, g_variant_get_type (value), data_size, error))
    return FALSE;

  return TRUE;
}

gboolean
ot_variant_builder_add (OtVariantBuilder *builder,
                        GError          **error,
                        const gchar     *format_string,
                        ...)
{
  GVariant *variant;
  va_list ap;

  va_start (ap, format_string);
  variant = g_variant_new_va (format_string, NULL, &ap);
  va_end (ap);

  return ot_variant_builder_add_value (builder, variant, error);
}


gboolean
ot_variant_builder_open (OtVariantBuilder *builder,
                         const GVariantType *type,
                         GError **error)
{
  OtVariantBuilderInfo *info = builder->head;
  OtVariantBuilderInfo *new_info;

  g_return_val_if_fail (info->n_children < info->max_items,
                        FALSE);
  g_return_val_if_fail (!info->expected_type ||
                        g_variant_type_is_subtype_of (type,
                                                      info->expected_type),
                        FALSE);
  g_return_val_if_fail (!info->prev_item_type ||
                        g_variant_type_is_subtype_of (info->prev_item_type,
                                                      type),
                        FALSE);

  if (!ot_variant_builder_pre_add (info, type, error))
    return FALSE;

  new_info = ot_variant_builder_info_new (builder, type);
  new_info->parent = info;

  /* push the prev_item_type down into the subcontainer */
  if (info->prev_item_type)
    {
      if (!new_info->uniform_item_types)
        /* tuples and dict entries */
        new_info->prev_item_type =
          g_variant_type_first (info->prev_item_type);

      else if (!g_variant_type_is_variant (new_info->type))
        /* maybes and arrays */
        new_info->prev_item_type =
          g_variant_type_element (info->prev_item_type);
    }

  builder->head = new_info;
  return TRUE;
}

gboolean
ot_variant_builder_close (OtVariantBuilder *builder,
                          GError **error)
{
  OtVariantBuilderInfo *info = builder->head;
  OtVariantBuilderInfo *parent;

  g_return_val_if_fail (info->parent != NULL, FALSE);

  if (!ot_variant_builder_end (builder, error))
    return FALSE;

  parent = info->parent;

  if (!ot_variant_builder_post_add (parent, info->type, info->offset, error))
    return FALSE;

  builder->head = parent;

  info->parent = NULL;
  ot_variant_builder_info_free (info);

  return TRUE;
}

gboolean
ot_variant_builder_end (OtVariantBuilder *builder,
                        GError **error)
{
  OtVariantBuilderInfo *info = builder->head;
  gsize total_size;
  gsize offset_size;
  int i;
  g_autofree guchar *offset_table = NULL;
  gsize offset_table_size;
  gboolean add_offset_table = FALSE;
  gboolean reverse_offset_table = FALSE;
  guchar *p;

  g_return_val_if_fail (info->n_children >= info->min_items,
                        FALSE);
  g_return_val_if_fail (!info->uniform_item_types ||
                        info->prev_item_type != NULL ||
                        g_variant_type_is_definite (info->type),
                        FALSE);

  if (g_variant_type_is_tuple (info->type) ||
      g_variant_type_is_dict_entry (info->type))
    {
      add_offset_table = TRUE;
      reverse_offset_table = TRUE;
    }
  else if (g_variant_type_is_array (info->type))
    {
      GVariantTypeInfo *element_info = g_variant_type_info_element (info->type_info);

      if (!element_info->fixed_size)
        add_offset_table = TRUE;
    }
  else if (g_variant_type_is_variant (info->type))
    {
      /* noop */
    }
  else
    return glnx_throw (error, "closing type %s not supported", (char *)info->type);

  if (add_offset_table)
    {
      total_size = gvs_calculate_total_size (info->offset, info->child_ends->len);
      offset_size = gvs_get_offset_size (total_size);

      offset_table_size = total_size - info->offset;
      offset_table = g_malloc (offset_table_size);
      p = offset_table;
      if (reverse_offset_table)
        {
          for (i = info->child_ends->len - 1; i >= 0; i--)
            {
              gvs_write_unaligned_le (p, g_array_index (info->child_ends, guint64, i), offset_size);
              p += offset_size;
            }
        }
      else
        {
          for (i = 0; i < info->child_ends->len; i++)
            {
              gvs_write_unaligned_le (p, g_array_index (info->child_ends, guint64, i), offset_size);
              p += offset_size;
            }
        }

      if (glnx_loop_write (builder->fd, offset_table, offset_table_size) < 0)
        return glnx_throw_errno (error);

      info->offset += offset_table_size;
    }
  else
    g_assert (info->child_ends->len == 0);

  return TRUE;
}
