#
# Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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

echo "1..10"

function validate_bootloader() {
    (cd ${test_tmpdir};
     if test -f sysroot/boot/syslinux/syslinux.cfg; then
	$(dirname $0)/syslinux-entries-crosscheck.py sysroot
     fi)
}

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin status | tee status.txt
validate_bootloader

echo "ok deploy command"

${CMD_PREFIX} ostree admin --print-current-dir > curdir
assert_file_has_content curdir ^`pwd`/sysroot/ostree/deploy/testos/deploy/${rev}.0$

echo "ok --print-current-dir"

assert_not_has_dir sysroot/boot/loader.0
assert_has_dir sysroot/boot/loader.1
assert_has_dir sysroot/ostree/boot.1.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-0.conf
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* quiet'
assert_file_has_content sysroot/boot/ostree/testos-${bootcsum}/vmlinuz-3.6.0 'a kernel'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.1/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status


echo "ok layout"

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Need a new bootversion, sine we now have two deployments
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
assert_has_dir sysroot/ostree/boot.0.1
assert_not_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.1.0
assert_not_has_dir sysroot/ostree/boot.1.1
# Ensure we propagated kernel arguments from previous deployment
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.* root=LABEL=MOO'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/boot.0/testos/${bootcsum}/0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok second deploy"

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
# Keep the same bootversion
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
# But swap subbootversion
assert_has_dir sysroot/ostree/boot.0.0
assert_not_has_dir sysroot/ostree/boot.0.1
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok third deploy (swap)"

${CMD_PREFIX} ostree admin os-init otheros

${CMD_PREFIX} ostree admin deploy --os=otheros testos/buildmaster/x86_64-runtime
assert_not_has_dir sysroot/boot/loader.0
assert_has_dir sysroot/boot/loader.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-1.conf
assert_has_file sysroot/boot/loader/entries/ostree-otheros-0.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.1/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/otheros/deploy/${rev}.0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok independent deploy"

${CMD_PREFIX} ostree admin deploy --retain --os=testos testos:testos/buildmaster/x86_64-runtime
assert_has_dir sysroot/boot/loader.0
assert_not_has_dir sysroot/boot/loader.1
assert_has_file sysroot/boot/loader/entries/ostree-testos-0.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.2/etc/os-release 'NAME=TestOS'
assert_has_file sysroot/boot/loader/entries/ostree-testos-2.conf
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok fourth deploy (retain)"

echo "a new local config file" > sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/a-new-config-file
rm -r  sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/testdirectory
rm sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/aconfigfile
ln -s /ENOENT sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/a-new-broken-symlink
${CMD_PREFIX} ostree admin deploy --retain --os=testos testos:testos/buildmaster/x86_64-runtime
linktarget=$(readlink sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/a-new-broken-symlink)
test "${linktarget}" = /ENOENT
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/os-release 'NAME=TestOS'
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/a-new-config-file 'a new local config file'
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.4/etc/aconfigfile
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok deploy with modified /etc"

os_repository_new_commit
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos:testos/buildmaster/x86_64-runtime)
export newrev
assert_not_streq ${rev} ${newrev}

${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
# New files in /usr/etc
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/a-new-default-config-file "a new default config file"
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/new-default-dir/moo "a new default dir and file"
# And persist /etc changes from before
assert_not_has_file sysroot/ostree/deploy/testos/deploy/${rev}.3/etc/aconfigfile
${CMD_PREFIX} ostree admin status
validate_bootloader

echo "ok upgrade bare"

os_repository_new_commit
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin upgrade --os=testos
origrev=${rev}
rev=${newrev}
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_streq ${rev} ${newrev}
assert_file_has_content sysroot/ostree/deploy/testos/deploy/${newrev}.0/etc/os-release 'NAME=TestOS'
${CMD_PREFIX} ostree admin status
validate_bootloader
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo refs testos:testos > reftest.txt
assert_file_has_content reftest.txt testos:buildmaster/x86_64-runtime

echo "ok upgrade"

originfile=$(${CMD_PREFIX} ostree admin --print-current-dir).origin
cp ${originfile} saved-origin
${CMD_PREFIX} ostree admin set-origin --index=0 bacon --set=gpg-verify=false http://tasty.com
assert_file_has_content "${originfile}" "bacon:testos/buildmaster/x86_64-runtime"
${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote list -u > remotes.txt
assert_file_has_content remotes.txt 'bacon.*http://tasty.com'
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

if ${CMD_PREFIX} ostree admin deploy --os=unknown testos:testos/buildmaster/x86_64-runtime; then
    assert_not_reached "Unexpected successful deploy of unknown OS"
fi
echo "ok deploy with unknown OS"

${CMD_PREFIX} ostree admin deploy --os=testos --karg-append=console=/dev/foo --karg-append=console=/dev/bar testos:testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'console=/dev/foo.*console=/dev/bar'
validate_bootloader

echo "ok deploy with multiple kernel args"

origrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
os_repository_new_commit 0 "test upgrade multiple kernel args"
${CMD_PREFIX} ostree admin upgrade --os=testos
newrev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
assert_not_streq ${origrev} ${newrev}
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'console=/dev/foo.*console=/dev/bar'
validate_bootloader

echo "ok upgrade with multiple kernel args"

# Test upgrade with and without --override-commit
# See https://github.com/GNOME/ostree/pull/147
${CMD_PREFIX} ostree pull --repo=sysroot/ostree/repo --commit-metadata-only --depth=-1 testos:testos/buildmaster/x86_64-runtime
head_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmaster/x86_64-runtime)
prev_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmaster/x86_64-runtime^^^^)
assert_not_streq ${head_rev} ${prev_rev}
${CMD_PREFIX} ostree admin upgrade --os=testos --override-commit=${prev_rev} --allow-downgrade
curr_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmaster/x86_64-runtime)
assert_streq ${curr_rev} ${prev_rev}
${CMD_PREFIX} ostree admin upgrade --os=testos
curr_rev=$(${CMD_PREFIX} ostree rev-parse --repo=sysroot/ostree/repo testos/buildmaster/x86_64-runtime)
assert_streq ${curr_rev} ${head_rev}

echo "ok upgrade with and without override-commit"
