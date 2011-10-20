# Post-installation hook for gdk-pixbuf.  -*- mode: sh -*-
# Corresponds to gdk-pixbuf/gdk-pixbuf/Makefile.am:install-data-hook
#
# Written by Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# IfExecutable: gdk-pixbuf-query-loaders
# RequiresChroot: true
# LiteralMatch: /gdk-pixbuf-2.0/2.10.0/loaders/

exec gdk-pixbuf-query-loaders --update-cache
