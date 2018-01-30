/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
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
