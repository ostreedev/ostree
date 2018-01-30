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

#include <assert.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <stdint.h>
#include <string.h>

#include "ostree-bloom-private.h"

/**
 * SECTION:bloom
 * @title: Bloom filter
 * @short_description: Bloom filter implementation supporting building and
 *    reading filters
 * @stability: Unstable
 * @include: libostree/ostree-bloom-private.h
 *
 * #OstreeBloom is an implementation of a bloom filter which supports writing to
 * and loading from a #GBytes bit array. The caller must store metadata about
 * the bloom filter (its hash function and `k` parameter value) separately, as
 * the same values must be used when reading from a serialised bit array as were
 * used to build the array in the first place.
 *
 * This is a standard implementation of a bloom filter, and background reading
 * on the theory can be
 * [found on Wikipedia](https://en.wikipedia.org/wiki/Bloom_filter). In
 * particular, a bloom filter is parameterised by `m` and `k` parameters: the
 * size of the bit array (in bits) is `m`, and the number of hash functions
 * applied to each element is `k`. Bloom filters require a universal hash
 * function which can be parameterised by `k`. We have #OstreeBloomHashFunc,
 * with ostree_str_bloom_hash() being an implementation for strings.
 *
 * The serialised output from a bloom filter is guaranteed to be stable across
 * versions of libostree as long as the same values for `k` and the hash
 * function are used.
 *
 * #OstreeBloom is mutable when constructed with ostree_bloom_new(), and elements
 * can be added to it using ostree_bloom_add_element(), until ostree_bloom_seal()
 * is called to serialise it and make it immutable. After then, the bloom filter
 * can only be queried using ostree_bloom_maybe_contains().
 *
 * If constructed with ostree_bloom_new_from_bytes(), the bloom filter is
 * immutable from construction, and can only be queried.
 *
 * Reference:
 *  - https://en.wikipedia.org/wiki/Bloom_filter
 *  - https://llimllib.github.io/bloomfilter-tutorial/
 *
 * Since: 2017.8
 */

struct _OstreeBloom
{
  guint ref_count;
  gsize n_bytes;  /* 0 < n_bytes <= G_MAXSIZE / 8 */
  gboolean is_mutable;  /* determines which of [im]mutable_bytes is accessed */
  union
    {
      guint8 *mutable_bytes;  /* owned; mutually exclusive */
      GBytes *immutable_bytes;  /* owned; mutually exclusive */
    };
  guint8 k;
  OstreeBloomHashFunc hash_func;
};

G_DEFINE_BOXED_TYPE (OstreeBloom, ostree_bloom, ostree_bloom_ref, ostree_bloom_unref)

/**
 * ostree_bloom_new:
 * @n_bytes: size to make the bloom filter, in bytes
 * @k: number of hash functions to use
 * @hash_func: universal hash function to use
 *
 * Create a new mutable #OstreeBloom filter, with all its bits initialised to
 * zero. Set elements in the filter using ostree_bloom_add_element(), and seal
 * it to return an immutable #GBytes using ostree_bloom_seal().
 *
 * To load an #OstreeBloom from an existing #GBytes, use
 * ostree_bloom_new_from_bytes().
 *
 * Note that @n_bytes is in bytes, so is 8 times smaller than the parameter `m`
 * which is used when describing bloom filters academically.
 *
 * Returns: (transfer full): a new mutable bloom filter
 *
 * Since: 2017.8
 */
OstreeBloom *
ostree_bloom_new (gsize               n_bytes,
                  guint8              k,
                  OstreeBloomHashFunc hash_func)
{
  g_autoptr(OstreeBloom) bloom = NULL;

  g_return_val_if_fail (n_bytes > 0, NULL);
  g_return_val_if_fail (n_bytes <= G_MAXSIZE / 8, NULL);
  g_return_val_if_fail (k > 0, NULL);
  g_return_val_if_fail (hash_func != NULL, NULL);

  bloom = g_new0 (OstreeBloom, 1);
  bloom->ref_count = 1;

  bloom->is_mutable = TRUE;
  bloom->mutable_bytes = g_malloc0 (n_bytes);
  bloom->n_bytes = n_bytes;
  bloom->k = k;
  bloom->hash_func = hash_func;

  return g_steal_pointer (&bloom);
}

