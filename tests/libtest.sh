# Source library for shell script tests
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

dn=$(dirname $0)

if [ -n "${G_TEST_SRCDIR:-}" ]; then
  test_srcdir="${G_TEST_SRCDIR}/tests"
else
  test_srcdir=$(dirname $0)
fi

top_builddir="${G_TEST_BUILDDIR:-}"
if test -z "${top_builddir}"; then
    top_builddir=$(cd $(dirname $0)/.. && pwd)
fi

test_builddir="${top_builddir}/tests"
. ${test_srcdir}/libtest-core.sh

# Make sure /sbin/capsh etc. are in our PATH even if non-root
PATH="$PATH:/usr/sbin:/sbin"

# Array of expressions to execute when exiting. Each expression should
# be a single string (quoting if necessary) that will be eval'd. To add
# a command to run on exit, append to the libtest_exit_cmds array like
# libtest_exit_cmds+=(expr).
libtest_exit_cmds=()
run_exit_cmds() {
  # Quiet termination
  set +x
  for expr in "${libtest_exit_cmds[@]}"; do
    eval "${expr}" || true
  done
}
trap run_exit_cmds EXIT

save_core() {
  if [ -e core ]; then
    cp core "$test_srcdir/core"
  fi
}
libtest_exit_cmds+=(save_core)

test_tmpdir=$(pwd)

# Sanity check that we're in a tmpdir that has
# just .testtmp (created by tap-driver for `make check`,
# or nothing at all (as ginstest-runner does)
if ! test -f .testtmp; then
    files=$(ls)
    if test -n "${files}"; then
	ls -l
	assert_not_reached "test tmpdir=${test_tmpdir} is not empty; run this test via \`make check TESTS=\`, not directly"
    fi
    # Remember that this is an acceptable test $(pwd), for the benefit of
    # C and JS tests which may source this file again
    touch .testtmp
fi

# Some distribution builds set this, but some of our build-time tests
# assume this won't be used when committing
unset SOURCE_DATE_EPOCH

# Also, unbreak `tar` inside `make check`...Automake will inject
# TAR_OPTIONS: --owner=0 --group=0 --numeric-owner presumably so that
# tarballs are predictable, except we don't want this in our tests.
unset TAR_OPTIONS

# Don't flag deployments as immutable so that test harnesses can
# easily clean up.
export OSTREE_SYSROOT_DEBUG=mutable-deployments

# By default, don't use a cache directory since it makes the tests racy.
# Tests that are explicitly testing the cache operation should unset
# this.
export OSTREE_SKIP_CACHE=1

export TEST_GPG_KEYID_1="7FCA23D8472CDAFA"
export TEST_GPG_KEYFPR_1="5E65DE75AB1C501862D476347FCA23D8472CDAFA"
export TEST_GPG_KEYID_2="D8228CFECA950D41"
export TEST_GPG_KEYFPR_2="7B3B1020D74479687FDB2273D8228CFECA950D41"
export TEST_GPG_KEYID_3="0D15FAE7DF444D67"
export TEST_GPG_KEYFPR_3="7D29CF060B8269CDF63BFBDD0D15FAE7DF444D67"

# GPG when creating signatures demands a private writable
# homedir in order to create lockfiles.  Work around
# this by copying locally.
echo "Copying gpghome to ${test_tmpdir}"
cp -a "${test_srcdir}/gpghome" ${test_tmpdir}
chmod -R u+w "${test_tmpdir}"
chmod 700 "${test_tmpdir}/gpghome"
export TEST_GPG_KEYHOME=${test_tmpdir}/gpghome
export OSTREE_GPG_HOME=${test_tmpdir}/gpghome/trusted

assert_has_setfattr() {
    if ! command -v setfattr 2>/dev/null; then
        fatal "no setfattr available to determine xattr support"
    fi
}

