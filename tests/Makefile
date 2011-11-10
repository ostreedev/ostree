# Toplevel tests Makefile
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

TESTS = $(wildcard t[0-9][0-9][0-9][0-9]-*.sh)

all: tmpdir-lifecycle run-apache

tmpdir-lifecycle: tmpdir-lifecycle.c Makefile
	gcc $(CFLAGS) `pkg-config --cflags --libs gio-unix-2.0` -o $@ $<

run-apache: run-apache.c Makefile
	gcc $(CFLAGS) `pkg-config --cflags --libs gio-unix-2.0` -o $@ $<

check:
	@for test in $(TESTS); do \
	  echo $$test; \
	  ./$$test; \
	done

