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

import os,sys,stat,subprocess,tempfile,re,shutil
import argparse
from StringIO import StringIO
import json

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from . import buildutil

class OstbuildQueryContent(builtins.Builtin):
    name = "query-content"
    short_description = "Output metadata from a component"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--branch', required=True)
        parser.add_argument('--component')

        args = parser.parse_args(argv)
        self.args = args
        self.parse_config()

        contents_json_text = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                  'cat', args.branch, 'contents.json'])
        
        if args.component is None:
            sys.stdout.write(contents_json_text)
        else:
            contents = json.loads(contents_json_text)
            contents_list = contents['contents']
            found = False
            for content in contents_list:
                if content['name'] != args.component:
                    found = True
                    break
            if not found:
                fatal("Unknown component '%s'" % (args.component, ))
            ostbuildmeta_json = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                     'cat', content['ostree-revision'],
                                                     '/_ostbuild-meta.json'])
            sys.stdout.write(ostbuildmeta_json)

builtins.register(OstbuildQueryContent)
