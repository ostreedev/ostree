#!/usr/bin/python

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

# ostbuild-compile-one-make wraps systems that implement the GNOME build API:
# http://people.gnome.org/~walters/docs/build-api.txt

import os,sys,subprocess,tempfile,re
import argparse

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync
from . import buildutil

class OstbuildCommitArtifacts(builtins.Builtin):
    name = "commit-artifacts"
    short_description = "Commit artifacts to their corresponding repository branches"

    def execute(self, argv):

        parser = argparse.ArgumentParser(self.short_description)
        parser.add_argument('--repo')
        parser.add_argument('artifacts', nargs='+')

        args = parser.parse_args(argv)

        if args.repo is None:
            fatal("--repo must be specified")

        for arg in args.artifacts:
            basename = os.path.basename(arg)
            parsed = buildutil.parse_artifact_name(basename)
    
            branch_name = buildutil.branch_name_for_artifact(parsed)

            run_sync(['ostree', '--repo=' + args.repo,
                      'commit', '-b', branch_name, '-s', 'Build ' + parsed['version'],
                     '--add-metadata-string=ostbuild-buildroot-version=' + parsed['buildroot_version'],
                     '--add-metadata-string=ostbuild-artifact-version=' + parsed['version'],
                     '--skip-if-unchanged', '--tar-autocreate-parents', '--tree=tar=' + arg])
                     
builtins.register(OstbuildCommitArtifacts)