_have_selinux_relabel=''
have_selinux_relabel() {
    assert_has_setfattr
    if test "${_have_selinux_relabel}" = ''; then
        pushd ${test_tmpdir}
        echo testlabel > testlabel.txt
        selinux_xattr=security.selinux
        if getfattr --encoding=base64 -n ${selinux_xattr} testlabel.txt >label.txt 2>err.txt; then
            label=$(grep -E -e "^${selinux_xattr}=" < label.txt |sed -e "s,${selinux_xattr}=,,")
            if setfattr -n ${selinux_xattr} -v ${label} testlabel.txt 2>err.txt; then
                echo "SELinux enabled in $(pwd), and have privileges to relabel"
                _have_selinux_relabel=yes
            else
                sed -e 's/^/# /' < err.txt >&2
                echo "Found SELinux label, but unable to set (Unprivileged Docker?)"
                _have_selinux_relabel=no
            fi
        else
            sed -e 's/^/# /' < err.txt >&2
            echo "Unable to retrieve SELinux label, assuming disabled"
            _have_selinux_relabel=no
        fi
        popd
    fi
    test ${_have_selinux_relabel} = yes
}

# just globally turn off xattrs if we can't manipulate security xattrs; this is
# the case for overlayfs -- really, we should only enforce this for tests that
# use bare repos; separate from other tests that should check for user xattrs
# support
# see https://github.com/ostreedev/ostree/issues/758
# and https://github.com/ostreedev/ostree/pull/1217
echo -n checking for xattrs...
if ! have_selinux_relabel; then
    export OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},no-xattrs"
    export OSTREE_NO_XATTRS=1
fi
echo done

# whiteout char 0:0 devices can be created as regular users, but
# cannot be created inside containers mounted via overlayfs
can_create_whiteout_devices() {
    mknod -m 000 ${test_tmpdir}/.test-whiteout c 0 0 || return 1
    rm -f ${test_tmpdir}/.test-whiteout
    return 0
}

echo -n checking for overlayfs whiteouts...
if ! can_create_whiteout_devices; then
    export OSTREE_NO_WHITEOUTS=1
fi
echo done

if test -n "${OT_TESTS_DEBUG:-}"; then
    set -x
fi

# This is substituted by the build for installed tests
BUILT_WITH_ASAN=""
if test -n "${ASAN_OPTIONS:-}"; then
    BUILT_WITH_ASAN=1
fi

CMD_PREFIX=""
if test -n "${OT_TESTS_VALGRIND:-}"; then
    CMD_PREFIX="env G_SLICE=always-malloc valgrind -q --error-exitcode=1 --leak-check=full --num-callers=30 --suppressions=${test_srcdir}/glib.supp --suppressions=${test_srcdir}/ostree.supp"
fi

if test -z "${OSTREE_HTTPD:-}"; then
    OSTREE_HTTPD="${top_builddir}/ostree-trivial-httpd"
    if ! [ -x "${OSTREE_HTTPD}" ]; then
        OSTREE_HTTPD=
    fi
fi

files_are_hardlinked() {
    inode1=$(stat -c %i $1)
    inode2=$(stat -c %i $2)
    test -n "${inode1}" && test -n "${inode2}"
    [ "${inode1}" == "${inode2}" ]
}

assert_files_hardlinked() {
    if ! files_are_hardlinked "$1" "$2"; then
        fatal "Files '$1' and '$2' are not hardlinked"
    fi
}