/**
 * ostree_bloom_new_from_bytes:
 * @bytes: array of bytes containing the filter data
 * @k: number of hash functions to use
 * @hash_func: universal hash function to use
 *
 * Load an immutable #OstreeBloom filter from the given @bytes. Check whether
 * elements are probably set in the filter using ostree_bloom_maybe_contains().
 *
 * To create a new mutable #OstreeBloom, use ostree_bloom_new().
 *
 * Note that all the bits in @bytes are loaded, so the parameter `m` for the
 * filter (as commonly used in academic literature) is always a multiple of 8.
 *
 * Returns: (transfer full): a new immutable bloom filter
 *
 * Since: 2017.8
 */
OstreeBloom *
ostree_bloom_new_from_bytes (GBytes              *bytes,
                             guint8               k,
                             OstreeBloomHashFunc  hash_func)
{
  g_autoptr(OstreeBloom) bloom = NULL;

  g_return_val_if_fail (bytes != NULL, NULL);
  g_return_val_if_fail (g_bytes_get_size (bytes) > 0, NULL);
  g_return_val_if_fail (g_bytes_get_size (bytes) <= G_MAXSIZE / 8, NULL);
  g_return_val_if_fail (k > 0, NULL);
  g_return_val_if_fail (hash_func != NULL, NULL);

  bloom = g_new0 (OstreeBloom, 1);
  bloom->ref_count = 1;

  bloom->is_mutable = FALSE;
  bloom->immutable_bytes = g_bytes_ref (bytes);
  bloom->n_bytes = g_bytes_get_size (bytes);
  bloom->k = k;
  bloom->hash_func = hash_func;

  return g_steal_pointer (&bloom);
}

/**
 * ostree_bloom_ref:
 * @bloom: an #OstreeBloom
 *
 * Increase the reference count of @bloom.
 *
 * Returns: (transfer full): @bloom
 * Since: 2017.8
 */
OstreeBloom *
ostree_bloom_ref (OstreeBloom *bloom)
{
  g_return_val_if_fail (bloom != NULL, NULL);
  g_return_val_if_fail (bloom->ref_count >= 1, NULL);
  g_return_val_if_fail (bloom->ref_count == G_MAXUINT - 1, NULL);

  bloom->ref_count++;

  return bloom;
}

/**
 * ostree_bloom_unref:
 * @bloom: (transfer full): an #OstreeBloom
 *
 * Decrement the reference count of @bloom. If it reaches zero, the filter
 * is destroyed.
 *
 * Since: 2017.8
 */
void
ostree_bloom_unref (OstreeBloom *bloom)
{
  g_return_if_fail (bloom != NULL);
  g_return_if_fail (bloom->ref_count >= 1);

  bloom->ref_count--;

  if (bloom->ref_count == 0)
    {
      if (bloom->is_mutable)
        g_clear_pointer (&bloom->mutable_bytes, g_free);
      else
        g_clear_pointer (&bloom->immutable_bytes, g_bytes_unref);
      bloom->n_bytes = 0;
      g_free (bloom);
    }
}

/* @idx is in bits, not bytes. */
static inline gboolean
ostree_bloom_get_bit (OstreeBloom *bloom,
                      gsize        idx)
{
  const guint8 *bytes;

  if (bloom->is_mutable)
    bytes = bloom->mutable_bytes;
  else
    bytes = g_bytes_get_data (bloom->immutable_bytes, NULL);

  g_assert (idx / 8 < bloom->n_bytes);
  return (bytes[idx / 8] & (1 << (idx % 8)));
}

/* @idx is in bits, not bytes. */
static inline void
ostree_bloom_set_bit (OstreeBloom *bloom,
                      gsize        idx)
{
  g_assert (bloom->is_mutable);
  g_assert (idx / 8 < bloom->n_bytes);
  bloom->mutable_bytes[idx / 8] |= (guint8) (1 << (idx % 8));
}

/**
 * ostree_bloom_maybe_contains:
 * @bloom: an #OstreeBloom
 * @element: (nullable): element to check for membership
 *
 * Check whether @element is potentially in @bloom, or whether it definitely
 * isn’t. @element may be %NULL only if the hash function passed to @bloom at
 * construction time supports %NULL elements.
 *
 * Returns: %TRUE if @element is potentially in @bloom; %FALSE if it definitely
 *    isn’t
 * Since: 2017.8
 */
gboolean
ostree_bloom_maybe_contains (OstreeBloom   *bloom,
                             gconstpointer  element)
{
  guint8 i;

  g_return_val_if_fail (bloom != NULL, TRUE);
  g_return_val_if_fail (bloom->ref_count >= 1, TRUE);

  for (i = 0; i < bloom->k; i++)
    {
      guint64 idx;

      idx = bloom->hash_func (element, i);

      if (!ostree_bloom_get_bit (bloom, (gsize) (idx % (bloom->n_bytes * 8))))
        return FALSE;  /* definitely not in the set */
    }

  return TRUE;  /* possibly in the set */
}

