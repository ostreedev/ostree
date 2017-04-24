# Common definitions for installed, privileged tests
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

dn=$(dirname $0)
. ${dn}/libtest-core.sh

# Determine our origin refspec - we'll use this as a test base
rpmostree=$(which rpm-ostree 2>/dev/null)
if test -z "${rpmostree}"; then
    skip "no rpm-ostree, at some point point this to raw ostree too"
fi

# We need to be root
assert_streq $(id -u) 0

PYTHON=
for py in /usr/bin/python3 /usr/bin/python; do
    if ! test -x ${py}; then continue; fi
    export PYTHON=${py}
    break
done
if test -z "${PYTHON}"; then
    fatal "no python found"
fi

rpmostree_query_json() {
    query=$1
    rpm-ostree status --json | $PYTHON -c 'import json,sys; v=json.load(sys.stdin); print(v'${query}')'
}
host_refspec=$(rpmostree_query_json '["deployments"][0]["origin"]')
host_commit=$(rpmostree_query_json '["deployments"][0]["checksum"]')
host_osname=$(rpmostree_query_json '["deployments"][0]["osname"]')
