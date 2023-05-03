/**
 * copyright 2002, 2003 Bryce "Zooko" Wilcox-O'Hearn
 * mailto:zooko@zooko.com
 *
 * See the end of this file for the free software, open source license (BSD-style).
 */
#include "zbase32.h"

#include <math.h>
#include <stdio.h> /* XXX only for debug printfs */
#include <stdlib.h>
#include <string.h>

static const char *const chars = "ybndrfg8ejkmcpqxot1uwisza345h769";

/* Types from zstr */
/**
 * A zstr is simply an unsigned int length and a pointer to a buffer of
 * unsigned chars.
 */
typedef struct
{
  size_t len;         /* the length of the string (not counting the null-terminating character) */
  unsigned char *buf; /* pointer to the first byte */
} zstr;

/**
 * A zstr is simply an unsigned int length and a pointer to a buffer of
 * const unsigned chars.
 */
typedef struct
{
  size_t len; /* the length of the string (not counting the null-terminating character) */
  const unsigned char *buf; /* pointer to the first byte */
} czstr;

/* Functions from zstr */
static zstr
new_z (const size_t len)
{
  zstr result;
  result.buf = (unsigned char *)malloc (len + 1);
  if (result.buf == NULL)
    {
      result.len = 0;
      return result;
    }
  result.len = len;
  result.buf[len] = '\0';
  return result;
}

/* Functions from zutil */
static size_t
divceil (size_t n, size_t d)
{
  return n / d + ((n % d) != 0);
}

static zstr
b2a_l_extra_Duffy (const czstr os, const size_t lengthinbits)
{
  zstr result = new_z (
      divceil (os.len * 8, 5)); /* if lengthinbits is not a multiple of 8 then this is allocating
                                   space for 0, 1, or 2 extra quintets that will be truncated at
                                   the end of this function if they are not needed */
  if (result.buf == NULL)
    return result;

  unsigned char *resp = result.buf + result.len; /* pointer into the result buffer, initially
                                                    pointing to the "one-past-the-end" quintet */
  const unsigned char *osp = os.buf + os.len; /* pointer into the os buffer, initially pointing to
                                                 the "one-past-the-end" octet */

  /* Now this is a real live Duff's device.  You gotta love it. */
  unsigned long x = 0; /* to hold up to 32 bits worth of the input */
  switch ((osp - os.buf) % 5)
    {
    case 0:
      do
        {
          x = *--osp;
          *--resp = chars[x % 32]; /* The least sig 5 bits go into the final quintet. */
          x /= 32;                 /* ... now we have 3 bits worth in x... */
        case 4:
          x |= ((unsigned long)(*--osp)) << 3; /* ... now we have 11 bits worth in x... */
          *--resp = chars[x % 32];
          x /= 32; /* ... now we have 6 bits worth in x... */
          *--resp = chars[x % 32];
          x /= 32; /* ... now we have 1 bits worth in x... */
        case 3:
          x |= ((unsigned long)(*--osp)) << 1; /* The 8 bits from the 2-indexed octet.  So now we
                                                  have 9 bits worth in x... */
          *--resp = chars[x % 32];
          x /= 32; /* ... now we have 4 bits worth in x... */
        case 2:
          x |= ((unsigned long)(*--osp)) << 4; /* The 8 bits from the 1-indexed octet.  So now we
                                                  have 12 bits worth in x... */
          *--resp = chars[x % 32];
          x /= 32; /* ... now we have 7 bits worth in x... */
          *--resp = chars[x % 32];
          x /= 32; /* ... now we have 2 bits worth in x... */
        case 1:
          x |= ((unsigned long)(*--osp)) << 2; /* The 8 bits from the 0-indexed octet.  So now we
                                                  have 10 bits worth in x... */
          *--resp = chars[x % 32];
          x /= 32; /* ... now we have 5 bits worth in x... */
          *--resp = chars[x];
        }
      while (osp > os.buf);
    } /* switch ((osp - os.buf) % 5) */

  /* truncate any unused trailing zero quintets */
  result.len = divceil (lengthinbits, 5);
  result.buf[result.len] = '\0';
  return result;
}

static zstr
b2a_l (const czstr os, const size_t lengthinbits)
{
  return b2a_l_extra_Duffy (os, lengthinbits);
}

static zstr
b2a (const czstr os)
{
  return b2a_l (os, os.len * 8);
}

char *
zbase32_encode (const unsigned char *data, size_t length)
{
  czstr input = { length, data };
  zstr output = b2a (input);
  return (char *)output.buf;
}

/**
 * Copyright (c) 2002 Bryce "Zooko" Wilcox-O'Hearn
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software to deal in this software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of this software, and to permit
 * persons to whom this software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of this software.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THIS SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THIS SOFTWARE.
 */
