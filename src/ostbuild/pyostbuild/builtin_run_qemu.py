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

class OstbuildRunQemu(builtins.Builtin):
    name = "run-qemu"
    short_description = "Run QEMU image"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('target')

        args = parser.parse_args(argv)
        
        self.parse_config()
        
        helper = privileged_subproc.PrivilegedSubprocess()
        child_args = ['ostbuild', 'privhelper-run-qemu', args.target]
        helper.spawn_sync(child_args)
        
builtins.register(OstbuildRunQemu)
