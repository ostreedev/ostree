#!/usr/bin/python3
#
# Copyright (C) 2015 Red Hat
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

import os
import sys

if len(sys.argv) == 1:
    sysroot = ''
else:
    sysroot = sys.argv[1]

bootloader = sys.argv[2]
loaderpath = sysroot + '/boot/loader/entries'
syslinuxpath = sysroot + '/boot/syslinux/syslinux.cfg'

if bootloader == "grub2":
    sys.stdout.write('GRUB2 configuration validation not implemented.\n')
    sys.exit(0)

def fatal(msg):
    sys.stderr.write(msg)
    sys.stderr.write('\n')
    sys.exit(1)

def entry_get_version(entry):
    return int(entry['version'])

def get_ostree_option(optionstring):
    for o in optionstring.split():
        if o.startswith('ostree='):
            return o[8:]
    raise ValueError('ostree= not found')
            
entries = []
syslinux_entries = []

# Parse loader configs
for fname in os.listdir(loaderpath):
    path = os.path.join(loaderpath, fname)
    with open(path) as f:
        entry = {}
        for line in f:
            line = line.strip()
            if (line == '' or line.startswith('#')):
                continue
            s = line.find(' ')
            assert s > 0
            k = line[0:s]
            v = line[s+1:]
            entry[k] = v
        entries.append(entry)
    entries.sort(key=entry_get_version, reverse=True)

# Parse SYSLINUX config
with open(syslinuxpath) as f:
    in_ostree_config = False
    syslinux_entry = None
    syslinux_default = None
    for line in f:
        try:
            k, v = line.strip().split(" ", 1)
        except ValueError:
            continue
        if k == 'DEFAULT':
            if syslinux_entry is not None:
                syslinux_default = v
        elif k == 'LABEL':
            if syslinux_entry is not None:
                syslinux_entries.append(syslinux_entry)
            syslinux_entry = {}
            syslinux_entry['title'] = v
        elif k == 'KERNEL':
            syslinux_entry['linux'] = v
        elif k == 'INITRD':
            syslinux_entry['initrd'] = v
        elif k == 'APPEND':
            syslinux_entry['options'] = v
    if syslinux_entry is not None:
        syslinux_entries.append(syslinux_entry)

if len(entries) != len(syslinux_entries):
    fatal("Found {0} loader entries, but {1} SYSLINUX entries\n".format(len(entries), len(syslinux_entries)))


def assert_eq(a, b):
    assert a == b, "%r == %r" % (a, b)


def assert_key_same_file(a, b, key):
    aval = a[key]
    bval = b[key]
    sys.stderr.write("aval: %r\nbval: %r\n" % (aval, bval))

    # Paths in entries are always relative to /boot
    entry = os.stat(sysroot + "/boot" + aval)

    # Syslinux entries can be relative to /boot (if it's on another filesystem)
    # or relative to / if /boot is on /.
    s1 = os.stat(sysroot + bval)
    s2 = os.stat(sysroot + "/boot" + bval)

    # A symlink ensures that no matter what they point at the same file
    assert_eq(entry, s1)
    assert_eq(entry, s2)


for i,(entry,syslinuxentry) in enumerate(zip(entries, syslinux_entries)):
    assert_key_same_file(entry, syslinuxentry, 'linux')
    assert_key_same_file(entry, syslinuxentry, 'initrd')
    entry_ostree = get_ostree_option(entry['options'])
    syslinux_ostree = get_ostree_option(syslinuxentry['options'])
    if entry_ostree != syslinux_ostree:
        fatal("Mismatch on ostree option: {0} != {1}".format(entry_ostree, syslinux_ostree))

sys.stdout.write('SYSLINUX configuration validated\n')
sys.exit(0)
