#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..1'

cd ${test_tmpdir}

# Check that adding a remote with a collection ID results in the ID being in the config.
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add some-remote https://example.com/ --collection-id example-id --gpg-import=${test_tmpdir}/gpghome/key1.asc

assert_file_has_content repo/config "^collection-id=example-id$"

echo "ok remote-add-collections"
