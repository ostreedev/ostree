# Common definitions for installed, privileged tests
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

. ${KOLA_EXT_DATA}/libtest-core.sh

# Copy of bits from tap-test
test_tmpdir=
function _tmpdir_cleanup () {
    if test -z "${TEST_SKIP_CLEANUP:-}" &&
       test -n "${test_tmpdir}" && test -f ${test_tmpdir}/.testtmp; then
	      rm "${test_tmpdir}" -rf
    fi
}
prepare_tmpdir() {
    local tmpdir=${1:-/var/tmp}
    test_tmpdir=$(mktemp -p ${tmpdir} -d ostree-insttest.XXXXXXXX)
    touch ${test_tmpdir}/.testtmp
    cd ${test_tmpdir}
}

# This is copied from flatpak/flatpak/tests/test-webserver.sh
run_tmp_webserver() {
    dir=$1

    port=8000
    podman create --name ostree-httpd --privileged --user root -ti --net=host -v "${dir}":/srv --workdir /srv \
        quay.io/coreos-assembler/fcos-buildroot:testing-devel python3 -m http.server "${port}"
    podman generate systemd ostree-httpd > /etc/systemd/system/ostree-httpd.service
    systemctl daemon-reload
    systemctl start ostree-httpd.service

    address="http://127.0.0.1:${port}"
    while ! curl --head "${address}" &>/dev/null; do sleep 1; done
    echo "${address}" > ${test_tmpdir}/httpd-address
}

# Yeah this is a hack.  Doing this better requires more first class support
# for creating derived commits in ostree potentially.  Or barring that,
# just recommend to people to use `unshare -m` or equivalent and
# mount -o remount,rw /sysroot in their code.
require_writable_sysroot() {
    if ! test -w /sysroot; then
        mount -o remount,rw /sysroot
    fi
}

nth_boot() {
    journalctl --list-boots | wc -l
}

# Determine our origin refspec - we'll use this as a test base
rpmostree=$(which rpm-ostree 2>/dev/null)
if test -z "${rpmostree}"; then
    skip "no rpm-ostree, at some point point this to raw ostree too"
fi

# We need to be root
assert_streq $(id -u) 0

rpmostree_query_json() {
    query=$1
    rpm-ostree status --json | jq -r "${query}"
}
host_commit=$(rpmostree_query_json '.deployments[0].checksum')
host_osname=$(rpmostree_query_json '.deployments[0].osname')

# $1  - json file
# $2+ - assertions
assert_jq() {
    f=$1; shift
    for expression in "$@"; do
        if ! jq -e "${expression}" >/dev/null < $f; then
            jq . < $f | sed -e 's/^/# /' >&2
            echo 1>&2 "${expression} failed to match $f"
            exit 1
        fi
    done
}

# Assert that ostree admin status --json matches the provided jq predicates.
assert_status_jq() {
    local t=$(mktemp --suffix=.json)
    ostree admin status --json > $t
    assert_jq $t "$@"
    rm $t
}
