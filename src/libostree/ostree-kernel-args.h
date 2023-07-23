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

#include "ostree-types.h"
#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct _OstreeKernelArgs OstreeKernelArgs;

_OSTREE_PUBLIC
void ostree_kernel_args_free (OstreeKernelArgs *kargs);

_OSTREE_PUBLIC
OstreeKernelArgs *ostree_kernel_args_new (void);

_OSTREE_PUBLIC
void ostree_kernel_args_cleanup (void *loc);

_OSTREE_PUBLIC
void ostree_kernel_args_replace_take (OstreeKernelArgs *kargs, char *arg);

_OSTREE_PUBLIC
void ostree_kernel_args_replace (OstreeKernelArgs *kargs, const char *arg);

_OSTREE_PUBLIC
void ostree_kernel_args_replace_argv (OstreeKernelArgs *kargs, char **argv);

_OSTREE_PUBLIC
void ostree_kernel_args_append (OstreeKernelArgs *kargs, const char *arg);

_OSTREE_PUBLIC
void ostree_kernel_args_append_argv (OstreeKernelArgs *kargs, char **argv);

_OSTREE_PUBLIC
void ostree_kernel_args_append_argv_filtered (OstreeKernelArgs *kargs, char **argv,
                                              char **prefixes);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_new_replace (OstreeKernelArgs *kargs, const char *arg, GError **error);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_delete (OstreeKernelArgs *kargs, const char *arg, GError **error);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_delete_key_entry (OstreeKernelArgs *kargs, const char *key,
                                              GError **error);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs, GCancellable *cancellable,
                                                 GError **error);

_OSTREE_PUBLIC
void ostree_kernel_args_parse_append (OstreeKernelArgs *kargs, const char *options);

_OSTREE_PUBLIC
const char *ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs, const char *key);

_OSTREE_PUBLIC
OstreeKernelArgs *ostree_kernel_args_from_string (const char *options);

_OSTREE_PUBLIC
char **ostree_kernel_args_to_strv (OstreeKernelArgs *kargs);

_OSTREE_PUBLIC
char *ostree_kernel_args_to_string (OstreeKernelArgs *kargs);

_OSTREE_PUBLIC
void ostree_kernel_args_append_if_missing (OstreeKernelArgs *kargs, const char *arg);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_contains (OstreeKernelArgs *kargs, const char *arg);

_OSTREE_PUBLIC
gboolean ostree_kernel_args_delete_if_present (OstreeKernelArgs *kargs, const char *arg,
                                               GError **error);
G_END_DECLS
