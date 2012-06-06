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

import os,sys,stat,subprocess,tempfile,re,shutil,time
import argparse
from StringIO import StringIO
import json

from . import builtins
from .ostbuildlog import log, fatal
from . import vcs
from .subprocess_helpers import run_sync, run_sync_get_output
from . import buildutil

class OstbuildGitMirror(builtins.Builtin):
    name = "git-mirror"
    short_description = "Update internal git mirror for one or more components"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--manifest')
        parser.add_argument('--src-snapshot')
        parser.add_argument('--start-at',
                            help="Start at the given component")
        parser.add_argument('--fetch-skip-secs', type=int, default=0,
                            help="Don't perform a fetch if we have done so in the last N seconds")
        parser.add_argument('--fetch', action='store_true',
                            help="Also do a git fetch for components")
        parser.add_argument('-k', '--keep-going', action='store_true',
                            help="Don't exit on fetch failures")
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        self.parse_config()
        if args.manifest:
            self.snapshot = json.load(open(args.manifest))
            components = map(lambda x: buildutil.resolve_component_meta(self.snapshot, x), self.snapshot['components'])
            self.snapshot['components'] = components
            self.snapshot['patches'] = buildutil.resolve_component_meta(self.snapshot, self.snapshot['patches'])
        else:
            self.parse_snapshot(args.prefix, args.src_snapshot)

        if len(args.components) == 0:
            components = []
            for component in self.snapshot['components']:
                components.append(component['name'])
            if 'patches' in self.snapshot:
                components.append(self.snapshot['patches']['name'])
            if args.start_at:
                idx = components.index(args.start_at)
                components = components[idx:]
        else:
            components = args.components

        for name in components:
            component = self.get_component(name)
            src = component['src']
            (keytype, uri) = vcs.parse_src_key(src)
            branch = component.get('branch')
            tag = component.get('tag')
            branch_or_tag = branch or tag
            mirrordir = vcs.ensure_vcs_mirror(self.mirrordir, keytype, uri, branch_or_tag)

            if not args.fetch:
                continue

            if tag is not None:
                log("Skipping fetch for %s at tag %s" % (name, tag))
                continue

            curtime = time.time()
            if args.fetch_skip_secs > 0:
                last_fetch_path = vcs.get_lastfetch_path(self.mirrordir, keytype, uri, branch_or_tag)
                try:
                    stbuf = os.stat(last_fetch_path)
                except OSError, e:
                    stbuf = None
                if stbuf is not None:
                    mtime = stbuf.st_mtime
                    delta = curtime - mtime
                    if delta < args.fetch_skip_secs:
                        log("Skipping fetch for %s updated in last %d seconds" % (name, delta))
                        continue

            log("Running git fetch for %s" % (name, ))
            vcs.fetch(self.mirrordir, keytype, uri, branch_or_tag, keep_going=args.keep_going)

builtins.register(OstbuildGitMirror)
