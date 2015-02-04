#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -e

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive-z2"

# Now, setup multiple branches
mkdir ${test_tmpdir}/ostree-srv/other-files
cd ${test_tmpdir}/ostree-srv/other-files
echo 'hello world another object' > hello-world
ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b other -s "A commit" -m "Another Commit body"

mkdir ${test_tmpdir}/ostree-srv/yet-other-files
cd ${test_tmpdir}/ostree-srv/yet-other-files
echo 'hello world yet another object' > yet-another-hello-world
ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b yet-another -s "A commit" -m "Another Commit body"

ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u

cd ${test_tmpdir}
mkdir repo
ostree --repo=repo init --mode=archive-z2
ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
ostree --repo=repo pull --mirror origin
assert_has_file repo/summary
ostree --repo=repo checkout -U main main-copy
assert_file_has_content main-copy/baz/cow "moo"
ostree --repo=repo checkout -U other other-copy
assert_file_has_content other-copy/hello-world "hello world another object"
ostree --repo=repo checkout -U yet-another yet-another-copy
assert_file_has_content yet-another-copy/yet-another-hello-world "hello world yet another object"
ostree --repo=repo fsck
echo "ok pull mirror summary"