setup_test_repository () {
    mode=$1
    shift

    oldpwd=`pwd`

    COMMIT_ARGS=""
    if [ $mode == "bare-user-only" ] ; then
       COMMIT_ARGS="--owner-uid=0 --owner-gid=0 --no-xattrs --canonical-permissions"
    fi

    cd ${test_tmpdir}
    rm -rf repo
    if test -n "${mode}"; then
        ostree_repo_init repo --mode=${mode}
    else
        ostree_repo_init repo
    fi
    ot_repo="--repo=$(pwd)/repo"
    export OSTREE="${CMD_PREFIX} ostree ${ot_repo}"

    cd ${test_tmpdir}
    local oldumask="$(umask)"
    umask 022
    rm -rf files
    mkdir files
    cd files
    ot_files=`pwd`
    export ht_files
    ln -s nosuchfile somelink
    echo first > firstfile

    cd ${test_tmpdir}/files
    $OSTREE commit ${COMMIT_ARGS}  -b test2 -s "Test Commit 1" -m "Commit body first"

    mkdir baz
    echo moo > baz/cow
    echo mooro > baz/cowro
    chmod 600 baz/cowro
    echo alien > baz/saucer
    mkdir baz/deeper
    echo hi > baz/deeper/ohyeah
    echo hix > baz/deeper/ohyeahx
    chmod 755 baz/deeper/ohyeahx
    ln -s nonexistent baz/alink
    mkdir baz/another/
    echo x > baz/another/y

    mkdir baz/sub1
    echo SAME_CONTENT > baz/sub1/duplicate_a
    echo SAME_CONTENT > baz/sub1/duplicate_b

    mkdir baz/sub2
    echo SAME_CONTENT > baz/sub2/duplicate_c

    # if we are running inside a container we cannot test
    # the overlayfs whiteout marker passthrough
    if ! test -n "${OSTREE_NO_WHITEOUTS:-}"; then
        mkdir whiteouts
        touch whiteouts/.ostree-wh.whiteout
        touch whiteouts/.ostree-wh.whiteout2
        chmod 755 whiteouts/.ostree-wh.whiteout2
    fi
    umask "${oldumask}"

    cd ${test_tmpdir}/files
    $OSTREE commit ${COMMIT_ARGS}  -b test2 -s "Test Commit 2" -m "Commit body second"
    $OSTREE fsck -q

    cd $oldpwd
}

# A wrapper which also possibly disables xattrs for CI testing
ostree_repo_init() {
    repo=$1
    shift
    ${CMD_PREFIX} ostree --repo=${repo} init "$@"
    if test -n "${OSTREE_NO_XATTRS:-}"; then
        echo -e 'disable-xattrs=true\n' >> ${repo}/config
    fi
}

run_webserver() {
    echo httpd=${OSTREE_HTTPD}
    if test -z "${OSTREE_HTTPD:-}"; then
        if test "$#" -gt 0; then
            echo "fatal: unhandled arguments for webserver: $@" 1>&2
            exit 1
        fi
        # Note this automatically daemonizes; close stdin to ensure it doesn't leak to the child.
        ${test_srcdir}/webserver.py ${test_tmpdir}/httpd-port </dev/null &>/dev/null
        echo "Waiting for webserver..."
        while test '!' -f ${test_tmpdir}/httpd-port; do
            sleep 0.5
        done
    else
        ${OSTREE_HTTPD} --autoexit --log-file $(pwd)/httpd.log --daemonize -p ${test_tmpdir}/httpd-port "$@"
    fi
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
}

# The original one; use setup_fake_remote_repo2 for newer code,
# down the line we'll try to port tests.
setup_fake_remote_repo1() {
    mode=$1; shift
    commit_opts=${1:-}
    [ $# -eq 0 ] || shift
    oldpwd=`pwd`
    mkdir ostree-srv
    cd ostree-srv
    mkdir gnomerepo
    ostree_repo_init gnomerepo --mode=$mode
    mkdir gnomerepo-files
    cd gnomerepo-files 
    echo first > firstfile
    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit $commit_opts --add-metadata-string version=3.0 -b main -s "A remote commit" -m "Some Commit body"
    mkdir baz/deeper
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit $commit_opts --add-metadata-string version=3.1 -b main -s "Add deeper"
    echo hi > baz/deeper/ohyeah
    mkdir baz/another/
    echo x > baz/another/y
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit $commit_opts --add-metadata-string version=3.2 -b main -s "The rest"
    cd ..
    rm -rf gnomerepo-files
    
    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir}/ostree-srv ostree
    run_webserver "$@"
    cd ${oldpwd} 

    export OSTREE="${CMD_PREFIX} ostree --repo=repo"
}

