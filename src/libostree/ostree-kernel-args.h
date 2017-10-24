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
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include "libglnx.h"

G_BEGIN_DECLS

typedef struct _OstreeKernelArgs OstreeKernelArgs;
void _ostree_kernel_args_free (OstreeKernelArgs *kargs);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeKernelArgs, _ostree_kernel_args_free);

OstreeKernelArgs *_ostree_kernel_args_new (void);
void _ostree_kernel_args_replace_take (OstreeKernelArgs  *kargs,
                                       char              *key);
void _ostree_kernel_args_replace (OstreeKernelArgs  *kargs,
                                  const char        *key);
void _ostree_kernel_args_replace_argv (OstreeKernelArgs  *kargs,
                                       char             **argv);
void _ostree_kernel_args_append (OstreeKernelArgs  *kargs,
                                 const char     *key);
void _ostree_kernel_args_append_argv (OstreeKernelArgs  *kargs,
                                      char **argv);
void _ostree_kernel_args_append_argv_filtered (OstreeKernelArgs  *kargs,
                                               char **argv,
                                               char **prefixes);

gboolean _ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs,
                                                  GCancellable     *cancellable,
                                                  GError          **error);

void _ostree_kernel_args_parse_append (OstreeKernelArgs *kargs,
                                       const char *options);

const char *_ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs, const char *key);

OstreeKernelArgs * _ostree_kernel_args_from_string (const char *options);

char ** _ostree_kernel_args_to_strv (OstreeKernelArgs *kargs);
char * _ostree_kernel_args_to_string (OstreeKernelArgs *kargs);

G_END_DECLS
