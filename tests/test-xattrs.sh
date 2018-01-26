#!/bin/bash
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

skip "We don't really have a use case for committing user. xattrs right now. See also https://github.com/ostreedev/ostree/issues/758"

# Dead code below
skip_without_user_xattrs

echo "1..2"

setup_test_repository "archive"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo checkout test2 test2-checkout1
setfattr -n user.ostree-test -v testvalue test2-checkout1/firstfile
setfattr -n user.test0 -v moo test2-checkout1/firstfile
${CMD_PREFIX} ostree --repo=repo commit -b test2 -s xattrs --tree=dir=test2-checkout1
rm test2-checkout1 -rf
echo "ok commit with xattrs"

${CMD_PREFIX} ostree --repo=repo checkout test2 test2-checkout2
getfattr -m . test2-checkout2/firstfile > attrs
assert_file_has_content attrs '^user.ostree-test'
assert_file_has_content attrs '^user.test0'
getfattr -n user.ostree-test --only-values test2-checkout2/firstfile > v0
assert_file_has_content v0 '^testvalue$'
getfattr -n user.test0 --only-values test2-checkout2/firstfile > v1
assert_file_has_content v1 '^moo$'
echo "ok checkout with xattrs"