# Newer version of the above with more "real" data
setup_fake_remote_repo2() {
    mode=$1
    commit_opts=${2:-}
    args=${3:-}
    shift
    oldpwd=`pwd`
    mkdir ostree-srv
    cd ostree-srv
    mkdir repo
    ostree_repo_init repo --mode=$mode
    # Backcompat
    ln -sr repo gnomerepo
    # Initialize content
    mkdir files
    cd files
    mkdir -p usr/{etc,bin,lib,share}
    ln -sr usr/bin bin
    ln -sr usr/lib lib
    tar xf ${test_srcdir}/fah-deltadata-old.tar.xz
    remote_ref=exampleos/42/x86_64/main
    cd ..
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/repo commit \
                  --consume $commit_opts --add-metadata-string version=42.0 -b ${remote_ref} \
                  --tree=dir=files
    test '!' -d files
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/repo checkout -U ${remote_ref} files
    (cd files && tar xf ${test_srcdir}/fah-deltadata-new.tar.xz)
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/repo commit \
                  --consume $commit_opts --add-metadata-string version=42.1 -b ${remote_ref} \
                  --tree=dir=files

    # And serve via HTTP
    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir}/ostree-srv ostree
    run_webserver $args
    cd ${oldpwd}
    export OSTREE="${CMD_PREFIX} ostree --repo=repo"
}

setup_os_boot_syslinux() {
    # Stub syslinux configuration
    mkdir -p sysroot/boot/loader.0
    ln -s loader.0 sysroot/boot/loader
    touch sysroot/boot/loader/syslinux.cfg
    # And a compatibility symlink
    mkdir -p sysroot/boot/syslinux
    ln -s ../loader/syslinux.cfg sysroot/boot/syslinux/syslinux.cfg
}

setup_os_boot_uboot() {
    # Stub U-Boot configuration
    mkdir -p sysroot/boot/loader.0
    ln -s loader.0 sysroot/boot/loader
    touch sysroot/boot/loader/uEnv.txt
    # And a compatibility symlink
    ln -s loader/uEnv.txt sysroot/boot/uEnv.txt
}

setup_os_boot_grub2() {
    grub2_options=$1
    mkdir -p sysroot/boot/grub2/
    ln -s ../loader/grub.cfg sysroot/boot/grub2/grub.cfg
    export OSTREE_BOOT_PARTITION="/boot"
    case "$grub2_options" in
        *ostree-grub-generator*)
            cp ${test_srcdir}/ostree-grub-generator ${test_tmpdir}
            chmod +x ${test_tmpdir}/ostree-grub-generator
            export OSTREE_GRUB2_EXEC=${test_tmpdir}/ostree-grub-generator
            ;;
    esac
}

setup_os_boot_configured_bootloader() {
    bootloader_keyval=$1
    ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set ${bootloader_keyval}
}

