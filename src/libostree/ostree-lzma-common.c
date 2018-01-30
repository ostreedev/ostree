/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-lzma-common.h"

#include <errno.h>
#include <lzma.h>
#include <string.h>

GConverterResult
_ostree_lzma_return (lzma_ret   res,
                     GError   **error)
{
  switch (res)
    {
    case LZMA_OK:
      return G_CONVERTER_CONVERTED;
    case LZMA_STREAM_END:
      return G_CONVERTER_FINISHED;
    case LZMA_NO_CHECK:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Stream is corrupt");
      return G_CONVERTER_ERROR;
    case LZMA_UNSUPPORTED_CHECK:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Cannot calculate integrity check");
      return G_CONVERTER_ERROR;
    case LZMA_MEM_ERROR:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Out of memory");
      return G_CONVERTER_ERROR;
    case LZMA_MEMLIMIT_ERROR:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Exceeded memory limit");
      return G_CONVERTER_ERROR;
    case LZMA_FORMAT_ERROR:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "File format not recognized");
      return G_CONVERTER_ERROR;
    case LZMA_OPTIONS_ERROR:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Invalid or unsupported options");
      return G_CONVERTER_ERROR;
    case LZMA_DATA_ERROR:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Data is corrupt");
      return G_CONVERTER_ERROR;
    case LZMA_BUF_ERROR:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
         "Input buffer too small");
      return G_CONVERTER_ERROR;
    default:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   "Unrecognized LZMA error");
      return G_CONVERTER_ERROR;
    }
}
