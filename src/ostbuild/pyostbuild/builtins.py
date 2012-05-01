#!/usr/bin/python
#
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

import os
import sys
import stat
import argparse
import json

from . import ostbuildrc
from . import fileutil
from . import jsondb
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output

_all_builtins = {}

class Builtin(object):
    name = None
    short_description = None

    def __init__(self):
        self._meta_cache = {}
        self.prefix = None
        self.manifest = None
        self.snapshot = None
        self.bin_snapshot = None
        self.repo = None
        self.ostree_dir = self._find_ostree_dir()
        (self.active_branch, self.active_branch_checksum) = self._find_active_branch()
        self._src_snapshots = None
        self._bin_snapshots = None

    def _find_ostree_dir(self):
        for path in ['/ostree', '/sysroot/ostree']:
            if os.path.isdir(path):
                return path
        return None
        
    def _find_active_branch(self):
        if self.ostree_dir is None:
            return (None, None)
        current_path = os.path.join(self.ostree_dir, 'current')
        while True:
            try:
                target = os.path.join(self.ostree_dir, current_path)
                stbuf = os.lstat(target)
            except OSError, e:
                current_path = None
                break
            if not stat.S_ISLNK(stbuf.st_mode):
                break
            current_path = os.readlink(target)
        if current_path is not None:
            basename = os.path.basename(current_path)
            return basename.rsplit('-', 1)
        else:
            return (None, None)

    def get_component_from_cwd(self):
        cwd = os.getcwd()
        parent = os.path.dirname(cwd)
        parentparent = os.path.dirname(parent)
        return '%s/%s/%s' % tuple(map(os.path.basename, [parentparent, parent, cwd]))

    def parse_config(self):
        self.ostbuildrc = ostbuildrc

        self.mirrordir = ostbuildrc.get_key('mirrordir')
        if not os.path.isdir(self.mirrordir):
            fatal("Specified mirrordir '%s' is not a directory" % (self.mirrordir, ))
        self.workdir = ostbuildrc.get_key('workdir')
        if not os.path.isdir(self.workdir):
            fatal("Specified workdir '%s' is not a directory" % (self.workdir, ))

        self.snapshot_dir = os.path.join(self.workdir, 'snapshots')
        self.patchdir = os.path.join(self.workdir, 'patches')

    def load_bin_snapshot_from_path(self, path):
        self.bin_snapshot_path = os.path.join(path, 'contents.json')
        self.bin_snapshot = json.load(open(self.bin_snapshot_path))
        bin_ver = self.bin_snapshot['00ostree-bin-snapshot-version']
        if bin_ver != 0:
            fatal("Unhandled 00ostree-bin-snapshot-version \"%d\", expected 0", bin_ver)

    def load_bin_snapshot_from_current(self):
        if self.ostree_dir is None:
            fatal("/ostree directory not found")
        repo_path = os.path.join(self.ostree_dir, 'repo')
        if not os.path.isdir(repo_path):
            fatal("Repository '%s' doesn't exist" % (repo_path, ))
        self.repo = repo_path
        if self.active_branch is None:
            fatal("No \"current\" link found")
        tree_path = os.path.join(self.ostree_dir, self.active_branch)
        self.load_bin_snapshot_from_path(tree_path)

    def get_component_snapshot(self, name):
        found = False
        for content in self.active_branch_contents['contents']:
            if content['name'] == name:
                found = True
                break
        if not found:
            fatal("Unknown component '%s'" % (name, ))
        return content

    def get_component_meta_from_revision(self, revision):
        text = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                    'cat', revision,
                                    '/_ostbuild-meta.json'])
        return json.loads(text)

    def get_component_meta(self, name):
        assert self.repo is not None

        if self.snapshot is not None:
            return self.snapshot['components'][name]

        meta = self._meta_cache.get(name)
        if meta is None:
            content = self.get_component_snapshot(name)
            meta = self.get_component_meta_from_revision(content['ostree-revision'])
            self._meta_cache[name] = meta
        return meta

    def get_prefix(self):
        if self.prefix is None:
            path = os.path.expanduser('~/.config/ostbuild-prefix')
            if not os.path.exists(path):
                fatal("No prefix set; use \"ostbuild prefix\" to set one")
            f = open(path)
            self.prefix = f.read().strip()
            f.close()
        return self.prefix

    def create_db(self, dbsuffix, prefix=None):
        if prefix is None:
            target_prefix = self.get_prefix()
        else:
            target_prefix = prefix
        name = '%s-%s' % (target_prefix, dbsuffix)
        fileutil.ensure_dir(self.snapshot_dir)
        return jsondb.JsonDB(self.snapshot_dir, prefix=name)

    def get_src_snapshot_db(self):
        if self._src_snapshots is None:
            self._src_snapshots = self.create_db('src-snapshot')
        return self._src_snapshots

    def get_bin_snapshot_db(self):
        if self._bin_snapshots is None:
            self._bin_snapshots = self.create_db('bin-snapshot')
        return self._bin_snapshots

    def parse_snapshot(self, prefix, path):
        if prefix is not None:
            self.prefix = prefix
        self.repo = ostbuildrc.get_key('repo')
        if path is None:
            latest_path = self.get_src_snapshot_db().get_latest_path()
            if latest_path is None:
                raise Exception("No source snapshot found for prefix %r" % (self.prefix, ))
            self.snapshot_path = latest_path
        else:
            self.snapshot_path = path
        self.snapshot = json.load(open(self.snapshot_path))
        src_ver = self.snapshot['00ostree-src-snapshot-version']
        if src_ver != 0:
            fatal("Unhandled 00ostree-src-snapshot-version \"%d\", expected 0", src_ver)

    def parse_bin_snapshot(self, prefix, path):
        if prefix is not None:
            self.prefix = prefix
        self.repo = ostbuildrc.get_key('repo')
        if path is None:
            latest_path = self.get_bin_snapshot_db().get_latest_path()
            if latest_path is None:
                raise Exception("No binary snapshot found for prefix %r" % (self.prefix, ))
            self.bin_snapshot_path = latest_path
        else:
            self.bin_snapshot_path = path
        self.bin_snapshot = json.load(open(self.bin_snapshot_path))
        bin_ver = self.bin_snapshot['00ostree-bin-snapshot-version']
        if bin_ver != 0:
            fatal("Unhandled 00ostree-bin-snapshot-version \"%d\", expected 0", bin_ver)

    def execute(self, args):
        raise NotImplementedError()

def register(builtin):
    _all_builtins[builtin.name] = builtin

def get(name):
    builtin = _all_builtins.get(name)
    if builtin is not None:
        return builtin()
    return None

def get_all():
    return sorted(_all_builtins.itervalues(), lambda a, b: cmp(a.name, b.name))
