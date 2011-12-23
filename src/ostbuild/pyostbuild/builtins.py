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

_all_builtins = {}

class Builtin(object):
    name = None
    short_description = None

    def execute(self, args):
        raise NotImplementedError()

def register(builtin):
    _all_builtins[builtin.name] = builtin

def get(name):
    return _all_builtins.get(name)()

def get_all():
    return _all_builtins.itervalues()
