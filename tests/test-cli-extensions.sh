#!/usr/bin/env bash
#
# Copyright (C) 2021 Red Hat Inc.
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..2'

mkdir -p ./localbin
export PATH="./localbin/:${PATH}"
ln -s /usr/bin/env ./localbin/ostree-env
${CMD_PREFIX} ostree env --help >out.txt
assert_file_has_content out.txt "with an empty environment"
rm -rf -- localbin

echo 'ok CLI extension localbin ostree-env'

if ${CMD_PREFIX} ostree nosuchcommand 2>err.txt; then
    assert_not_reached "missing CLI extension ostree-nosuchcommand succeeded"
fi
assert_file_has_content err.txt "Unknown command 'nosuchcommand'"
rm -f -- err.txt

echo 'ok CLI extension unknown ostree-nosuchcommand'
