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

import os,sys,stat,subprocess,tempfile,re,shutil
from StringIO import StringIO
import json
import select,time
import argparse

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output

class OstbuildModifySnapshot(builtins.Builtin):
    name = "modify-snapshot"
    short_description = "Change the current source snapshot"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--src-snapshot')

        args = parser.parse_args(argv)
        
        self.parse_config()
        self.parse_snapshot(args.prefix, args.src_snapshot)

        component_name = self.get_component_from_cwd()
        current_meta = self.get_component_meta(component_name)
       
        new_snapshot = dict(self.snapshot)
        new_meta = dict(current_meta)
        if 'patches' in new_meta:
            del new_meta['patches']
        new_meta['src'] = "dirty-git:%s" % (os.getcwd(), )
        new_meta['revision'] = run_sync_get_output(['git', 'rev-parse', 'HEAD'])

        new_snapshot['components'][component_name] = new_meta

        db = self.get_src_snapshot_db()
        path = db.store(new_snapshot)
        log("Replaced %s with %s %s" % (component_name, new_meta['src'],
                                        new_meta['revision']))
        log("New source snapshot: %s" % (path, ))
    
builtins.register(OstbuildModifySnapshot)
