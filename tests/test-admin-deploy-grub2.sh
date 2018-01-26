#!/bin/bash
#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "grub2 ostree-grub-generator"

extra_admin_tests=0

. $(dirname $0)/admin-test.sh
