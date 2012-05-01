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

class OstbuildTreeToBin(builtins.Builtin):
    name = "tree-to-bin"
    short_description = "Turn a tree into a binary snapshot"

    def __init__(self):
        builtins.Builtin.__init__(self)

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

        db = self.get_bin_snapshot_db()
        path = db.store(self.bin_snapshot)
        log("Binary snapshot: %s" % (path, ))

builtins.register(OstbuildTreeToBin)
