#!/usr/bin/python
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
    loaderpath = '/boot/loader/entries'
    grub2path = '/boot/grub2/grub.cfg'
else:
    loaderpath = sys.argv[1]
    grub2path = sys.argv[2]

def fatal(msg):
    sys.stderr.write(msg)
    sys.stderr.write('\n')
    sys.exit(1)

def compare_entries_descending(a, b):
    return int(b['version']) - int(a['version'])

def get_ostree_option(optionstring):
    for o in optionstring.split():
        if o.startswith('ostree='):
            return o[8:]
    raise ValueError('ostree= not found')
            
entries = []
grub2_entries = []

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
    entries.sort(compare_entries_descending)

# Parse GRUB2 config
with open(grub2path) as f:
    in_ostree_config = False
    grub2_entry = None
    for line in f:
        if line.startswith('### BEGIN /etc/grub.d/15_ostree ###'):
            in_ostree_config = True
        elif line.startswith('### END /etc/grub.d/15_ostree ###'): 
            in_ostree_config = False
            if grub2_entry is not None:
                grub2_entries.append(grub2_entry)
        elif in_ostree_config:
            if line.startswith('menuentry '):
                if grub2_entry is not None:
                    grub2_entries.append(grub2_entry)
                grub2_entry = {}
            elif line.startswith('linux'):
                parts = line.split()
                grub2_entry['linux'] = parts[1]
                grub2_entry['options'] = ' '.join(parts[2:])
            elif line.startswith('initrd'):
                grub2_entry['initrd'] = line.split()[1]

if len(entries) != len(grub2_entries):
    fatal("Found {0} loader entries, but {1} GRUB2 entries\n".format(len(entries), len(grub2_entries)))

def assert_matches_key(a, b, key):
    aval = a[key]
    bval = b[key]
    if aval != bval:
        fatal("Mismatch on {0}: {1} != {2}".format(key, aval, bval))

for i,(entry,grub2entry) in enumerate(zip(entries, grub2_entries)):
    assert_matches_key(entry, grub2entry, 'linux')
    assert_matches_key(entry, grub2entry, 'initrd')
    entry_ostree = get_ostree_option(entry['options'])
    grub2_ostree = get_ostree_option(grub2entry['options'])
    if entry_ostree != grub2_ostree:
        fatal("Mismatch on ostree option: {0} != {1}".format(entry_ostree, grub2_ostree))

sys.stdout.write('GRUB2 configuration validated\n')
sys.exit(0)
