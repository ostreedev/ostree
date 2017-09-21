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

/* Significant code derived from protobuf: */

/*
 * Protocol Buffers - Google's data interchange format
 * Copyright 2008 Google Inc.  All rights reserved.
 * http: *code.google.com/p/protobuf/
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#include "ostree-varint.h"

static const int max_varint_bytes = 10;

/**
 * _ostree_read_varuint64:
 * @buf: (array length=buflen): Byte buffer
 * @buflen: Length of bytes in @buf
 * @out_value: (out): Value
 * @bytes_read: (out): Number of bytes read
 *
 * Returns: %TRUE on success, %FALSE on end of stream
 */
gboolean
_ostree_read_varuint64 (const guint8   *buf,
                        gsize           buflen,
                        guint64        *out_value,
                        gsize          *bytes_read)
{
  guint64 result = 0;
  int count = 0;
  guint8 b;
  
  /* Adapted from CodedInputStream::ReadVarint64Slow */

  do
    {
      if (count == max_varint_bytes)
        return FALSE;
      if (buflen == 0)
        return FALSE;

      b = *buf;
      result |= ((guint64)(b & 0x7F)) << (7 * count);
      buf++;
      buflen--;
      ++count;
  } while (b & 0x80);

  *bytes_read = count;
  *out_value = result;

  return TRUE;
}

/**
 * _ostree_write_varuint64:
 * @buf: Target buffer (will contain binary data)
 * @n: Integer to encode
 *
 * Append a varint-encoded version of @n to @buf.
 */
void
_ostree_write_varuint64 (GString *buf, guint64 n)
{
  /* Splitting into 32-bit pieces gives better performance on 32-bit
   * processors. */
  guint32 part0 = (guint32)n;
  guint32 part1 = (guint32)(n >> 28);
  guint32 part2 = (guint32)(n >> 56);
  guint8 target[10];
  int i;
  int size;

  /*
   * Here we can't really optimize for small numbers, since the value is
   * split into three parts.  Cheking for numbers < 128, for instance,
   * would require three comparisons, since you'd have to make sure part1
   * and part2 are zero.  However, if the caller is using 64-bit integers,
   * it is likely that they expect the numbers to often be very large, so
   * we probably don't want to optimize for small numbers anyway.  Thus,
   * we end up with a hardcoded binary search tree...
   */
  if (part2 == 0) {
    if (part1 == 0) {
      if (part0 < (1 << 14)) {
        if (part0 < (1 << 7)) {
          size = 1; goto size1;
        } else {
          size = 2; goto size2;
        }
      } else {
        if (part0 < (1 << 21)) {
          size = 3; goto size3;
        } else {
          size = 4; goto size4;
        }
      }
    } else {
      if (part1 < (1 << 14)) {
        if (part1 < (1 << 7)) {
          size = 5; goto size5;
        } else {
          size = 6; goto size6;
        }
      } else {
        if (part1 < (1 << 21)) {
          size = 7; goto size7;
        } else {
          size = 8; goto size8;
        }
      }
    }
  } else {
    if (part2 < (1 << 7)) {
      size = 9; goto size9;
    } else {
      size = 10; goto size10;
    }
  }

  g_assert_not_reached ();

  size10: target[9] = (guint8)((part2 >>  7) | 0x80);
  size9 : target[8] = (guint8)((part2      ) | 0x80);
  size8 : target[7] = (guint8)((part1 >> 21) | 0x80);
  size7 : target[6] = (guint8)((part1 >> 14) | 0x80);
  size6 : target[5] = (guint8)((part1 >>  7) | 0x80);
  size5 : target[4] = (guint8)((part1      ) | 0x80);
  size4 : target[3] = (guint8)((part0 >> 21) | 0x80);
  size3 : target[2] = (guint8)((part0 >> 14) | 0x80);
  size2 : target[1] = (guint8)((part0 >>  7) | 0x80);
  size1 : target[0] = (guint8)((part0      ) | 0x80);

  target[size-1] &= 0x7F;

  for (i = 0; i < size; i++)
    g_string_append_c (buf, target[i]);
}