/**
 * ostree_bloom_seal:
 * @bloom: an #OstreeBloom
 *
 * Seal a constructed bloom filter, so that elements may no longer be added to
 * it, and queries can now be performed against it. The serialised form of the
 * bloom filter is returned as a bit array. Note that this does not include
 * information about the filter hash function or parameters; the caller is
 * responsible for serialising those separately if appropriate.
 *
 * It is safe to call this function multiple times.
 *
 * Returns: (transfer full): a #GBytes containing the immutable filter data
 * Since: 2017.8
 */
GBytes *
ostree_bloom_seal (OstreeBloom *bloom)
{
  g_return_val_if_fail (bloom != NULL, NULL);
  g_return_val_if_fail (bloom->ref_count >= 1, NULL);

  if (bloom->is_mutable)
    {
      bloom->is_mutable = FALSE;
      bloom->immutable_bytes = g_bytes_new_take (g_steal_pointer (&bloom->mutable_bytes), bloom->n_bytes);
    }

  return g_bytes_ref (bloom->immutable_bytes);
}

/**
 * ostree_bloom_add_element:
 * @bloom: an #OstreeBloom
 * @element: (nullable): element to add to the filter
 *
 * Add the given @element to the bloom filter, which must not yet have been
 * sealed (ostree_bloom_seal()). @element may be %NULL if the hash function
 * passed to @bloom at construction time supports %NULL elements.
 *
 * Since: 2017.8
 */
void
ostree_bloom_add_element (OstreeBloom   *bloom,
                          gconstpointer  element)
{
  guint8 i;

  g_return_if_fail (bloom != NULL);
  g_return_if_fail (bloom->ref_count >= 1);
  g_return_if_fail (bloom->is_mutable);

  for (i = 0; i < bloom->k; i++)
    {
      guint64 idx = bloom->hash_func (element, i);
      ostree_bloom_set_bit (bloom, (gsize) (idx % (bloom->n_bytes * 8)));
    }
}

/**
 * ostree_bloom_get_size:
 * @bloom: an #OstreeBloom
 *
 * Get the size of the #OstreeBloom filter, in bytes, as configured at
 * construction time.
 *
 * Returns: the bloom filter’s size in bytes, guaranteed to be >0
 * Since: 2017.8
 */
gsize
ostree_bloom_get_size (OstreeBloom *bloom)
{
  g_return_val_if_fail (bloom != NULL, 0);

  return bloom->n_bytes;
}

/**
 * ostree_bloom_get_k:
 * @bloom: an #OstreeBloom
 *
 * Get the `k` value from the #OstreeBloom filter, as configured at
 * construction time.
 *
 * Returns: the bloom filter’s `k` value, guaranteed to be >0
 * Since: 2017.8
 */
guint8
ostree_bloom_get_k (OstreeBloom *bloom)
{
  g_return_val_if_fail (bloom != NULL, 0);

  return bloom->k;
}

/**
 * ostree_bloom_get_hash_func:
 * @bloom: an #OstreeBloom
 *
 * Get the #OstreeBloomHashFunc from the #OstreeBloom filter, as configured at
 * construction time.
 *
 * Returns: the bloom filter’s universal hash function
 * Since: 2017.8
 */
OstreeBloomHashFunc
ostree_bloom_get_hash_func (OstreeBloom *bloom)
{
  g_return_val_if_fail (bloom != NULL, NULL);

  return bloom->hash_func;
}

/* SipHash code adapted from https://github.com/veorq/SipHash/blob/master/siphash.c */

/*
   SipHash reference C implementation
   Copyright (c) 2012-2016 Jean-Philippe Aumasson
   <jeanphilippe.aumasson@gmail.com>
   Copyright (c) 2012-2014 Daniel J. Bernstein <djb@cr.yp.to>
   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.
   You should have received a copy of the CC0 Public Domain Dedication along
   with
   this software. If not, see
   <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

/* default: SipHash-2-4 */
#define cROUNDS 2
#define dROUNDS 4

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define U32TO8_LE(p, v)                                                        \
    (p)[0] = (uint8_t)((v));                                                   \
    (p)[1] = (uint8_t)((v) >> 8);                                              \
    (p)[2] = (uint8_t)((v) >> 16);                                             \
    (p)[3] = (uint8_t)((v) >> 24);

#define U64TO8_LE(p, v)                                                        \
    U32TO8_LE((p), (uint32_t)((v)));                                           \
    U32TO8_LE((p) + 4, (uint32_t)((v) >> 32));

