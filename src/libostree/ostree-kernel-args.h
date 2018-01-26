/*
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
