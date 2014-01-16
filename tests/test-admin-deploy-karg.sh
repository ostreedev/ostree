#!/bin/bash
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

set -e

. $(dirname $0)/libtest.sh

echo "1..1"

setup_os_repository "archive-z2" "syslinux"

echo "ok setup"

echo "1..1"

ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
rev=$(ostree --repo=sysroot/ostree/repo rev-parse testos/buildmaster/x86_64-runtime)
export rev
# This initial deployment gets kicked off with some kernel arguments 
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot deploy --karg=FOO=BAR --os=testos testos:testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot deploy --karg=TESTARG=TESTVALUE --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-1.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*TESTARG=TESTVALUE'
ostree admin --sysroot=sysroot deploy --karg=ANOTHERARG=ANOTHERVALUE --os=testos testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*TESTARG=TESTVALUE'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*ANOTHERARG=ANOTHERVALUE'

echo "ok deploy with --karg, but same config"

ostree admin --sysroot=sysroot deploy --karg-proc-cmdline --os=testos testos:testos/buildmaster/x86_64-runtime
# Here we're asserting that the *host* system has a root= kernel
# argument.  I think it's unlikely that anyone doesn't have one, but
# if this is not true for you, please file a bug!
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*root=.'

echo "ok deploy --karg-proc-cmdline"

ostree admin --sysroot=sysroot status
ostree admin --sysroot=sysroot undeploy 0

ostree admin --sysroot=sysroot deploy  --os=testos --karg-append=APPENDARG=VALAPPEND --karg-append=APPENDARG=2NDAPPEND testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*TESTARG=TESTVALUE'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*APPENDARG=VALAPPEND .*APPENDARG=2NDAPPEND'

echo "ok deploy --karg-append"
