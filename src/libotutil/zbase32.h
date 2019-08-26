/**
 * copyright 2002, 2003 Bryce "Zooko" Wilcox-O'Hearn
 * mailto:zooko@zooko.com
 *
 * See the end of this file for the free software, open source license (BSD-style).
 */
#ifndef __INCL_base32_h
#define __INCL_base32_h

static char const* const base32_h_cvsid = "$Id: base32.h,v 1.11 2003/12/15 01:16:19 zooko Exp $";

static int const base32_vermaj = 0;
static int const base32_vermin = 9;
static int const base32_vermicro = 12;
static char const* const base32_vernum = "0.9.12";

#include <assert.h>
#include <stddef.h>

/**
 * @param data to be zbase-32 encoded
 * @param length size of the data buffer
 *
 * @return an allocated string containing the zbase-32 encoded representation
 */
char *zbase32_encode(const unsigned char *data, size_t length);

#endif /* #ifndef __INCL_base32_h */

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
