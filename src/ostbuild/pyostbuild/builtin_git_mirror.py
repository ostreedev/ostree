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
from . import vcs
from .subprocess_helpers import run_sync, run_sync_get_output
from . import buildutil

class OstbuildGitMirror(builtins.Builtin):
    name = "git-mirror"
    short_description = "Update internal git mirror for one or more components"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--src-snapshot')
        parser.add_argument('--fetch', action='store_true')
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        self.parse_config()
        self.parse_snapshot(args.prefix, args.src_snapshot)

        if len(args.components) == 0:
            components = []
            for component in self.snapshot['components']:
                components.append(component['name'])
        else:
            components = args.components

        for name in components:
            component = self.get_component(name)
            src = component['src']
            (keytype, uri) = vcs.parse_src_key(src)
            mirrordir = vcs.ensure_vcs_mirror(self.mirrordir, keytype, uri, component['branch'])

            if args.fetch:
                log("Running git fetch for %s" % (name, ))
                run_sync(['git', 'fetch'], cwd=mirrordir, log_initiation=False)

builtins.register(OstbuildGitMirror)
