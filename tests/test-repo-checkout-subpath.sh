#!/bin/bash
#
# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
# Copyright (C) 2014 Red Hat, Inc.
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

set -euo pipefail

. $(dirname $0)/libtest.sh

setup_test_repository "bare"
echo "ok setup"

echo '1..2'

repopath=${test_tmpdir}/ostree-srv/gnomerepo

${CMD_PREFIX} ostree --repo=repo checkout -U --subpath=/ test2 checkedout

${CMD_PREFIX} ostree --repo=repo checkout -U --subpath=/firstfile test2 checkedout2
