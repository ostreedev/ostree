#!/usr/bin/env gjs
//
// Copyright (C) 2013 Colin Walters <walters@verbum.org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the
// Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;

const OSTree = imports.gi.OSTree;

function assertEquals(a, b) {
    if (a != b)
	throw new Error("assertion failed " + JSON.stringify(a) + " == " + JSON.stringify(b));
}

function libtestExec(shellCode) {
    let testdatadir = GLib.getenv("TESTDATADIR");
    let libtestPath = GLib.build_filenamev([testdatadir, 'libtest.sh'])
    let cmdline = 'bash -c ' + GLib.shell_quote('. ' + GLib.shell_quote(libtestPath) + '; ' + shellCode);
    print("shellcode=" +cmdline);
    let [,stdout,stderr,estatus] = GLib.spawn_command_line_sync(cmdline);
    print(stderr);
    GLib.spawn_check_exit_status(estatus);
}

libtestExec('setup_os_repository archive-z2 syslinux');

let upstreamRepo = OSTree.Repo.new(Gio.File.new_for_path('testos-repo'));
upstreamRepo.open(null);

let [,rev] = upstreamRepo.resolve_rev('testos/buildmaster/x86_64-runtime', false);

print("testos => " + rev);

let sysroot = OSTree.Sysroot.new(Gio.File.new_for_path('sysroot'));
sysroot.load(null);
let deployments = sysroot.get_deployments();
assertEquals(deployments.length, 0);

