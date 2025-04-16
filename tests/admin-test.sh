# This file is to be sourced, not executed

# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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

echo "1..$((31 + ${extra_admin_tests:-0}))"

for flag in --modern --epoch=1; do
    mkdir sysrootmin
    ${CMD_PREFIX} ostree admin init-fs --modern sysrootmin
    assert_has_dir sysrootmin/boot
    assert_has_dir sysrootmin/ostree/repo
    assert_not_has_dir sysrootmin/home
    rm sysrootmin -rf
done
mkdir sysrootmin
${CMD_PREFIX} ostree admin init-fs --epoch=2 sysrootmin
assert_streq "$(stat -c '%f' sysrootmin/ostree)" 41c0
assert_not_has_dir sysrootmin/home
echo "ok init-fs"

function validate_bootloader() {
    cd ${test_tmpdir};
    bootloader=""
    if test -f sysroot/boot/syslinux/syslinux.cfg; then
	    bootloader="syslinux"
    elif test -f sysroot/boot/grub2/grub.cfg; then
	    bootloader="grub2"
    fi
    if test -n "${bootloader}"; then
        $(dirname $0)/bootloader-entries-crosscheck.py sysroot ${bootloader}
    fi
    cd -
}

# Test generate_deployment_refs()
assert_ostree_deployment_refs() {
    ${CMD_PREFIX} ostree --repo=sysroot/ostree/repo refs ostree | sort > ostree-refs.txt
    (for v in "$@"; do echo $v; done) | sort > ostree-refs-expected.txt
    diff -u ostree-refs{-expected,}.txt
}

orig_mtime=$(stat -c '%.Y' sysroot/ostree/deploy)
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmain/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments.  We also set the initial
# timestamp of the deploy directory to the epoch as a regression test.
touch -d @0 sysroot/ostree/deploy

if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    # Have loader as a directory (simulate ESP-based deployments)
    if [ -h sysroot/boot/loader ]; then
        loader=`readlink sysroot/boot/loader`
        rm -f sysroot/boot/loader
        mv sysroot/boot/${loader} sysroot/boot/loader
        echo -n ${loader} > sysroot/boot/loader/ostree_bootversion
    else
        mkdir -p sysroot/boot/loader
        echo -n "loader.0" > sysroot/boot/loader/ostree_bootversion
    fi
fi

