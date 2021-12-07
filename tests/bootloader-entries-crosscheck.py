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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

import os
import sys


def main(argv):
    _, sysroot, bootloader = argv

    if bootloader == "grub2":
        sys.stdout.write('GRUB2 configuration validation not implemented.\n')
        return 0
    else:
        return validate_syslinux(sysroot)


def fatal(msg):
    sys.stderr.write(msg)
    sys.stderr.write('\n')
    sys.exit(1)


def get_ostree_option(optionstring):
    for o in optionstring.split():
        if o.startswith('ostree='):
            return o[8:]
    raise ValueError('ostree= not found in %r' % (optionstring,))


def parse_loader_configs(sysroot):
    loaderpath = sysroot + '/boot/loader/entries'
    entries = []

    # Parse loader configs
    for fname in os.listdir(loaderpath):
        path = os.path.join(loaderpath, fname)
        entry = {}
        with open(path) as f:
            for line in f:
                line = line.strip()
                if (line == '' or line.startswith('#')):
                    continue
                k, v = line.split(' ', 1)
                entry[k] = v
        entries.append(entry)
    entries.sort(key=lambda e: int(e['version']), reverse=True)
    return entries


def validate_syslinux(sysroot):
    syslinuxpath = sysroot + '/boot/syslinux/syslinux.cfg'

    entries = parse_loader_configs(sysroot)
    syslinux_entries = []

    # Parse SYSLINUX config
    with open(syslinuxpath) as f:
        syslinux_entry = None
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
        fatal("Found {0} loader entries, but {1} SYSLINUX entries\n".format(
            len(entries), len(syslinux_entries)))

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

    for i, (entry, syslinuxentry) in enumerate(zip(entries, syslinux_entries)):
        assert_key_same_file(entry, syslinuxentry, 'linux')
        assert_key_same_file(entry, syslinuxentry, 'initrd')
        entry_ostree = get_ostree_option(entry['options'])
        syslinux_ostree = get_ostree_option(syslinuxentry['options'])
        if entry_ostree != syslinux_ostree:
            fatal("Mismatch on ostree option: {0} != {1}".format(
                entry_ostree, syslinux_ostree))

    sys.stdout.write('SYSLINUX configuration validated\n')
    return 0


def assert_eq(a, b):
    assert a == b, "%r == %r" % (a, b)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
