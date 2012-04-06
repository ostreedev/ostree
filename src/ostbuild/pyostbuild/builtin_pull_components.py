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
        parser.add_argument('targets', nargs='*')

        args = parser.parse_args(argv)

        self.parse_active_branch()

        if len(args.targets) == 0:
            targets = [self.active_branch]
        else:
            targets = args.targets

        tree_contents_list = []
        for target in targets:
            tree_contents_path = os.path.join(self.ostree_dir, target, 'contents.json')
            tree_contents = json.load(open(tree_contents_path))
            tree_contents_list.append(tree_contents)
        revisions = set()
        for tree_contents in tree_contents_list:
            for content_item in tree_contents['contents']:
                revisions.add(content_item['ostree-revision'])
        args = ['ostree-pull', '--repo=' + self.repo]
        # FIXME FIXME - don't hardcode origin here
        args.append('gnome')
        for revision in revisions:
            args.append(revision)
        run_sync(args)
        
builtins.register(OstbuildPullComponents)
