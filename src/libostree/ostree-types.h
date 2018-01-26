/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

#ifndef _OSTREE_PUBLIC
#define _OSTREE_PUBLIC extern
#endif

G_BEGIN_DECLS

typedef struct OstreeRepo OstreeRepo;
typedef struct OstreeRepoDevInoCache OstreeRepoDevInoCache;
typedef struct OstreeSePolicy OstreeSePolicy;
typedef struct OstreeSysroot OstreeSysroot;
typedef struct OstreeSysrootUpgrader OstreeSysrootUpgrader;
typedef struct OstreeMutableTree OstreeMutableTree;
typedef struct OstreeRepoFile OstreeRepoFile;

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
typedef struct OstreeRemote OstreeRemote;
#endif

G_END_DECLS
