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
from .subprocess_helpers import run_sync, run_sync_get_output
from . import ostbuildrc
from . import vcs
from . import jsondb
from . import buildutil
from . import kvfile
from . import odict

class OstbuildResolve(builtins.Builtin):
    name = "resolve"
    short_description = "Expand git revisions in source to exact targets"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--manifest', required=True,
                            help="Path to manifest file")
        parser.add_argument('--fetch-patches', action='store_true',
                            help="Git fetch the patches")
        parser.add_argument('--fetch', action='store_true',
                            help="Also perform a git fetch")
        parser.add_argument('components', nargs='*',
                            help="List of component names to git fetch")

        args = parser.parse_args(argv)
        self.args = args

        if len(args.components) > 0 and not args.fetch:
            fatal("Can't specify components without --fetch")
        
        self.parse_config()

        self.snapshot = json.load(open(args.manifest))
        self.prefix = self.snapshot['prefix']

        components = map(lambda x: buildutil.resolve_component_meta(self.snapshot, x), self.snapshot['components'])
        self.snapshot['components'] = components

        unique_component_names = set()
        for component in components:
            name = component['name']

            if name in unique_component_names:
                fatal("Duplicate component name '%s'" % (name, ))
            unique_component_names.add(name)

        global_patches_meta = buildutil.resolve_component_meta(self.snapshot, self.snapshot['patches'])
        self.snapshot['patches'] = global_patches_meta
        (keytype, uri) = vcs.parse_src_key(global_patches_meta['src'])
        mirrordir = vcs.ensure_vcs_mirror(self.mirrordir, keytype, uri, global_patches_meta['branch'])
        if args.fetch_patches:
            run_sync(['git', 'fetch'], cwd=mirrordir, log_initiation=False)

        git_mirror_args = ['ostbuild', 'git-mirror', '--manifest=' + args.manifest]
        if args.fetch:
            git_mirror_args.append('--fetch')
            git_mirror_args.extend(args.components)
        run_sync(git_mirror_args)

        patch_revision = buildutil.get_git_version_describe(mirrordir, global_patches_meta['branch'])
        global_patches_meta['revision'] = patch_revision

        for component in components:
            src = component['src']
            (keytype, uri) = vcs.parse_src_key(src)
            branch = component.get('branch')
            tag = component.get('tag')
            branch_or_tag = branch or tag
            mirrordir = vcs.ensure_vcs_mirror(self.mirrordir, keytype, uri, branch_or_tag)
            revision = buildutil.get_git_version_describe(mirrordir, branch_or_tag)
            component['revision'] = revision

        src_db = self.get_src_snapshot_db()
        path = src_db.store(self.snapshot)
        log("Source snapshot: %s" % (path, ))
        
builtins.register(OstbuildResolve)
