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

import os,sys,re,subprocess,tempfile,shutil
from StringIO import StringIO
import argparse
import time
import json
import hashlib

from . import builtins
from . import buildutil
from . import fileutil
from . import ostbuildrc
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output

class OstbuildChrootCompileOne(builtins.Builtin):
    name = "chroot-compile-one"
    short_description = "Build artifacts from the current source directory in a chroot"

    def _resolve_refs(self, refs):
        if len(refs) == 0:
            return []
        args = ['ostree', '--repo=' + self.repo, 'rev-parse']
        args.extend(refs)
        output = run_sync_get_output(args)
        return output.split('\n')

    def _compose_buildroot(self, component_name, architecture):
        starttime = time.time()

        rootdir_prefix = os.path.join(self.workdir, 'roots')
        rootdir = os.path.join(rootdir_prefix, component_name)
        fileutil.ensure_parent_dir(rootdir)

        # Clean up any leftover root dir
        rootdir_tmp = rootdir + '.tmp'
        if os.path.isdir(rootdir_tmp):
            shutil.rmtree(rootdir_tmp)

        components = self.snapshot['components']
        component = None
        build_dependencies = []
        for component in components:
            if component['name'] == component_name:
                break
            build_dependencies.append(component)

        ref_to_rev = {}

        prefix = self.snapshot['prefix']

        arch_buildroot_name = 'bases/%s/%s-%s-devel' % (self.snapshot['base']['name'],
                                                        prefix,
                                                        architecture)

        arch_buildroot_rev = run_sync_get_output(['ostree', '--repo=' + self.repo, 'rev-parse',
                                                  arch_buildroot_name]).strip()

        ref_to_rev[arch_buildroot_name] = arch_buildroot_rev
        checkout_trees = [(arch_buildroot_name, '/')]
        refs_to_resolve = []
        for dependency in build_dependencies:
            buildname = 'components/%s/%s/%s' % (prefix, dependency['name'], architecture)
            refs_to_resolve.append(buildname)
            checkout_trees.append((buildname, '/runtime'))
            checkout_trees.append((buildname, '/devel'))

        resolved_refs = self._resolve_refs(refs_to_resolve)
        for ref,rev in zip(refs_to_resolve, resolved_refs):
            ref_to_rev[ref] = rev

        sha = hashlib.sha256()

        (fd, tmppath) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-buildroot-')
        f = os.fdopen(fd, 'w')
        for (branch, subpath) in checkout_trees:
            f.write(ref_to_rev[branch])
            f.write('\0')
            f.write(subpath)
            f.write('\0')
        f.close()

        f = open(tmppath)
        buf = f.read(8192)
        while buf != '':
            sha.update(buf)
            buf = f.read(8192)
        f.close()

        new_root_cacheid = sha.hexdigest()

        rootdir_cache_path = os.path.join(rootdir_prefix, component_name + '.cacheid')

        if os.path.isdir(rootdir):
            if os.path.isfile(rootdir_cache_path):
                f = open(rootdir_cache_path)
                prev_cache_id = f.read().strip()
                f.close()
                if prev_cache_id == new_root_cacheid:
                    log("Reusing previous buildroot")
                    os.unlink(tmppath)
                    return rootdir
                else:
                    log("New buildroot differs from previous")

            shutil.rmtree(rootdir)

        os.mkdir(rootdir_tmp)

        if len(checkout_trees) > 0:
            log("composing buildroot from %d parents (last: %r)" % (len(checkout_trees),
                                                                    checkout_trees[-1][0]))

        run_sync(['ostree', '--repo=' + self.repo,
                  'checkout', '--user-mode', '--union',
                  '--from-file=' + tmppath, rootdir_tmp])

        os.unlink(tmppath);

        builddir_tmp = os.path.join(rootdir_tmp, 'ostbuild')
        os.mkdir(builddir_tmp)
        os.mkdir(os.path.join(builddir_tmp, 'source'))
        os.mkdir(os.path.join(builddir_tmp, 'results'))
        os.rename(rootdir_tmp, rootdir)

        f = open(rootdir_cache_path, 'w')
        f.write(new_root_cacheid)
        f.write('\n')
        f.close()

        endtime = time.time()
        log("Composed buildroot; %d seconds elapsed" % (int(endtime - starttime),))

        return rootdir

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--snapshot', required=True)
        parser.add_argument('--name')
        parser.add_argument('--resultdir')
        parser.add_argument('--arch', required=True)
        parser.add_argument('--debug-shell', action='store_true')
        
        args = parser.parse_args(argv)

        self.parse_config()
        self.parse_snapshot(args.prefix, args.snapshot)

        if args.name:
            component_name = args.name
        else:
            component_name = self.get_component_from_cwd()

        component = self.get_expanded_component(component_name)

        workdir = self.workdir
            
        log("Using working directory: %s" % (workdir, ))
        
        child_tmpdir=os.path.join(workdir, 'tmp')
        if os.path.isdir(child_tmpdir):
            log("Cleaning up previous tmpdir: %r" % (child_tmpdir, ))
            shutil.rmtree(child_tmpdir)
        fileutil.ensure_dir(child_tmpdir)

        resultdir = args.resultdir
        
        rootdir = self._compose_buildroot(component_name, args.arch)

        log("Checked out buildroot: %s" % (rootdir, ))
        
        sourcedir=os.path.join(rootdir, 'ostbuild', 'source', component_name)
        fileutil.ensure_dir(sourcedir)
        
        output_metadata = open('_ostbuild-meta.json', 'w')
        json.dump(component, output_metadata, indent=4, sort_keys=True)
        output_metadata.close()
        
        chroot_sourcedir = os.path.join('/ostbuild', 'source', component_name)

        child_args = buildutil.get_base_user_chroot_args()
        child_args.extend([
                '--mount-readonly', '/',
                '--mount-proc', '/proc', 
                '--mount-bind', '/dev', '/dev',
                '--mount-bind', child_tmpdir, '/tmp',
                '--mount-bind', os.getcwd(), chroot_sourcedir,
                '--mount-bind', resultdir, '/ostbuild/results',
                '--chdir', chroot_sourcedir])
        if args.debug_shell:
            child_args.extend([rootdir, '/bin/sh'])
        else:
            child_args.extend([rootdir, '/usr/bin/ostbuild',
                               'compile-one',
                               '--ostbuild-resultdir=/ostbuild/results',
                               '--ostbuild-meta=_ostbuild-meta.json'])
        env_copy = dict(buildutil.BUILD_ENV)
        env_copy['PWD'] = chroot_sourcedir
        run_sync(child_args, env=env_copy, keep_stdin=args.debug_shell)

        recorded_meta_path = os.path.join(resultdir, '_ostbuild-meta.json')
        recorded_meta_f = open(recorded_meta_path, 'w')
        json.dump(component, recorded_meta_f, indent=4, sort_keys=True)
        recorded_meta_f.close()
        
builtins.register(OstbuildChrootCompileOne)
