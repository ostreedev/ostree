# Source library for shell script tests
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

if [ -n "${G_TEST_SRCDIR:-}" ]; then
  test_srcdir="${G_TEST_SRCDIR}/tests"
else
  test_srcdir=$(dirname $0)
fi

if [ -n "${G_TEST_BUILDDIR:-}" ]; then
  test_builddir="${G_TEST_BUILDDIR}/tests"
else
  test_builddir=$(dirname $0)
fi
. ${test_srcdir}/libtest-core.sh

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

# Also, unbreak `tar` inside `make check`...Automake will inject
# TAR_OPTIONS: --owner=0 --group=0 --numeric-owner presumably so that
# tarballs are predictable, except we don't want this in our tests.
unset TAR_OPTIONS

# Don't flag deployments as immutable so that test harnesses can
# easily clean up.
export OSTREE_SYSROOT_DEBUG=mutable-deployments

export TEST_GPG_KEYID_1="472CDAFA"
export TEST_GPG_KEYID_2="CA950D41"
export TEST_GPG_KEYID_3="DF444D67"

# GPG when creating signatures demands a writable
# homedir in order to create lockfiles.  Work around
# this by copying locally.
echo "Copying gpghome to ${test_tmpdir}"
cp -a "${test_srcdir}/gpghome" ${test_tmpdir}
chmod -R u+w "${test_tmpdir}"
export TEST_GPG_KEYHOME=${test_tmpdir}/gpghome
export OSTREE_GPG_HOME=${test_tmpdir}/gpghome/trusted

# See comment in ot-builtin-commit.c and https://github.com/ostreedev/ostree/issues/758
# Also keep this in sync with the bits in libostreetest.c
echo evaluating for overlayfs...
case $(stat -f --printf '%T' /) in
    overlayfs)
        echo "overlayfs found; enabling OSTREE_NO_XATTRS"
        export OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},no-xattrs"
        export OSTREE_NO_XATTRS=1 ;;
    *) ;;
esac
echo done

if test -n "${OT_TESTS_DEBUG:-}"; then
    set -x
fi

# This is substituted by the build for installed tests
BUILT_WITH_ASAN=""

if test -n "${OT_TESTS_VALGRIND:-}"; then
    CMD_PREFIX="env G_SLICE=always-malloc OSTREE_SUPPRESS_SYNCFS=1 valgrind -q --error-exitcode=1 --leak-check=full --num-callers=30 --suppressions=${test_srcdir}/glib.supp --suppressions=${test_srcdir}/ostree.supp"
else
    # In some cases the LD_PRELOAD may cause obscure problems,
    # e.g. right now it breaks for me with -fsanitize=address, so
    # let's allow users to skip it.
    if test -z "${OT_SKIP_READDIR_RAND:-}" && test -z "${BUILT_WITH_ASAN:-}"; then
	CMD_PREFIX="env LD_PRELOAD=${test_builddir}/libreaddir-rand.so"
    else
	CMD_PREFIX=""
    fi
fi

if test -n "${OSTREE_UNINSTALLED:-}"; then
    OSTREE_HTTPD=${OSTREE_UNINSTALLED}/ostree-trivial-httpd
else
    # trivial-httpd is now in $libexecdir by default, which we don't
    # know at this point. Fortunately, libtest.sh is also in
    # $libexecdir, so make an educated guess. If it's not found, assume
    # it's still runnable as "ostree trivial-httpd".
    if [ -x "${test_srcdir}/../../libostree/ostree-trivial-httpd" ]; then
        OSTREE_HTTPD="${CMD_PREFIX} ${test_srcdir}/../../libostree/ostree-trivial-httpd"
    else
        OSTREE_HTTPD="${CMD_PREFIX} ostree trivial-httpd"
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
    if test -n "${mode}"; then
        ostree_repo_init repo --mode=${mode}
    else
        ostree_repo_init repo
    fi
    ot_repo="--repo=$(pwd)/repo"
    export OSTREE="${CMD_PREFIX} ostree ${ot_repo}"

    cd ${test_tmpdir}
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

setup_fake_remote_repo1() {
    mode=$1
    commit_opts=${2:-}
    args=${3:-}
    shift
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
    ${OSTREE_HTTPD} --autoexit --log-file $(pwd)/httpd.log --daemonize -p ${test_tmpdir}/httpd-port $args
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
    cd ${oldpwd} 

    export OSTREE="${CMD_PREFIX} ostree --repo=repo"
}

