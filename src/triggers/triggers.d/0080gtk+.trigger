#!/bin/sh
# Post-installation hook for gtk icon cache.  -*- mode: sh -*-
#
# Written by Colin Walters <walters@verbum.org>
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

if test -x "$(which gtk-update-icon-cache 2>/dev/null)"; then
    for dir in /usr/share/icons/*; do
	if test -f $dir/index.theme; then
	    if ! gtk-update-icon-cache --quiet $dir; then
		echo "Failed to run gtk-update-icon-cache for $dir"
		exit 1
	    fi
	fi
    done
fi
