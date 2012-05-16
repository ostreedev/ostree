# Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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

# ostbuild-compile-one-make wraps systems that implement the GNOME build API:
# http://people.gnome.org/~walters/docs/build-api.txt

import os,sys,stat,subprocess,tempfile,re,shutil
import argparse
from StringIO import StringIO
import json

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from . import buildutil

class OstbuildImportTree(builtins.Builtin):
    name = "import-tree"
    short_description = "Extract source data from tree into new prefix"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--tree')
        parser.add_argument('--prefix')

        args = parser.parse_args(argv)
        self.parse_config()
        self.parse_snapshot_from_current()

        log("Loading source from tree %r" % (self.snapshot_path, ))

        related_objects = run_sync_get_output(['ostree', '--repo='+ self.repo,
                                               'show', '--print-related',
                                               self.active_branch_checksum])
        ref_to_revision = {}
        for line in StringIO(related_objects):
            line = line.strip()
            (ref, revision) = line.split(' ', 1)
            ref_to_revision[ref] = revision

        if args.prefix:
            target_prefix = args.prefix
        else:
            target_prefix = self.snapshot['prefix']

        (fd, tmppath) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-import-tree-')
        f = os.fdopen(fd, 'w')
        for (ref, rev) in ref_to_revision.iteritems():
            if ref.startswith('components/'):
                ref = ref[len('components/'):]
                (prefix, subref) = ref.split('/', 1)
                newref = 'components/%s/%s' % (target_prefix, subref)
            elif ref.startswith('bases/'):
                # hack
                base_key = '/' + self.snapshot['prefix'] + '-'
                replace_key = '/' + target_prefix + '-'
                newref = ref.replace(base_key, replace_key)
            else:
                fatal("Unhandled ref %r; expected components/ or bases/" % (ref, ))
                
            f.write('%s %s\n' % (newref, rev))
        f.close()

        run_sync(['ostree', '--repo=' + self.repo,
                  'write-refs'], stdin=open(tmppath))

        self.snapshot['prefix'] = target_prefix

        run_sync(['ostbuild', 'prefix', target_prefix])
        self.prefix = target_prefix

        db = self.get_src_snapshot_db()
        path = db.store(self.snapshot)
        log("Source snapshot: %s" % (path, ))

builtins.register(OstbuildImportTree)
