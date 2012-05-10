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

class OstbuildTreeToSrc(builtins.Builtin):
    name = "tree-to-src"
    short_description = "Turn a tree into a source snapshot"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def bin_snapshot_to_src(self, bin_snapshot):
        del bin_snapshot['00ostree-bin-snapshot-version']

        src_snapshot = dict(bin_snapshot)
        src_snapshot['00ostree-src-snapshot-version'] = 0

        return src_snapshot

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--tree')

        args = parser.parse_args(argv)
        self.parse_config()
        if args.prefix:
            self.prefix = args.prefix

        if args.tree:
            self.load_bin_snapshot_from_path(args.tree)
        else:
            self.load_bin_snapshot_from_current()

        snapshot = self.bin_snapshot_to_src(self.bin_snapshot)
        db = self.get_src_snapshot_db()
        path = db.store(snapshot)
        log("Source snapshot: %s" % (path, ))

builtins.register(OstbuildTreeToSrc)
