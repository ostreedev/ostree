#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_no_selinux_or_relabel

setup_test_repository "bare"
. $(dirname $0)/basic-test.sh
