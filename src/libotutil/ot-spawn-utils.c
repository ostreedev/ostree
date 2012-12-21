/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "otutil.h"

#include <string.h>

/**
 * ot_thread_pool_new_nproc:
 *
 * Like g_thread_pool_new (), but choose number of threads appropriate
 * for CPU bound workers automatically.  Also aborts on error.
 */
GThreadPool *
ot_thread_pool_new_nproc (GFunc     func,
                          gpointer  user_data)
{
  long nproc_onln;
  GThreadPool *ret;
  GError *local_error = NULL;

  nproc_onln = sysconf (_SC_NPROCESSORS_ONLN);
  if (G_UNLIKELY (nproc_onln == -1 && errno == EINVAL))
    nproc_onln = 2;
  ret = g_thread_pool_new (func, user_data, (int)nproc_onln, FALSE, &local_error);
  g_assert_no_error (local_error);
  return ret;
}