# Set up a large repository for stress testing.
# Something like the Fedora Atomic Workstation branch which has
# objects: meta: 7497 content: 103541
# 9443 directories, 7097 symlinks, 112832 regfiles
# So we'll make ~11 files per dir, with one of them a symlink
# Actually, let's cut this down to 1/3 which is still useful.  So:
# 3147 dirs, with still ~11 files per dir, for 37610 content objects
setup_exampleos_repo() {
    args=${1:-}
    cd ${test_tmpdir}
    mkdir ostree-srv
    mkdir -p ostree-srv/exampleos/{repo,build-repo}
    export ORIGIN_REPO=ostree-srv/exampleos/repo
    export ORIGIN_BUILD_REPO=ostree-srv/exampleos/build-repo
    ostree_repo_init ${ORIGIN_REPO} --mode=archive
    ostree_repo_init ${ORIGIN_BUILD_REPO} --mode=bare-user
    cd ${test_tmpdir}
    rm main -rf
    mkdir main
    cd main
    ndirs=3147
    depth=0
    set +x  # No need to spam the logs for this
    echo "$(date): Generating initial content..."
    while [ $ndirs -gt 0 ]; do
        # 2/3 of the time, recurse a dir, up to a max of 9, otherwise back up
        x=$(($ndirs % 3))
        case $x in
            0) if [ $depth -gt 0 ]; then cd ..; depth=$((depth-1)); fi ;;
            1|2) if [ $depth -lt 9 ]; then
                         mkdir dir-${ndirs}
                         cd dir-${ndirs}
                         depth=$((depth+1))
                     else
                         if [ $depth -gt 0 ]; then cd ..; depth=$((depth-1)); fi
                 fi ;;
        esac
        # One symlink - we use somewhat predictable content to have dupes
        ln -s $(($x % 20)) link-$ndirs
        # 10 files
        nfiles=10
        while [ $nfiles -gt 0 ]; do
            echo file-$ndirs-$nfiles > f$ndirs-$nfiles
            # Make an unreadable file to trigger https://github.com/ostreedev/ostree/pull/634
            if [ $(($x % 10)) -eq 0 ]; then
                chmod 0600 f$ndirs-$nfiles
            fi
            nfiles=$((nfiles-1))
        done
        ndirs=$((ndirs-1))
    done
    cd ${test_tmpdir}
    set -x

    export REF=exampleos/42/standard

    ${CMD_PREFIX} ostree --repo=${ORIGIN_BUILD_REPO} commit -b ${REF} --tree=dir=main
    rm main -rf
    ${CMD_PREFIX} ostree --repo=${ORIGIN_BUILD_REPO} checkout ${REF} main

    find main > files.txt
    nfiles=$(wc -l files.txt | cut -f 1 -d ' ')
    # We'll make 5 more commits
    for iter in $(seq 5); do
        set +x
        # Change 10% of files
        for fiter in $(seq $(($nfiles / 10))); do
            filenum=$(($RANDOM % ${nfiles}))
            set +o pipefail
            filename=$(tail -n +${filenum} < files.txt | head -1)
            set -o pipefail
            if test -f $filename; then
                rm -f $filename
                echo file-${iter}-${fiter} > ${filename}
            fi
        done
        set -x
        ${CMD_PREFIX} ostree --repo=${ORIGIN_BUILD_REPO} commit --link-checkout-speedup -b ${REF} --tree=dir=main
    done

    ${CMD_PREFIX} ostree --repo=${ORIGIN_REPO} pull-local --depth=-1 ${ORIGIN_BUILD_REPO}

    for x in "^^" "^" ""; do
        ${CMD_PREFIX} ostree --repo=${ORIGIN_REPO} static-delta generate --from="${REF}${x}^" --to="${REF}${x}"
    done
    ${CMD_PREFIX} ostree --repo=${ORIGIN_REPO} summary -u

    cd ${test_tmpdir}/ostree-srv
    mkdir httpd
    ${OSTREE_HTTPD} --autoexit --log-file $(pwd)/httpd/httpd.log --daemonize -p httpd/port $args
    port=$(cat httpd/port)
    echo "http://127.0.0.1:${port}" > httpd/address

    cd ${test_tmpdir}
    rm repo -rf
    ostree_repo_init repo --mode=bare-user
    ${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat ostree-srv/httpd/address)/exampleos/repo
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

