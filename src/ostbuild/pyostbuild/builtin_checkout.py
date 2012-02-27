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
import json
import urlparse
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from . import ostbuildrc
from . import buildutil
from . import odict
from . import vcs

class OstbuildCheckout(builtins.Builtin):
    name = "checkout"
    short_description = "Check out specified modules"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()

        build_manifest_path = os.path.join(self.workdir, 'snapshot.json')
        self.manifest = json.load(open(build_manifest_path))

        for component_name in args.components:
            found = False
            for component in self.manifest['components']:
                if component['name'] == component_name:
                    found = True
                    break
            if not found:
                fatal("Unknown component %r" % (component_name, ))
            (keytype, uri) = buildutil.parse_src_key(component['src'])
            checkoutdir = os.path.join(os.getcwd(), component['name'])
            component_src = vcs.get_vcs_checkout(self.mirrordir, keytype, uri, checkoutdir,
                                                 component['revision'],
                                                 overwrite=False)
            print "Checked out: %r" % (component_src, )
        
builtins.register(OstbuildCheckout)