${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmain/x86_64-runtime
new_mtime=$(stat -c '%.Y' sysroot/ostree/deploy)
assert_not_streq "${orig_mtime}" "${new_mtime}"
${CMD_PREFIX} ostree admin status | tee status.txt
assert_not_file_has_content status.txt "pending"
assert_not_file_has_content status.txt "rollback"
validate_bootloader

if has_ostree_feature composefs; then
    if ! test -f sysroot/ostree/deploy/testos/deploy/*.0/.ostree.cfs; then
        fatal "missing composefs"
    fi
fi

# Test the bootable and linux keys
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo --print-metadata-key=ostree.linux show testos:testos/buildmain/x86_64-runtime >out.txt
assert_file_has_content_literal out.txt 3.6.0
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo --print-metadata-key=ostree.bootable show testos:testos/buildmain/x86_64-runtime >out.txt
assert_file_has_content_literal out.txt true

echo "ok deploy command"

${CMD_PREFIX} ostree admin --print-current-dir > curdir
assert_file_has_content curdir ^`pwd`/sysroot/ostree/deploy/testos/deploy/${rev}\.0$

echo "ok --print-current-dir"

if ${CMD_PREFIX} ostree admin deploy --stateroot=nosuchroot testos:testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "deployed to nonexistent root"
fi
assert_file_has_content err.txt "error:.*No such stateroot: nosuchroot"

echo "ok nice error for deploy with no stateroot"

# Test layout of bootloader config and refs
assert_not_has_dir sysroot/boot/loader.0
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.1
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.1'
else
    assert_has_dir sysroot/boot/loader.1
fi
assert_has_dir sysroot/ostree/boot.1.1
assert_has_file sysroot/boot/loader/entries/ostree-1.conf
assert_file_has_content sysroot/boot/loader/entries/ostree-1.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/loader/entries/ostree-1.conf 'options.* quiet'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.1/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
assert_ostree_deployment_refs 1/1/0
${CMD_PREFIX} ostree admin status
echo "ok layout"

if ${CMD_PREFIX} ostree admin deploy --stage --os=testos testos:testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "staged when not booted"
fi
assert_file_has_content_literal err.txt "Cannot stage deployment: Not currently booted into an OSTree system"
echo "ok staging does not work when not booted"

orig_mtime=$(stat -c '%.Y' sysroot/ostree/deploy)
${CMD_PREFIX} ostree admin deploy --stateroot=testos testos:testos/buildmain/x86_64-runtime
new_mtime=$(stat -c '%.Y' sysroot/ostree/deploy)
assert_not_streq "${orig_mtime}" "${new_mtime}"
# Need a new bootversion, sine we now have two deployments
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.0
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.0'
else
    assert_has_dir sysroot/boot/loader.0
fi
assert_not_has_dir sysroot/boot/loader.1
assert_has_dir sysroot/ostree/boot.0.1
assert_not_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.1.0
assert_not_has_dir sysroot/ostree/boot.1.1
# Ensure we propagated kernel arguments from previous deployment
assert_file_has_content sysroot/boot/loader/entries/ostree-2.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.0/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
assert_ostree_deployment_refs 0/1/{0,1}
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok second deploy"

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
# Keep the same bootversion
assert_not_has_dir sysroot/boot/loader.1
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.0
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.0'
else
    assert_has_dir sysroot/boot/loader.0
fi
# But swap subbootversion
assert_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.0.1
assert_ostree_deployment_refs 0/0/{0,1}
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok third deploy (swap)"

${CMD_PREFIX} ostree admin os-init otheros

${CMD_PREFIX} ostree admin deploy --os=otheros testos/buildmain/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.0
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.1
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.1'
else
    assert_has_dir sysroot/boot/loader.1
fi
assert_has_file sysroot/boot/loader/entries/ostree-2.conf
assert_has_file sysroot/boot/loader/entries/ostree-3.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/otheros/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_ostree_deployment_refs 1/1/{0,1,2}
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok independent deploy"

${CMD_PREFIX} ostree admin deploy --retain --os=testos testos:testos/buildmain/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.1
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.0
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.0'
else
    assert_has_dir sysroot/boot/loader.0
fi
assert_has_file sysroot/boot/loader/entries/ostree-4.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.2/etc/os-release 'NAME=TestOS'
assert_has_file sysroot/boot/loader/entries/ostree-2.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
assert_ostree_deployment_refs 0/1/{0,1,2,3}
validate_bootloader

echo "ok fourth deploy (retain)"

echo "a new local config file" > sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/a-new-config-file
rm -r  sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/testdirectory
rm sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/aconfigfile
ln -s /ENOENT sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/a-new-broken-symlink
${CMD_PREFIX} ostree admin deploy --retain --os=testos testos:testos/buildmain/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.0
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.1
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.1'
else
    assert_has_dir sysroot/boot/loader.1
fi
link=sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/a-new-broken-symlink
if ! test -L ${link}; then
    ls -al ${link}
    fatal "Not a symlink: ${link}"
fi
linktarget=$(readlink ${link})
assert_streq "${linktarget}" /ENOENT
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/a-new-config-file 'a new local config file'
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/aconfigfile
${CMD_PREFIX} ostree admin status
validate_bootloader
echo "ok deploy with modified /etc"

if ${CMD_PREFIX} ostree admin undeploy blah 2>err.txt; then
    fatal "undeploy parsed string"
fi
assert_file_has_content_literal err.txt 'error: Invalid index: blah'
echo "ok undeploy error invalid int"

# we now have 5 deployments, let's bring that back down to 1
for i in $(seq 4); do
  ${CMD_PREFIX} ostree admin undeploy 0
done
assert_has_file sysroot/boot/loader/entries/ostree-1.conf
assert_not_has_file sysroot/boot/loader/entries/ostree-2.conf
assert_not_has_file sysroot/boot/loader/entries/ostree-3.conf
${CMD_PREFIX} ostree admin deploy --not-as-default --os=otheros testos:testos/buildmain/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.1
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.0
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.0'
else
    assert_has_dir sysroot/boot/loader.0
fi
assert_has_file sysroot/boot/loader/entries/ostree-2.conf
assert_has_file sysroot/boot/loader/entries/ostree-1.conf
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok deploy --not-as-default"

${CMD_PREFIX} ostree admin deploy --retain-rollback --os=otheros testos:testos/buildmain/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.0
if [ "${folders_instead_symlinks_in_boot}" == "1" ]; then
    assert_not_has_dir sysroot/boot/loader.1
    assert_file_has_content sysroot/boot/loader/ostree_bootversion 'loader.1'
else
    assert_has_dir sysroot/boot/loader.1
fi
assert_has_file sysroot/boot/loader/entries/ostree-3.conf
assert_has_file sysroot/boot/loader/entries/ostree-2.conf
assert_has_file sysroot/boot/loader/entries/ostree-1.conf
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok deploy --retain-rollback"


${CMD_PREFIX} ostree admin status
assert_file_has_content sysroot/boot/loader/entries/ostree-3.conf "^title.*TestOS 42 1.0.10"
${CMD_PREFIX} ostree admin set-default 1
assert_file_has_content sysroot/boot/loader/entries/ostree-3.conf "^title.*TestOS 42 1.0.10"
${CMD_PREFIX} ostree admin set-default 1
assert_file_has_content sysroot/boot/loader/entries/ostree-3.conf "^title.*TestOS 42 1.0.10"

echo "ok set-default"

os_repository_new_commit
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos:testos/buildmain/x86_64-runtime)
export newrev
assert_not_streq ${rev} ${newrev}

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
# New files in /usr/etc
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/a-new-default-config-file "a new default config file"
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/new-default-dir/moo "a new default dir and file"
# And persist /etc changes from before
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/aconfigfile
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok upgrade bare"

os_repository_new_commit
if env OSTREE_EX_STAGE_DEPLOYMENTS=1 ${CMD_PREFIX} ostree admin upgrade --os=testos 2>err.txt; then
    fatal "staged when not booted"
fi
echo "ok upgrade failed when staged"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
origrev=${rev}
rev=${newrev}
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmain/x86_64-runtime)
assert_not_streq ${rev} ${newrev}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
validate_bootloader
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo refs testos:testos > reftest.txt
assert_file_has_content reftest.txt testos:buildmain/x86_64-runtime

echo "ok upgrade"

originfile=$(${CMD_PREFIX} ostree admin --print-current-dir).origin
cp ${originfile} saved-origin
${CMD_PREFIX} ostree admin set-origin --index=0 bacon --set=gpg-verify=false http://tasty.com
assert_file_has_content "${originfile}" "bacon:testos/buildmain/x86_64-runtime"
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote list -u > remotes.txt
assert_file_has_content remotes.txt 'bacon.*http://tasty\.com'
cp saved-origin ${originfile}
validate_bootloader

echo "ok set-origin"

assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin undeploy 1
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
assert_not_has_dir sysroot/ostree/deploy/testos/deploy/${rev}.0

${CMD_PREFIX} ostree admin undeploy 0
assert_not_has_dir sysroot/ostree/deploy/testos/deploy/${newrev}.0
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok undeploy"

if ${CMD_PREFIX} ostree admin deploy --os=unknown testos:testos/buildmain/x86_64-runtime; then
    assert_not_reached "Unexpected successful deploy of unknown OS"
fi
echo "ok deploy with unknown OS"

${CMD_PREFIX} ostree admin deploy --os=testos --karg-append=console=/dev/foo --karg-append=console=/dev/bar testos:testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-4.conf 'console=/dev/foo.*console=/dev/bar'
validate_bootloader

echo "ok deploy with multiple kernel args"

origrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmain/x86_64-runtime)
os_repository_new_commit 0 "test upgrade multiple kernel args"
${CMD_PREFIX} ostree admin upgrade --os=testos
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmain/x86_64-runtime)
assert_not_streq ${origrev} ${newrev}
assert_file_has_content sysroot/boot/loader/entries/ostree-4.conf 'console=/dev/foo.*console=/dev/bar'
validate_bootloader

echo "ok upgrade with multiple kernel args"

os_repository_new_commit
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_file_has_content sysroot/boot/loader/entries/ostree-4.conf "^title TestOS 42 ${version} (ostree:testos:0)$"
os_repository_new_commit 0 0 testos/buildmain/x86_64-runtime 42
${CMD_PREFIX} ostree admin upgrade --os=testos
assert_file_has_content sysroot/boot/loader/entries/ostree-4.conf "^title TestOS 42 (ostree:testos:0)$"

echo "ok no duplicate version strings in title"


# Test upgrade with and without --override-commit
# See https://github.com/GNOME/ostree/pull/147
sleep 1
os_repository_new_commit
# upgrade to the latest
${CMD_PREFIX} ostree admin upgrade --os=testos
head_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmain/x86_64-runtime)
prev_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmain/x86_64-runtime^)
assert_not_streq ${head_rev} ${prev_rev}
# Don't use `ostree admin status | head -n 1` directly here because `head`
# exiting early might cause SIGPIPE to ostree, which with `set -euo pipefail`
# will cause us to exit. See: https://github.com/ostreedev/ostree/pull/2110.
${CMD_PREFIX} ostree admin status > status-out.txt
head -n 1 < status-out.txt > status.txt
assert_file_has_content status.txt ".* testos ${head_rev}.*"
# now, check that we can't downgrade to an older commit without --allow-downgrade
if ${CMD_PREFIX} ostree admin upgrade --os=testos --override-commit=${prev_rev} 2> err.txt; then
    cat err.txt
    fatal "downgraded without --allow-downgrade?"
fi
assert_file_has_content err.txt "Upgrade.*is chronologically older"
${CMD_PREFIX} ostree admin upgrade --os=testos --override-commit=${prev_rev} --allow-downgrade
${CMD_PREFIX} ostree admin status > status-out.txt
head -n 1 < status-out.txt > status.txt
assert_file_has_content status.txt ".* testos ${prev_rev}.*"
${CMD_PREFIX} ostree admin upgrade --os=testos
${CMD_PREFIX} ostree admin status > status-out.txt
head -n 1 < status-out.txt > status.txt
assert_file_has_content status.txt ".* testos ${head_rev}.*"

echo "ok upgrade with and without override-commit"

# check that we can still upgrade to a rev that's not the tip of the branch but
# that's still newer than the deployment
sleep 1
os_repository_new_commit
sleep 1
os_repository_new_commit
${CMD_PREFIX} ostree pull --repo=sysroot/ostree/repo --commit-metadata-only --depth=-1 testos:testos/buildmain/x86_64-runtime
curr_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmain/x86_64-runtime)
prev_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmain/x86_64-runtime^)
${CMD_PREFIX} ostree admin upgrade --os=testos --override-commit=${prev_rev}
echo "ok upgrade to newer version older than branch tip"

${CMD_PREFIX} ostree --repo=${test_tmpdir}/testos-repo commit  --add-metadata-string "version=${version}" \
              --add-metadata-string 'ostree.source-title=libtest os_repository_new_commit()' -b testos/buildmain/x86_64-runtime \
              -s "Build" --tree=dir=${test_tmpdir}/osdata
${CMD_PREFIX} ostree admin upgrade --os=testos
${CMD_PREFIX} ostree admin status | tee status.txt
assert_file_has_content_literal status.txt '`- libtest os_repository_new_commit()'
echo "ok source title"

deployment=$(${CMD_PREFIX} ostree admin --sysroot=sysroot --print-current-dir)
${CMD_PREFIX} ostree --sysroot=sysroot remote add --set=gpg-verify=false remote-test-physical file://$(pwd)/testos-repo
assert_not_has_file ${deployment}/etc/ostree/remotes.d/remote-test-physical.conf testos-repo
assert_file_has_content sysroot/ostree/repo/config remote-test-physical
echo "ok remote add physical sysroot"

# Now a hack...symlink ${deployment}/sysroot to the sysroot in lieu of a bind
# mount which we can't do in unit tests.
ln -sr sysroot ${deployment}/sysroot
ln -s sysroot/ostree ${deployment}/ostree
${CMD_PREFIX} ostree --sysroot=${deployment} remote add --set=gpg-verify=false remote-test-nonphysical file://$(pwd)/testos-repo
assert_not_file_has_content sysroot/ostree/repo/config remote-test-nonphysical
assert_file_has_content ${deployment}/etc/ostree/remotes.d/remote-test-nonphysical.conf testos-repo
echo "ok remote add nonphysical sysroot"

# Test that setting add-remotes-config-dir to false adds a remote in the
# config file rather than the remotes config dir even though this is a
# "system" repo.
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo config set core.add-remotes-config-dir false
${CMD_PREFIX} ostree --sysroot=${deployment} remote add --set=gpg-verify=false remote-test-config-dir file://$(pwd)/testos-repo
assert_not_has_file ${deployment}/etc/ostree/remotes.d/remote-test-config-dir.conf testos-repo
assert_file_has_content sysroot/ostree/repo/config remote-test-config-dir
echo "ok remote add nonphysical sysroot add-remotes-config-dir false"

if env OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},test-fifreeze" \
       ${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmain/x86_64-runtime 2>err.txt; then
    fatal "fifreeze-test exited successfully?"
fi
assert_file_has_content err.txt "fifreeze watchdog was run"
assert_file_has_content err.txt "During fsfreeze-thaw: aborting due to test-fifreeze"
echo "ok fifreeze test"
