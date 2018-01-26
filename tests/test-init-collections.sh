#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..1'

cd ${test_tmpdir}

# Check that initialising a repository with a collection ID results in the ID being in the config.
mkdir repo
ostree_repo_init repo --collection-id org.example.Collection
assert_file_has_content repo/config "^collection-id=org.example.Collection$"

echo "ok init-collections"
