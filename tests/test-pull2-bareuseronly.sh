#!/bin/bash
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_fake_remote_repo2 "archive" "--canonical-permissions"

repo_mode=bare-user-only
. ${test_srcdir}/pull-test2.sh
