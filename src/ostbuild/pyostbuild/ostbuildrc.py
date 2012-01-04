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

import os,sys,ConfigParser

_config = None

def get():
    global _config
    if _config is None:
        configpath = os.path.expanduser('~/.config/ostbuild.cfg')
        parser = ConfigParser.RawConfigParser()
        parser.read([configpath])

        _config = {}
        for (k, v) in parser.items('global'):
            _config[k.strip()] = v.strip()
    return _config

def get_key(name, provided_args=None):
    config = get()
    if provided_args:
        v = provided_args.get(name)
        if v is not None:
            return v
    return config[name]
                                        
