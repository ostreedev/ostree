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

import os,sys,re,subprocess,tempfile,shutil
import argparse

from . import builtins
from .ostbuildlog import log, fatal

class OstbuildAutodiscoverMeta(builtins.Builtin):
    name = "autodiscover-meta"
    short_description = "Extract metadata from the current source directory"

    def execute(self, argv):
        parser = argparse.ArgumentParser(self.short_description)
        parser.add_argument('--meta')

        args = parser.parse_args(argv)

        KEYS = {}
        AUTODISCOVERED_KEYS = {}

        def _register_discover_func(key, func):
            if key not in AUTODISCOVERED_KEYS:
                AUTODISCOVERED_KEYS[key] = []
            AUTODISCOVERED_KEYS[key].append(func)

        _register_discover_func('NAME', self._discover_name_from_cwd)
        _register_discover_func('VERSION', self._discover_version_from_git)
        _register_discover_func('BRANCH', self._discover_branch_from_git)

        if args.meta:
            f = open(args.meta)
            for line in f.readlines():
                (k,v) = line.split('=', 1)
                KEYS[k.strip()] = v.strip()
            f.close()
            
        for (key,hooks) in AUTODISCOVERED_KEYS.iteritems():
            if key in KEYS:
                continue
            for func in hooks:
                    value = func()
            
                    if value is None:
                        continue
            
                    KEYS[key] = value
                    break
            
        for (key,value) in KEYS.iteritems():
            print "%s=%s" % (key, value)
        
    def _discover_name_from_cwd(self):
        return os.path.basename(os.getcwd())
        
    def _discover_version_from_git(self):
        if os.path.isdir('.git'):
            version = subprocess.check_output(['git', 'describe', '--long', '--abbrev=42', '--always'])
            return version.strip()
        return None
        
    def _discover_branch_from_git(self):
        if os.path.isdir('.git'):
            try:
                ref = subprocess.check_output(['git', 'symbolic-ref', 'HEAD'])
                return ref.replace('refs/heads/', '').strip()
            except subprocess.CalledProcessError, e:
                return None
        return None
    
builtins.register(OstbuildAutodiscoverMeta)
