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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <ostree-async-progress.h>
#include <ostree-bootconfig-parser.h>
#include <ostree-content-writer.h>
#include <ostree-core.h>
#include <ostree-deployment.h>
#include <ostree-diff.h>
#include <ostree-gpg-verify-result.h>
#include <ostree-kernel-args.h>
#include <ostree-mutable-tree.h>
#include <ostree-ref.h>
#include <ostree-remote.h>
#include <ostree-repo-file.h>
#include <ostree-repo-finder-avahi.h>
#include <ostree-repo-finder-config.h>
#include <ostree-repo-finder-mount.h>
#include <ostree-repo-finder-override.h>
#include <ostree-repo-finder.h>
#include <ostree-repo-os.h>
#include <ostree-repo.h>
#include <ostree-sign.h>
#include <ostree-sysroot-upgrader.h>
#include <ostree-sysroot.h>
#include <ostree-version.h>

// Include after type definitions
#include <ostree-autocleanups.h>