setup_os_repository () {
    mode=$1
    shift
    bootmode=$1
    shift
    bootdir=${1:-usr/lib/ostree-boot}

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
    mkdir -p usr/bin usr/lib/modules/3.6.0 usr/share usr/etc
    mkdir -p ${bootdir}
    echo "a kernel" > ${bootdir}/vmlinuz-3.6.0
    echo "an initramfs" > ${bootdir}/initramfs-3.6.0
    bootcsum=$(cat ${bootdir}/vmlinuz-3.6.0 ${bootdir}/initramfs-3.6.0 | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    # Add the checksum for legacy dirs (/boot, /usr/lib/ostree-boot), but not
    # /usr/lib/modules.
    if [[ $bootdir != usr/lib/modules ]]; then
        mv ${bootdir}/vmlinuz-3.6.0{,-${bootcsum}}
        mv ${bootdir}/initramfs-3.6.0{,-${bootcsum}}
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

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmaster/x86_64-runtime -s "Build"
    
    # Ensure these commits have distinct second timestamps
    sleep 2
    echo "a new executable" > usr/bin/sh
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.10 -b testos/buildmaster/x86_64-runtime -s "Build"

    cd ${test_tmpdir}
    cp -a osdata osdata-devel
    cd osdata-devel
    mkdir -p usr/include
    echo "a development header" > usr/include/foo.h
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmaster/x86_64-devel -s "Build"

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo fsck -q

    cd ${test_tmpdir}
    mkdir sysroot
    export OSTREE_SYSROOT=sysroot
    ${CMD_PREFIX} ostree admin init-fs sysroot
    if test -n "${OSTREE_NO_XATTRS:-}"; then
        echo -e 'disable-xattrs=true\n' >> sysroot/ostree/repo/config
    fi
    ${CMD_PREFIX} ostree admin os-init testos

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
    esac
    
    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir} ostree
    ${OSTREE_HTTPD} --autoexit --daemonize -p ${test_tmpdir}/httpd-port
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
    cd ${oldpwd} 
}

os_repository_new_commit ()
{
    boot_checksum_iteration=${1:-0}
    content_iteration=${2:-0}
    branch=${3:-testos/buildmaster/x86_64-runtime}
    echo "BOOT ITERATION: $boot_checksum_iteration"
    cd ${test_tmpdir}/osdata
    bootdir=usr/lib/ostree-boot
    if ! test -d ${bootdir}; then
        bootdir=boot
    fi
    rm ${bootdir}/*
    echo "new: a kernel ${boot_checksum_iteration}" > ${bootdir}/vmlinuz-3.6.0
    echo "new: an initramfs ${boot_checksum_iteration}" > ${bootdir}/initramfs-3.6.0
    bootcsum=$(cat ${bootdir}/vmlinuz-3.6.0 ${bootdir}/initramfs-3.6.0 | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    mv ${bootdir}/vmlinuz-3.6.0 ${bootdir}/vmlinuz-3.6.0-${bootcsum}
    mv ${bootdir}/initramfs-3.6.0 ${bootdir}/initramfs-3.6.0-${bootcsum}

    echo "a new default config file" > usr/etc/a-new-default-config-file
    mkdir -p usr/etc/new-default-dir
    echo "a new default dir and file" > usr/etc/new-default-dir/moo

    echo "content iteration ${content_iteration}" > usr/bin/content-iteration

    version=$(date "+%Y%m%d.${content_iteration}")

    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit  --add-metadata-string "version=${version}" -b $branch -s "Build"
    cd ${test_tmpdir}
}

skip_without_user_xattrs () {
    touch test-xattrs
    setfattr -n user.testvalue -v somevalue test-xattrs || \
        skip "this test requires xattr support"
}

skip_without_fuse () {
    fusermount --version >/dev/null 2>&1 || skip "no fusermount"

    capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin' || \
        skip "No cap_sys_admin in bounding set, can't use FUSE"

    [ -w /dev/fuse ] || skip "no write access to /dev/fuse"
    [ -e /etc/mtab ] || skip "no /etc/mtab"
}

has_gpgme () {
    ${CMD_PREFIX} ostree --version > version.txt
    assert_file_has_content version.txt '- gpgme'
    rm -f version.txt
    true
}

libtest_cleanup_gpg () {
    gpg-connect-agent --homedir ${test_tmpdir}/gpghome killagent /bye || true
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

# Given a path to a file in a repo for a ref, print the (relative) path to its
# object
ostree_file_path_to_relative_object_path() {
    repo=$1
    ref=$2
    path=$3
    checksum=$(ostree_file_path_to_checksum $repo $ref $path)
    test -n "${checksum}"
    echo objects/${checksum:0:2}/${checksum:2}.file
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
