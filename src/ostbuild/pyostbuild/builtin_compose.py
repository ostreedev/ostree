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
import argparse
import time
import urlparse
import json
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from .subprocess_helpers import run_sync_monitor_log_file
from . import ostbuildrc
from . import buildutil
from . import fileutil
from . import kvfile
from . import odict
from . import vcs

class OstbuildCompose(builtins.Builtin):
    name = "compose"
    short_description = "Build complete trees from components"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _compose_one_target(self, bin_snapshot, target):
        components = bin_snapshot['component-revisions']
        base = target['base']
        base_name = 'bases/%s' % (base['name'], )
        base_revision = target['base']['ostree-revision']

        compose_rootdir = os.path.join(self.workdir, 'roots', target['name'])
        if os.path.isdir(compose_rootdir):
            shutil.rmtree(compose_rootdir)
        os.mkdir(compose_rootdir)

        compose_contents = [(base_revision, '/')]
        for tree_content in target['contents']:
            name = tree_content['name']
            rev = components[name]
            subtrees = tree_content['trees']
            for subpath in subtrees:
                compose_contents.append((rev, subpath))

        (fd, tmppath) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-compose-')
        f = os.fdopen(fd, 'w')
        for (branch, subpath) in compose_contents:
            f.write(branch)
            f.write('\0')
            f.write(subpath)
            f.write('\0')
        f.close()

        run_sync(['ostree', '--repo=' + self.repo,
                  'checkout', '--user-mode', '--no-triggers',
                  '--union', '--from-stdin', compose_rootdir],
                 stdin=open(tmppath))
        os.unlink(tmppath)

        contents_path = os.path.join(compose_rootdir, 'contents.json')
        f = open(contents_path, 'w')
        json.dump(bin_snapshot, f, indent=4, sort_keys=True)
        f.close()

        run_sync(['ostree', '--repo=' + self.repo,
                  'commit', '-b', target['name'], '-s', 'Compose',
                  '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                  '--skip-if-unchanged'], cwd=compose_rootdir)
        shutil.rmtree(compose_rootdir)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--bin-snapshot')

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()
        self.parse_bin_snapshot(args.prefix, args.bin_snapshot)
        
        log("Using binary snapshot: %s" % (os.path.basename(self.bin_snapshot_path), ))

        for target in self.bin_snapshot['targets']:
            log("Composing target %r from %u components" % (target['name'],
                                                            len(target['contents'])))
            self._compose_one_target(self.bin_snapshot, target)
        
builtins.register(OstbuildCompose)
