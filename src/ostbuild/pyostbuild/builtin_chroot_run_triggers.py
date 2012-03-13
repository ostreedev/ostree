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

import os,sys,re,subprocess,tempfile,shutil
from StringIO import StringIO
import argparse
import json

from . import builtins
from . import buildutil
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync

class OstbuildChrootRunTriggers(builtins.Builtin):
    name = "chroot-run-triggers"
    short_description = "Run ostree-run-triggers inside a chroot"

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('root')
        
        args = parser.parse_args(argv)

        child_args = buildutil.get_base_user_chroot_args()
        child_args.extend(['--mount-proc', '/proc', 
                           '--mount-bind', '/dev', '/dev',
                           args.root,
                           '/usr/bin/ostree-run-triggers'])
        env_copy = dict(buildutil.BUILD_ENV)
        env_copy['PWD'] = '/' 
        run_sync(child_args, env=env_copy)
                       
builtins.register(OstbuildChrootRunTriggers)
