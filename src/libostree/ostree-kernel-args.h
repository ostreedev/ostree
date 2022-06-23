/*
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include "ostree-types.h"

G_BEGIN_DECLS

typedef struct _OstreeKernelArgs OstreeKernelArgs;
typedef struct _OstreeKernelArgsEntry OstreeKernelArgsEntry;

GHashTable *_ostree_kernel_arg_get_kargs_table (OstreeKernelArgs *kargs);

GPtrArray *_ostree_kernel_arg_get_key_array (OstreeKernelArgs *kargs);

char *
_ostree_kernel_args_entry_get_key (const OstreeKernelArgsEntry *e);

char *
_ostree_kernel_args_entry_get_value (const OstreeKernelArgsEntry *e);

void
_ostree_kernel_args_entry_set_key (OstreeKernelArgsEntry *e,
                                   char  *key);

void
_ostree_kernel_args_entry_set_value (OstreeKernelArgsEntry *e,
                                     char  *value);

char *
_ostree_kernel_args_get_key_index (const OstreeKernelArgs *kargs,
                                   int i);

char *
_ostree_kernel_args_get_value_index (const OstreeKernelArgs *kargs,
                                     int i);

OstreeKernelArgsEntry *
_ostree_kernel_args_entry_new (void);

void
_ostree_kernel_args_entry_value_free (OstreeKernelArgsEntry *e);

_OSTREE_PUBLIC
void ostree_kernel_args_free (OstreeKernelArgs *kargs);

_OSTREE_PUBLIC
OstreeKernelArgs *ostree_kernel_args_new (void);

_OSTREE_PUBLIC
void ostree_kernel_args_cleanup (void *loc);

_OSTREE_PUBLIC
void ostree_kernel_args_replace_take (OstreeKernelArgs  *kargs,
                                      char              *arg);

_OSTREE_PUBLIC
void ostree_kernel_args_replace (OstreeKernelArgs  *kargs,
                                 const char        *arg);

_OSTREE_PUBLIC
void ostree_kernel_args_replace_argv (OstreeKernelArgs  *kargs,
                                      char             **argv);

_OSTREE_PUBLIC
void ostree_kernel_args_append (OstreeKernelArgs  *kargs,
                                const char     *arg);

_OSTREE_PUBLIC
void ostree_kernel_args_append_argv (OstreeKernelArgs  *kargs,
                                     char **argv);

_OSTREE_PUBLIC
void ostree_kernel_args_append_argv_filtered (OstreeKernelArgs  *kargs,
                                              char **argv,
                                              char **prefixes);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_new_replace (OstreeKernelArgs *kargs,
                                         const char       *arg,
                                         GError          **error);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_delete (OstreeKernelArgs *kargs,
                                    const char       *arg,
                                    GError           **error);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_delete_key_entry (OstreeKernelArgs *kargs,
                                              const char       *key,
                                              GError          **error);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs,
                                                 GCancellable     *cancellable,
                                                 GError          **error);

_OSTREE_PUBLIC
void ostree_kernel_args_parse_append (OstreeKernelArgs *kargs,
                                      const char *options);

_OSTREE_PUBLIC
const char *ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs,
                                               const char *key);

_OSTREE_PUBLIC
OstreeKernelArgs *ostree_kernel_args_from_string (const char *options);

_OSTREE_PUBLIC
char **ostree_kernel_args_to_strv (OstreeKernelArgs *kargs);

_OSTREE_PUBLIC
char *ostree_kernel_args_to_string (OstreeKernelArgs *kargs);

_OSTREE_PUBLIC
void ostree_kernel_args_append_if_missing (OstreeKernelArgs *kargs, 
                                           const char *arg);

G_END_DECLS
