/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <ostree-async-progress.h>
#include <ostree-core.h>
#include <ostree-repo.h>
#include <ostree-mutable-tree.h>
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
#include <ostree-remote.h>
#endif
#include <ostree-repo-file.h>
#include <ostree-sysroot.h>
#include <ostree-sysroot-upgrader.h>
#include <ostree-deployment.h>
#include <ostree-bootconfig-parser.h>
#include <ostree-diff.h>
#include <ostree-gpg-verify-result.h>

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
#include <ostree-ref.h>
#include <ostree-repo-finder.h>
#include <ostree-repo-finder-avahi.h>
#include <ostree-repo-finder-config.h>
#include <ostree-repo-finder-mount.h>
#include <ostree-repo-finder-override.h>
#endif /* OSTREE_ENABLE_EXPERIMENTAL_API */

#include <ostree-autocleanups.h>
#include <ostree-version.h>
