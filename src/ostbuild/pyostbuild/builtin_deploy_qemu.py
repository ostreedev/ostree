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

import os,sys,subprocess,tempfile,re,shutil
import argparse
import time
import urlparse
import json
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from . import ostbuildrc
from . import privileged_subproc

class OstbuildDeployQemu(builtins.Builtin):
    name = "deploy-qemu"
    short_description = "Extract data from shadow repository to qemu"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--bin-snapshot')

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()
        self.parse_bin_snapshot(args.prefix, args.bin_snapshot)
        
        target_names = []
        for target in self.bin_snapshot['targets']:
            target_names.append(target['name'])

        helper = privileged_subproc.PrivilegedSubprocess()
        sys_repo = os.path.join(self.ostree_dir, 'repo')
        shadow_path = os.path.join(self.workdir, 'shadow-repo')
        helper.spawn_sync(['ostree', '--repo=' + sys_repo,
                           'pull-local', shadow_path])
        
builtins.register(OstbuildDeployRoot)
