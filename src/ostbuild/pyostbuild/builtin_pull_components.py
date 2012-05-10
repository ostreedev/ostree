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

import os,sys,subprocess,tempfile,re,shutil
import copy
import argparse
import json
import time
import urlparse
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from . import ostbuildrc
from . import buildutil
from .subprocess_helpers import run_sync, run_sync_get_output
from . import kvfile
from . import odict

class OstbuildPullComponents(builtins.Builtin):
    name = "pull-components"
    short_description = "Download the component data for active branch"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('origin')
        parser.add_argument('targets', nargs='*')
        parser.add_argument('--prefix')
        parser.add_argument('--bin-snapshot')

        args = parser.parse_args(argv)
        self.parse_config()
        self.parse_bin_snapshot(args.prefix, args.bin_snapshot)

        child_args = ['ostree-pull', '--repo=' + self.repo, '--prefer-loose',
                      args.origin]
        for component,revision in self.bin_snapshot['components'].iteritems():
            child_args.append(revision)
        run_sync(child_args)
        
builtins.register(OstbuildPullComponents)
