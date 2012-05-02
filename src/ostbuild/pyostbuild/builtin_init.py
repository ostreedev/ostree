# Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

import os,sys,stat,subprocess,tempfile,re,shutil
from StringIO import StringIO
import json
import select,time
import argparse

from . import builtins
from . import ostbuildrc
from .ostbuildlog import log, fatal
from . import fileutil
from .subprocess_helpers import run_sync, run_sync_get_output

class OstbuildInit(builtins.Builtin):
    name = "init"
    short_description = "Initialize working state"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)

        args = parser.parse_args(argv)
        
        mirrordir = os.path.expanduser(ostbuildrc.get_key('mirrordir'))
        fileutil.ensure_dir(mirrordir)
        workdir = os.path.expanduser(ostbuildrc.get_key('workdir'))
        fileutil.ensure_dir(workdir)

        self.parse_config()

        path = os.path.join(self.workdir, 'shadow-repo')
        fileutil.ensure_dir(path)
        if os.path.isdir(os.path.join(path, 'objects')):
            log("note: shadow repository '%s' already exists" % (path, ))
        else:
            run_sync(['ostree', '--repo=' + path, 'init', '--archive'])
            run_sync(['ostree', '--repo=' + path, 'config', 'set', 'core.parent', '/ostree/repo'])
            log("Created shadow repository: %s" % (path, ))
    
builtins.register(OstbuildInit)
