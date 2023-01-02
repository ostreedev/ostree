#!/usr/bin/env bash
#
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh
TZ='UTC'
LANG='C'

echo "1..2"

# Explicit timestamp via CLI flag.
mkdir testrepo
ostree_repo_init testrepo --mode="archive"
mkdir testrepo-files
cd testrepo-files
echo first > firstfile
cd ..
${CMD_PREFIX} ostree --repo=./testrepo commit -b cli --timestamp='@1234567890' -s "cli timestamp"
${CMD_PREFIX} ostree --repo=./testrepo show cli > show-cli.txt
rm -rf testrepo testrepo-files
assert_file_has_content_literal show-cli.txt 'Date:  2009-02-13 23:31:30 +0000'
echo "ok commit with CLI timestamp"

# Reproducible timestamp via env flag.
mkdir testrepo
ostree_repo_init testrepo --mode="archive"
mkdir testrepo-files
cd testrepo-files
echo first > firstfile
cd ..
${CMD_PREFIX} env SOURCE_DATE_EPOCH='1234567890' ostree --repo=./testrepo commit -b env -s "env timestamp"
if (${CMD_PREFIX} env SOURCE_DATE_EPOCH='invalid' ostree --repo=./testrepo commit -b env -s "invalid timestamp") 2> commit-invalid.txt; then
    assert_not_reached "commit with invalid timestamp succeeded"
fi
if (${CMD_PREFIX} env SOURCE_DATE_EPOCH='12345678901234567890' ostree --repo=./testrepo commit -b env -s "overflowing timestamp") 2> commit-overflowing.txt; then
    assert_not_reached "commit with overflowing timestamp succeeded"
fi
${CMD_PREFIX} ostree --repo=./testrepo show env > show-env.txt
rm -rf testrepo testrepo-files
assert_file_has_content_literal commit-invalid.txt 'Failed to convert SOURCE_DATE_EPOCH'
assert_file_has_content commit-overflowing.txt 'Parsing SOURCE_DATE_EPOCH: \(Numerical result out of range\|Result not representable\)'
assert_file_has_content_literal show-env.txt 'Date:  2009-02-13 23:31:30 +0000'
echo "ok commit with env timestamp"
