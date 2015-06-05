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
import subprocess
import argparse

parser = argparse.ArgumentParser(description='Fix OSTree/GRUB2 configuration')
parser.add_argument('--yes', action='store_true', help='Do not prompt, automatically fix', default=False)
parser.add_argument('--sysroot', action='store', help='System root', default='')
parser.add_argument('--grub2path', action='store', help='Path to grub2 configuration')

args = parser.parse_args()

loaderpath = args.sysroot + '/boot/loader/entries'

grub2efi_etc = args.sysroot + '/etc/grub2-efi.cfg'
if os.path.islink(grub2efi_etc) and os.path.exists(grub2efi_etc):
    grub2path = os.path.realpath(grub2efi_etc)
else:
    grub2path = args.sysroot + '/boot/loader/grub.cfg'

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
            if line == '' or line.startswith('#'):
                continue
            s = line.find(' ')
            assert s > 0
            k = line[0:s]
            v = line[s+1:]
            entry[k] = v
        entries.append(entry)
    entries.sort(compare_entries_descending)

# Parse GRUB2 config...not in a really beautiful way, but it
# works well enough for what we need to do.
with open(grub2path) as f:
    in_ostree_config = False
    grub2_entry = None
    for line in f:
        line = line.rstrip()
        if line.startswith('### BEGIN /etc/grub.d/15_ostree ###'):
            in_ostree_config = True
        elif line.startswith('### END /etc/grub.d/15_ostree ###'):
            in_ostree_config = False
            if grub2_entry is not None:
                grub2_entries.append(grub2_entry)
        elif in_ostree_config:
            if line.startswith('menuentry '):
                assert grub2_entry is None
                grub2_entry = {'lines': []}
            elif line.startswith('linux'):
                assert grub2_entry is not None
                parts = line.split()
                grub2_entry['linux'] = parts[1]
                grub2_entry['options'] = ' '.join(parts[2:])
            elif line.startswith('initrd'):
                assert grub2_entry is not None
                grub2_entry['initrd'] = line.split()[1]

            if grub2_entry is not None:
                grub2_entry['lines'].append(line)
                if line == '}':
                    grub2_entries.append(grub2_entry)
                    grub2_entry = None

if len(entries) != len(grub2_entries):
    fatal("Found {0} loader entries, but {1} GRUB2 entries\n".format(len(entries), len(grub2_entries)))

def assert_matches_key(a, b, key):
    aval = a[key]
    bval = b[key]
    if aval != bval:
        fatal("Mismatch on {0}: {1} != {2}".format(key, aval, bval))

unmatched_entries = list(entries)

was_sorted = True
for grub2entry in grub2_entries:
    found = False
    for j, entry in enumerate(unmatched_entries):
        if not (entry['linux'] == grub2entry['linux'] and
                entry['initrd'] == grub2entry['initrd']):
            continue
        entry_ostree = get_ostree_option(entry['options'])
        grub2_ostree = get_ostree_option(grub2entry['options'])
        if entry_ostree != grub2_ostree:
            continue
        grub2entry['entry'] = entry
        found = True
        if j != 0:
            was_sorted = False
        del unmatched_entries[j]
        break
    if not found:
        fatal("Failed to find entry corresponding to GRUB2 entry {0}".format(grub2entry))

if was_sorted:
    print "GRUB2 configuration validated"
    sys.exit(0)

def compare_grub2_entries(a, b):
    eav = int(a['entry']['version'])
    ebv = int(b['entry']['version'])
    # Reversed
    return -cmp(eav, ebv)

grub2_entries.sort(compare_grub2_entries)

grub2tmp = grub2path + '.tmp'
with open(grub2tmp, 'w') as fout:
    with open(grub2path) as fin:
        in_ostree_config = False
        for line in fin:
            line = line.rstrip()
            if line.startswith('### BEGIN /etc/grub.d/15_ostree ###'):
                in_ostree_config = True
                fout.write(line)
                fout.write('\n')
                for grub2entry in grub2_entries:
                    for line in grub2entry['lines']:
                        fout.write(line)
                        fout.write('\n')
            elif line.startswith('### END /etc/grub.d/15_ostree ###'):
                in_ostree_config = False

            if not in_ostree_config:
                fout.write(line)
                fout.write('\n')

if not args.yes:
    subprocess.call(['diff', '-u', grub2path, grub2tmp])
    sys.stdout.write("Update bootloader configuration? [y/N] ")
    answer = raw_input()
else:
    answer = 'y'
if answer.startswith('y') or answer.startswith('Y'):
    os.rename(grub2tmp, grub2path)
    sys.stdout.write('GRUB2 configuration modified: re-synchronized with /boot/loader/entries\n')
else:
    os.unlink(grub2tmp)
    sys.stdout.write('GRUB2 configuration unchanged\n')
sys.exit(0)
