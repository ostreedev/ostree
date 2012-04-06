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

class OstbuildPrefix(builtins.Builtin):
    name = "prefix"
    short_description = "Display or modify \"prefix\" (build target)"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _set_prefix(self, prefix):
        f = open(self.path, 'w')
        f.write(prefix)
        f.write('\n')
        f.close()
        log("Prefix is now %r" % (prefix, ))

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('-a', '--active', action='store_true')
        parser.add_argument('prefix', nargs='?', default=None)

        args = parser.parse_args(argv)

        self.path = os.path.expanduser('~/.config/ostbuild-prefix')
        if args.prefix is None and not args.active:
            if os.path.exists(self.path):
                f = open(self.path)
                print "%s" % (f.read().strip(), )
                f.close()
            else:
                log("No currently active prefix")
        elif args.prefix is not None and args.active:
            fatal("Can't specify -a with prefix")
        elif args.prefix is not None:
            self._set_prefix(args.prefix)
        else:
            assert args.active

            self.parse_active_branch()

            active_prefix = self.active_branch_contents['prefix']
            
            self._set_prefix(active_prefix)

builtins.register(OstbuildPrefix)
