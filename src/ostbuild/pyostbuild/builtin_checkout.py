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
from . import fileutil
from . import odict
from . import vcs

class OstbuildCheckout(builtins.Builtin):
    name = "checkout"
    short_description = "Check out specified modules"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--overwrite', action='store_true')
        parser.add_argument('--prefix')
        parser.add_argument('--patches-path')
        parser.add_argument('--snapshot')
        parser.add_argument('--checkoutdir')
        parser.add_argument('-a', '--active-tree', action='store_true')
        parser.add_argument('--clean', action='store_true')
        parser.add_argument('component') 

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()

        if args.active_tree:
            self.parse_active_branch()
        else:
            self.parse_snapshot(args.prefix, args.snapshot)

        component_name = args.component

        found = False
        component = self.get_expanded_component(component_name)
        (keytype, uri) = buildutil.parse_src_key(component['src'])

        is_local = (keytype == 'local')

        if is_local:
            if args.checkoutdir:
                checkoutdir = args.checkoutdir
                # Kind of a hack, but...
                if os.path.islink(checkoutdir):
                    os.unlink(checkoutdir)
                if args.overwrite and os.path.isdir(checkoutdir):
                    shutil.rmtree(checkoutdir)
                os.symlink(uri, checkoutdir)
            else:
                checkoutdir = uri
        else:
            if args.checkoutdir:
                checkoutdir = args.checkoutdir
            else:
                checkoutdir = os.path.join(os.getcwd(), component_name)
                fileutil.ensure_parent_dir(checkoutdir)
            vcs.get_vcs_checkout(self.mirrordir, keytype, uri, checkoutdir,
                                 component['revision'],
                                 overwrite=args.overwrite)

        if args.clean:
            if is_local:
                log("note: ignoring --clean argument due to \"local:\" specification")
            else:
                vcs.clean(keytype, checkoutdir)

        patches = component.get('patches')
        if patches is not None:
            if self.args.patches_path:
                (patches_keytype, patches_uri) = ('local', self.args.patches_path)
            else:
                (patches_keytype, patches_uri) = buildutil.parse_src_key(patches['src'])
            if patches_keytype == 'git':
                patches_mirror = buildutil.get_mirrordir(self.mirrordir, patches_keytype, patches_uri)
                vcs.get_vcs_checkout(self.mirrordir, patches_keytype, patches_uri,
                                     self.patchdir, patches['branch'],
                                     overwrite=True)
                patchdir = self.patchdir
            else:
                patchdir = patches_uri

            patch_subdir = patches.get('subdir', None)
            if patch_subdir is not None:
                patchdir = os.path.join(patchdir, patch_subdir)
            else:
                patchdir = self.patchdir
            for patch in patches['files']:
                patch_path = os.path.join(patchdir, patch)
                run_sync(['git', 'am', '--ignore-date', '-3', patch_path], cwd=checkoutdir)

        metadata_path = os.path.join(checkoutdir, '_ostbuild-meta.json')
        f = open(metadata_path, 'w')
        json.dump(component, f, indent=4, sort_keys=True)
        f.close()
        
        log("Checked out: %r" % (checkoutdir, ))
        
builtins.register(OstbuildCheckout)
