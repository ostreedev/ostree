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
import argparse

from . import ostbuildrc
from .ostbuildlog import log, fatal

_all_builtins = {}

class Builtin(object):
    name = None
    short_description = None

    def parse_config(self):
        self.ostbuildrc = ostbuildrc
        self.repo = ostbuildrc.get_key('repo')
        self.mirrordir = ostbuildrc.get_key('mirrordir')
        if not os.path.isdir(self.mirrordir):
            fatal("Specified mirrordir '%s' is not a directory" % (self.mirrordir, ))
        self.workdir = ostbuildrc.get_key('workdir')
        if not os.path.isdir(self.workdir):
            fatal("Specified workdir '%s' is not a directory" % (self.workdir, ))
        self.patchdir = os.path.join(self.workdir, 'patches')

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
    return _all_builtins.itervalues()
