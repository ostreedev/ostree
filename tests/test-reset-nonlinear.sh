#!/bin/bash
#
# Copyright (C) 2015 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

echo "1..1"

. $(dirname $0)/libtest.sh

setup_test_repository "archive"
cd ${test_tmpdir}/files
$OSTREE commit -b testx -s "Another Commit"
cd ${test_tmpdir}
$OSTREE reset test2 testx

echo "ok reset nonlinear"
