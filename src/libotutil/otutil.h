/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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

#pragma once

#include <gio/gio.h>
#include <libgsystem.h>
#include <string.h> /* Yeah...let's just do that here. */
#include <gsystem-local-alloc.h>
#include <libglnx.h>

#define ot_gobject_refz(o) (o ? g_object_ref (o) : o)

#define ot_transfer_out_value(outp, srcp) G_STMT_START {   \
  if (outp)                                                \
    {                                                      \
      *outp = *srcp;                                       \
      *(srcp) = NULL;                                      \
    }                                                      \
  } G_STMT_END;

#include <ot-keyfile-utils.h>
#include <ot-gio-utils.h>
#include <ot-fs-utils.h>
#include <ot-opt-utils.h>
#include <ot-unix-utils.h>
#include <ot-variant-utils.h>
#include <ot-spawn-utils.h>
#include <ot-checksum-utils.h>
#include <ot-gpg-utils.h>
#include <ot-log-utils.h>

void ot_ptrarray_add_many (GPtrArray  *a, ...) G_GNUC_NULL_TERMINATED; 
