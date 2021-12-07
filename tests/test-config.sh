#!/bin/bash
#
# Copyright (C) 2018 Sinny Kumari <skumari@redhat.com>
#
# SPDX-License-Identifier: LGPL-2.0+
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..3'

ostree_repo_init repo
${CMD_PREFIX} ostree remote add --repo=repo --set=xa.title=Flathub --set=xa.title-is-set=true flathub https://dl.flathub.org/repo/
${CMD_PREFIX} ostree remote add --repo=repo org.mozilla.FirefoxRepo http://example.com/ostree/repo/

${CMD_PREFIX} ostree config --repo=repo get core.mode > list.txt
${CMD_PREFIX} ostree config --repo=repo get --group=core repo_version >> list.txt
${CMD_PREFIX} ostree config --repo=repo get --group='remote "flathub"' 'xa.title' >> list.txt
${CMD_PREFIX} ostree config --repo=repo get --group='remote "flathub"' 'xa.title-is-set' >> list.txt
${CMD_PREFIX} ostree config --repo=repo get --group='remote "org.mozilla.FirefoxRepo"' url >> list.txt
${CMD_PREFIX} cat list.txt

assert_file_has_content list.txt "bare"
assert_file_has_content list.txt "1"
assert_file_has_content list.txt "Flathub"
assert_file_has_content list.txt "true"
assert_file_has_content list.txt "http://example\.com/ostree/repo/"

# Check that it errors out if too many arguments are given
if ${CMD_PREFIX} ostree config --repo=repo get --group=core lock-timeout-secs extra 2>err.txt; then
    assert_not_reached "ostree config get should error out if too many arguments are given"
fi
assert_file_has_content err.txt "error: Too many arguments given"
echo "ok config get"

${CMD_PREFIX} ostree config --repo=repo set core.mode bare-user-only
${CMD_PREFIX} ostree config --repo=repo set --group='remote "flathub"' 'xa.title' 'Nightly Flathub'
${CMD_PREFIX} ostree config --repo=repo set --group='remote "flathub"' 'xa.title-is-set' 'false'
${CMD_PREFIX} ostree config --repo=repo set --group='remote "org.mozilla.FirefoxRepo"' url http://example.com/ostree/

assert_file_has_content repo/config "bare-user-only"
assert_file_has_content repo/config "Nightly Flathub"
assert_file_has_content repo/config "false"
assert_file_has_content repo/config "http://example\.com/ostree/"

# Check that it errors out if too many arguments are given
if ${CMD_PREFIX} ostree config --repo=repo set --group=core lock-timeout-secs 120 extra 2>err.txt; then
    assert_not_reached "ostree config set should error out if too many arguments are given"
fi
assert_file_has_content err.txt "error: Too many arguments given"
echo "ok config set"

# Check that using `--` works and that "ostree config unset" works
${CMD_PREFIX} ostree config --repo=repo set core.lock-timeout-secs 60
assert_file_has_content repo/config "lock-timeout-secs=60"
${CMD_PREFIX} ostree config --repo=repo -- set core.lock-timeout-secs -1
assert_file_has_content repo/config "lock-timeout-secs=-1"
${CMD_PREFIX} ostree config --repo=repo unset core.lock-timeout-secs
assert_not_file_has_content repo/config "lock-timeout-secs="

# Check that "ostree config get" errors out on the key
if ${CMD_PREFIX} ostree config --repo=repo get core.lock-timeout-secs 2>err.txt; then
    assert_not_reached "ostree config get should not work after unsetting a key"
fi
# Check for any character where quotation marks would be as they appear differently in the Fedora and Debian
# test suites (“” and '' respectively). See: https://github.com/ostreedev/ostree/pull/1839
assert_file_has_content err.txt "error: Key file does not have key .lock-timeout-secs. in group .core."

# Check that it's idempotent
${CMD_PREFIX} ostree config --repo=repo unset core.lock-timeout-secs
assert_not_file_has_content repo/config "lock-timeout-secs="

# Check that the group doesn't need to exist
${CMD_PREFIX} ostree config --repo=repo unset --group='remote "aoeuhtns"' 'xa.title'

# Check that the key doesn't need to exist
${CMD_PREFIX} ostree config --repo=repo set --group='remote "aoeuhtns"' 'xa.title-is-set' 'false'
${CMD_PREFIX} ostree config --repo=repo unset --group='remote "aoeuhtns"' 'xa.title'

# Check that it errors out if too many arguments are given
if ${CMD_PREFIX} ostree config --repo=repo unset core.lock-timeout-secs extra 2>err.txt; then
    assert_not_reached "ostree config unset should error out if too many arguments are given"
fi
assert_file_has_content err.txt "error: Too many arguments given"
echo "ok config unset"