setup_os_repository () {
    mode=$1
    shift
    bootmode=$1
    shift
    bootdir=${1:-usr/lib/modules/3.6.0}

    oldpwd=`pwd`

    cd ${test_tmpdir}
    mkdir testos-repo
    if test -n "$mode"; then
	      ostree_repo_init testos-repo --mode=${mode}
    else
	      ostree_repo_init testos-repo
    fi

    cd ${test_tmpdir}
    mkdir osdata
    cd osdata
    kver=3.6.0
    mkdir -p usr/bin ${bootdir} usr/lib/modules/${kver} usr/share usr/etc usr/container/layers/abcd
    kernel_path=${bootdir}/vmlinuz
    initramfs_path=${bootdir}/initramfs.img
    # the HMAC file is only in /usr/lib/modules
    hmac_path=usr/lib/modules/${kver}/.vmlinuz.hmac
    # /usr/lib/modules just uses "vmlinuz", since the version is in the module
    # directory name.
    if [[ $bootdir != usr/lib/modules/* ]]; then
        kernel_path=${kernel_path}-${kver}
        initramfs_path=${bootdir}/initramfs-${kver}.img
    fi
    echo "a kernel" > ${kernel_path}
    echo "an initramfs" > ${initramfs_path}
    echo "an hmac file" > ${hmac_path}
    bootcsum=$(cat ${kernel_path} ${initramfs_path} | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    bootable_flag=""
    # Add the checksum for legacy dirs (/boot, /usr/lib/ostree-boot), but not
    # /usr/lib/modules.
    if [[ $bootdir != usr/lib/modules/* ]]; then
        mv ${kernel_path}{,-${bootcsum}}
        mv ${initramfs_path}{,-${bootcsum}}
    else
        bootable_flag="--bootable"
    fi

    echo "an executable" > usr/bin/sh
    echo "some shared data" > usr/share/langs.txt
    echo "a library" > usr/lib/libfoo.so.0
    ln -s usr/bin bin
cat > usr/etc/os-release <<EOF
NAME=TestOS
VERSION=42
ID=testos
VERSION_ID=42
PRETTY_NAME="TestOS 42"
EOF
    echo "a config file" > usr/etc/aconfigfile
    mkdir -p usr/etc/NetworkManager
    echo "a default daemon file" > usr/etc/NetworkManager/nm.conf
    mkdir -p usr/etc/testdirectory
    echo "a default daemon file" > usr/etc/testdirectory/test

    # if we are running inside a container we cannot test
    # the overlayfs whiteout marker passthrough
    if ! test -n "${OSTREE_NO_WHITEOUTS:-}"; then
        # overlayfs whiteout passhthrough marker files
        touch usr/container/layers/abcd/.ostree-wh.whiteout
        chmod 400 usr/container/layers/abcd/.ostree-wh.whiteout

        touch usr/container/layers/abcd/.ostree-wh.whiteout2
        chmod 777 usr/container/layers/abcd/.ostree-wh.whiteout2
    fi

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit ${bootable_flag} --add-metadata-string version=1.0.9 -b testos/buildmain/x86_64-runtime -s "Build"

    # Ensure these commits have distinct second timestamps
    sleep 2
    echo "a new executable" > usr/bin/sh
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit ${bootable_flag} --add-metadata-string version=1.0.10 -b testos/buildmain/x86_64-runtime -s "Build"

    cd ${test_tmpdir}
    rm -rf osdata-devel
    mkdir osdata-devel
    tar -C osdata -cf - . | tar -C osdata-devel -xf -
    cd osdata-devel
    mkdir -p usr/include
    echo "a development header" > usr/include/foo.h
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit ${bootable_flag} --add-metadata-string version=1.0.9 -b testos/buildmain/x86_64-devel -s "Build"

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo fsck -q

    cd ${test_tmpdir}
    mkdir sysroot
    export OSTREE_SYSROOT=sysroot
    ${CMD_PREFIX} ostree admin init-fs sysroot
    if test -n "${OSTREE_NO_XATTRS:-}"; then
        echo -e 'disable-xattrs=true\n' >> sysroot/ostree/repo/config
    fi
    ${CMD_PREFIX} ostree admin stateroot-init testos

    case $bootmode in
        "syslinux")
	    setup_os_boot_syslinux
            ;;
        "uboot")
	    setup_os_boot_uboot
            ;;
        *grub2*)
        setup_os_boot_grub2 "${bootmode}"
            ;;
        sysroot\.bootloader*)
        setup_os_boot_configured_bootloader "${bootmode}"
            ;;
    esac

    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir} ostree
    run_webserver
    cd ${oldpwd} 
}

timestamp_of_commit()
{
  date --date="$(ostree --repo=$1 show $2 | grep -Ee '^Date: ' | sed -e 's,^Date: *,,')" '+%s'
}

os_repository_new_commit ()
{
    boot_checksum_iteration=${1:-0}
    content_iteration=${2:-0}
    branch=${3:-testos/buildmain/x86_64-runtime}
    export version=${4:-$(date "+%Y%m%d.${content_iteration}")}
    echo "BOOT ITERATION: $boot_checksum_iteration"
    cd ${test_tmpdir}/osdata
    kver=3.6.0
    if test -f usr/lib/modules/${kver}/vmlinuz; then
        bootdir=usr/lib/modules/${kver}
    else
        if test -d usr/lib/ostree-boot; then
            bootdir=usr/lib/ostree-boot
        else
            bootdir=boot
        fi
    fi
    rm ${bootdir}/*
    kernel_path=${bootdir}/vmlinuz
    initramfs_path=${bootdir}/initramfs.img
    if [[ $bootdir != usr/lib/modules/* ]]; then
        kernel_path=${kernel_path}-${kver}
        initramfs_path=${bootdir}/initramfs-${kver}.img
    fi
    echo "new: a kernel ${boot_checksum_iteration}" > ${kernel_path}
    echo "new: an initramfs ${boot_checksum_iteration}" > ${initramfs_path}
    bootcsum=$(cat ${kernel_path} ${initramfs_path} | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    if [[ $bootdir != usr/lib/modules/* ]]; then
        mv ${kernel_path}{,-${bootcsum}}
        mv ${initramfs_path}{,-${bootcsum}}
    fi

    echo "a new default config file" > usr/etc/a-new-default-config-file
    mkdir -p usr/etc/new-default-dir
    echo "a new default dir and file" > usr/etc/new-default-dir/moo

    echo "content iteration ${content_iteration}" > usr/bin/content-iteration

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit  --add-metadata-string "version=${version}" -b $branch -s "Build"
    if ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo rev-parse ${branch} 2>/dev/null; then
        prevdate=$(timestamp_of_commit ${test_tmpdir}/testos-repo "${branch}"^)
        newdate=$(timestamp_of_commit ${test_tmpdir}/testos-repo "${branch}")
        if [ $((${prevdate} > ${newdate})) = 1 ]; then
            fatal "clock skew detected writing commits: prev=${prevdate} new=${newdate}"
        fi
    fi
    cd ${test_tmpdir}
}

_have_user_xattrs=''
have_user_xattrs() {
    assert_has_setfattr
    if test "${_have_user_xattrs}" = ''; then
        touch test-xattrs
        if setfattr -n user.testvalue -v somevalue test-xattrs 2>/dev/null; then
            _have_user_xattrs=yes
        else
            _have_user_xattrs=no
        fi
        rm -f test-xattrs
    fi
    test ${_have_user_xattrs} = yes
}

# Usage: if ! skip_one_without_user_xattrs; then ... more tests ...; fi
skip_one_without_user_xattrs () {
    if ! have_user_xattrs; then
        echo "ok # SKIP - this test requires xattr support"
        return 0
    else
        return 1
    fi
}

skip_without_ostree_httpd () {
    if test -z "${OSTREE_HTTPD:-}"; then
        skip "this test requires libsoup (ostree-trivial-httpd)"
    fi
}

skip_known_xfail_docker() {
    if test "${OSTREE_TEST_SKIP:-}" = known-xfail-docker; then
        skip "This test was explicitly skipped via OSTREE_TEST_SKIP=known-xfail-docker"
    fi
}

skip_without_user_xattrs () {
    if ! have_user_xattrs; then
        skip "this test requires xattr support"
    fi
}

skip_without_sudo () {
    if test -z "${OSTREE_TEST_SUDO:-}"; then
        skip "this test needs sudo, skipping without OSTREE_TEST_SUDO being set"
    fi
}

# Usage: if ! skip_one_without_whiteouts_devices; then ... more tests ...; fi
skip_one_without_whiteouts_devices() {
    if ! can_create_whiteout_devices; then
        echo "ok # SKIP - this test requires whiteout device support (test outside containers)"
        return 0
    else
        return 1
    fi
}

skip_without_whiteouts_devices () {
    if ! can_create_whiteout_devices; then
        skip "this test requires whiteout device support (test outside containers)"
    fi
}

_have_systemd_and_libmount=''
have_systemd_and_libmount() {
    if test "${_have_systemd_and_libmount}" = ''; then
        if [ $(ostree --version | grep -c -e '- systemd' -e '- libmount') -eq 2 ]; then
            _have_systemd_and_libmount=yes
        else
            _have_systemd_and_libmount=no
        fi
    fi
    test ${_have_systemd_and_libmount} = yes
}

# Skip unless SELinux is disabled, or we can relabel.
# Default Docker has security.selinux xattrs, but returns
# EOPNOTSUPP when trying to set them, even to the existing value.
# https://github.com/ostreedev/ostree/pull/759
# https://github.com/ostreedev/ostree/pull/1217
skip_without_no_selinux_or_relabel () {
    if ! have_selinux_relabel; then
        skip "this test requires SELinux relabeling support"
    fi
}

# https://brokenpi.pe/tools/strace-fault-injection
_have_strace_fault_injection=''
have_strace_fault_injection() {
    if test "${_have_strace_fault_injection}" = ''; then
        if strace -P ${test_srcdir}/libtest-core.sh -e inject=read:retval=0 cat ${test_srcdir}/libtest-core.sh >out.txt &&
           test '!' -s out.txt; then
            _have_strace_fault_injection=yes
        else
            _have_strace_fault_injection=no
        fi
        rm -f out.txt
    fi
    test ${_have_strace_fault_injection} = yes
}

skip_one_without_strace_fault_injection() {
    if ! have_strace_fault_injection; then
        echo "ok # SKIP this test requires strace fault injection"
        return 0
    fi
    return 1
}

skip_without_fuse () {
    fusermount --version >/dev/null 2>&1 || skip "no fusermount"

    capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin' || \
        skip "No cap_sys_admin in bounding set, can't use FUSE"

    [ -w /dev/fuse ] || skip "no write access to /dev/fuse"
    [ -e /etc/mtab ] || skip "no /etc/mtab"
}

has_ostree_feature () {
    local ret=0
    # Note that this needs to write to a file and then grep the file, to
    # avoid ostree --version being killed with SIGPIPE and exiting with a
    # nonzero status under `set -o pipefail`.
    ${CMD_PREFIX} ostree --version > version.txt
    grep -q -e "- $1\$" version.txt || ret=$?
    rm -f version.txt
    return ${ret}
}

skip_without_ostree_feature () {
    if ! has_ostree_feature "$1"; then
        skip "no $1 support compiled in"
    fi
}

# Find an appropriate gpg program to use. We want one that has the
# --generate-key, --quick-set-expire and --quick-add-key options. The
# gpg program to use is returend.
which_gpg () {
    local gpg
    local gpg_options
    local needed_options=(
        --generate-key
        --quick-set-expire
        --quick-add-key
    )
    local opt

    # Prefer gpg2 in case gpg refers to gpg1
    if command -v gpg2 &>/dev/null; then
        gpg=gpg2
    elif command -v gpg &>/dev/null; then
        gpg=gpg
    else
        # Succeed but don't return anything.
        return 0
    fi

    # Make sure all the needed options are available
    gpg_options=$(${gpg} --dump-options) || return 0
    for opt in ${needed_options[*]}; do
      grep -q -x -e "${opt}" <<< "${gpg_options}" || return 0
    done

    # Found an appropriate gpg
    echo ${gpg}
}

libtest_cleanup_gpg () {
    set +x
    local gpg_homedir=${1:-${test_tmpdir}/gpghome}
    gpg-connect-agent --homedir "${gpg_homedir}" killagent /bye &>/dev/null || true
}
libtest_exit_cmds+=(libtest_cleanup_gpg)

# Keys for ed25519 signing tests
ED25519PUBLIC=
ED25519SEED=
ED25519SECRET=

gen_ed25519_keys ()
{
  # Generate private key in PEM format
  pemfile="$(mktemp -p ${test_tmpdir} ed25519_XXXXXX.pem)"
  openssl genpkey -algorithm ed25519 -outform PEM -out "${pemfile}"

  # Based on: http://openssl.6102.n7.nabble.com/ed25519-key-generation-td73907.html
  # Extract the private and public parts from generated key.
  ED25519PUBLIC="$(openssl pkey -outform DER -pubout -in ${pemfile} | tail -c 32 | base64)"
  ED25519SEED="$(openssl pkey -outform DER -in ${pemfile} | tail -c 32 | base64)"
  # Secret key is concantination of SEED and PUBLIC
  ED25519SECRET="$(echo ${ED25519SEED}${ED25519PUBLIC} | base64 -d | base64 -w 0)"

  echo "Generated ed25519 keys:"
  echo "public: ${ED25519PUBLIC}"
  echo "  seed: ${ED25519SEED}"
}

gen_ed25519_random_public()
{
  openssl genpkey -algorithm ED25519 | openssl pkey -outform DER | tail -c 32 | base64
}

# Keys for spki signing tests
SPKIPUBLICPEM=
SPKISECRETPEM=

SPKIPUBLIC=
SPKISECRET=

gen_spki_keys ()
{
  # Generate private key in PEM format
  SPKISECRETPEM="$(mktemp -p ${test_tmpdir} ed448_sk_XXXXXX.pem)"
  openssl genpkey -algorithm ed448 -outform PEM -out "${SPKISECRETPEM}"
  SPKIPUBLICPEM="$(mktemp -p ${test_tmpdir} ed448_pk_XXXXXX.pem)"
  openssl pkey -outform PEM -pubout -in "${SPKISECRETPEM}" -out "${SPKIPUBLICPEM}"

  SPKIPUBLIC="$(openssl pkey -inform PEM -outform DER -pubin -pubout -in ${SPKIPUBLICPEM} | base64 -w 0)"
  SPKISECRET="$(openssl pkey -inform PEM -outform DER -in ${SPKISECRETPEM} | base64 -w 0)"

  echo "Generated ed448 keys:"
  echo "public: ${SPKIPUBLIC}"
  echo "secret: ${SPKISECRET}"
}

gen_spki_random_public()
{
    openssl genpkey -algorithm ed448 | openssl pkey -pubout -outform DER | base64 -w 0
    echo
}

gen_spki_random_public_pem()
{
  openssl genpkey -algorithm ed448 | openssl pkey -pubout -outform PEM
}

is_bare_user_only_repo () {
  grep -q 'mode=bare-user-only' $1/config
}

# Given a path to a file in a repo for a ref, print its checksum
ostree_file_path_to_checksum() {
    repo=$1
    ref=$2
    path=$3
    $CMD_PREFIX ostree --repo=$repo ls -C $ref $path | awk '{ print $5 }'
}

# Given an object checksum, print its relative file path
ostree_checksum_to_relative_object_path() {
    repo=$1
    checksum=$2
    if grep -Eq -e '^mode=archive' ${repo}/config; then suffix=z; else suffix=''; fi
    echo objects/${checksum:0:2}/${checksum:2}.file${suffix}
}

# Given a path to a file in a repo for a ref, print the (relative) path to its
# object
ostree_file_path_to_relative_object_path() {
    repo=$1
    ref=$2
    path=$3
    checksum=$(ostree_file_path_to_checksum $repo $ref $path)
    test -n "${checksum}"
    ostree_checksum_to_relative_object_path ${repo} ${checksum}
}

# Given a path to a file in a repo for a ref, print the path to its object
ostree_file_path_to_object_path() {
    repo=$1
    ref=$2
    path=$3
    relpath=$(ostree_file_path_to_relative_object_path $repo $ref $path)
    echo ${repo}/${relpath}
}

# Assert ref $2 in repo $1 has checksum $3.
assert_ref () {
    assert_streq $(${CMD_PREFIX} ostree rev-parse --repo=$1 $2) $3
}

# Assert no ref named $2 is present in repo $1.
assert_not_ref () {
    if ${CMD_PREFIX} ostree rev-parse --repo=$1 $2 2>/dev/null; then
        fatal "rev-parse $2 unexpectedly succeeded!"
    fi
}

assert_fail () {
  set +e
  $@
  if [ $? = 0 ] ; then
    echo 1>&2 "$@ did not fail"; exit 1
  fi
  set -euo pipefail
}
