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

# This hack is because we want people to be able to pass None
# for "default", but still distinguish default=None from default
# not passed.
_default_not_supplied = object()
def get_key(name, provided_args=None, default=_default_not_supplied):
    global _default_not_supplied
    config = get()
    if provided_args:
        v = provided_args.get(name)
        if v is not None:
            return v
    if default is _default_not_supplied:
        # Possibly throw a KeyError
        return config[name]
    value = config.get(name, _default_not_supplied)
    if value is _default_not_supplied:
        return default
    return value
                                        