#define U8TO64_LE(p)                                                           \
    (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) |                        \
     ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) |                 \
     ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) |                 \
     ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL(v1, 13);                                                     \
        v1 ^= v0;                                                              \
        v0 = ROTL(v0, 32);                                                     \
        v2 += v3;                                                              \
        v3 = ROTL(v3, 16);                                                     \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL(v3, 21);                                                     \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL(v1, 17);                                                     \
        v1 ^= v2;                                                              \
        v2 = ROTL(v2, 32);                                                     \
    } while (0)

#ifdef DEBUG
#define TRACE                                                                  \
    do {                                                                       \
        printf("(%3d) v0 %08x %08x\n", (int)inlen, (uint32_t)(v0 >> 32),       \
               (uint32_t)v0);                                                  \
        printf("(%3d) v1 %08x %08x\n", (int)inlen, (uint32_t)(v1 >> 32),       \
               (uint32_t)v1);                                                  \
        printf("(%3d) v2 %08x %08x\n", (int)inlen, (uint32_t)(v2 >> 32),       \
               (uint32_t)v2);                                                  \
        printf("(%3d) v3 %08x %08x\n", (int)inlen, (uint32_t)(v3 >> 32),       \
               (uint32_t)v3);                                                  \
    } while (0)
#else
#define TRACE
#endif

static int siphash(const uint8_t *in, const size_t inlen, const uint8_t *k,
                   uint8_t *out, const size_t outlen) {

    assert((outlen == 8) || (outlen == 16));
    uint64_t v0 = 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;
    uint64_t k0 = U8TO64_LE(k);
    uint64_t k1 = U8TO64_LE(k + 8);
    uint64_t m;
    int i;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint64_t));
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    if (outlen == 16)
        v1 ^= 0xee;

    for (; in != end; in += 8) {
        m = U8TO64_LE(in);
        v3 ^= m;

        TRACE;
        for (i = 0; i < cROUNDS; ++i)
            SIPROUND;

        v0 ^= m;
    }

    switch (left) {
    case 7:
        b |= ((uint64_t)in[6]) << 48;
    case 6:
        b |= ((uint64_t)in[5]) << 40;
    case 5:
        b |= ((uint64_t)in[4]) << 32;
    case 4:
        b |= ((uint64_t)in[3]) << 24;
    case 3:
        b |= ((uint64_t)in[2]) << 16;
    case 2:
        b |= ((uint64_t)in[1]) << 8;
    case 1:
        b |= ((uint64_t)in[0]);
        break;
    case 0:
        break;
    }

    v3 ^= b;

    TRACE;
    for (i = 0; i < cROUNDS; ++i)
        SIPROUND;

    v0 ^= b;

    if (outlen == 16)
        v2 ^= 0xee;
    else
        v2 ^= 0xff;

    TRACE;
    for (i = 0; i < dROUNDS; ++i)
        SIPROUND;

    b = v0 ^ v1 ^ v2 ^ v3;
    U64TO8_LE(out, b);

    if (outlen == 8)
        return 0;

    v1 ^= 0xdd;

    TRACE;
    for (i = 0; i < dROUNDS; ++i)
        SIPROUND;

    b = v0 ^ v1 ^ v2 ^ v3;
    U64TO8_LE(out + 8, b);

    return 0;
}

/* End SipHash copied code. */

/**
 * ostree_str_bloom_hash:
 * @element: element to calculate the hash for
 * @k: hash function index
 *
 * A universal hash function implementation for strings. It expects @element to
 * be a pointer to a string (i.e. @element has type `const gchar*`), and expects
 * @k to be in the range `[0, k_max)`, where `k_max` is the `k` value used to
 * construct the bloom filter. The output range from this hash function could be
 * any value in #guint64, and it handles input strings of any length.
 *
 * This function does not allow %NULL as a valid value for @element.
 *
 * Reference:
 *  - https://www.131002.net/siphash/
 *
 * Returns: hash of the string at @element using parameter @k
 * Since: 2017.8
 */
guint64
ostree_str_bloom_hash (gconstpointer element,
                       guint8        k)
{
  const gchar *str = element;
  gsize str_len;
  union
    {
      guint64 u64;
      guint8 u8[8];
    } out_le;
  guint8 k_array[16];
  gsize i;

  str_len = strlen (str);
  for (i = 0; i < G_N_ELEMENTS (k_array); i++)
    k_array[i] = k;

  siphash ((const guint8 *) str, str_len, k_array, out_le.u8, sizeof (out_le));

  return le64toh (out_le.u64);
}
