/*
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
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "ostree-kernel-args.h"

G_BEGIN_DECLS

typedef struct _OstreeKernelArgsEntry OstreeKernelArgsEntry;

GHashTable *_ostree_kernel_arg_get_kargs_table (OstreeKernelArgs *kargs);

GPtrArray *_ostree_kernel_arg_get_key_array (OstreeKernelArgs *kargs);

char *_ostree_kernel_args_entry_get_key (const OstreeKernelArgsEntry *e);

char *_ostree_kernel_args_entry_get_value (const OstreeKernelArgsEntry *e);

void _ostree_kernel_args_entry_set_key (OstreeKernelArgsEntry *e, char *key);

void _ostree_kernel_args_entry_set_value (OstreeKernelArgsEntry *e, char *value);

char *_ostree_kernel_args_get_key_index (const OstreeKernelArgs *kargs, int i);

char *_ostree_kernel_args_get_value_index (const OstreeKernelArgs *kargs, int i);

OstreeKernelArgsEntry *_ostree_kernel_args_entry_new (void);

void _ostree_kernel_args_entry_value_free (OstreeKernelArgsEntry *e);

G_END_DECLS
