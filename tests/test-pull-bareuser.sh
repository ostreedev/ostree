#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_user_xattrs
setup_fake_remote_repo1 "archive"

repo_mode=bare-user
. ${test_srcdir}/pull-test.sh
