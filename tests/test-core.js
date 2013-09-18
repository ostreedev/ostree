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

const Gio = imports.gi.Gio;
const OSTree = imports.gi.OSTree;

function assertEquals(a, b) {
    if (a != b)
	throw new Error("assertion failed " + JSON.stringify(a) + " == " + JSON.stringify(b));
}

let testDataDir = Gio.File.new_for_path('test-data');
testDataDir.make_directory(null);
testDataDir.get_child('some-file').replace_contents("hello world!", null, false, 0, null);

let repoPath = Gio.File.new_for_path('repo');
let repo = OSTree.Repo.new(repoPath);
repo.create(OSTree.RepoMode.ARCHIVE_Z2, null);

repo.open(null);

assertEquals(repo.get_mode(), OSTree.RepoMode.ARCHIVE_Z2);

repo.prepare_transaction(null);

let mtree = OSTree.MutableTree.new();
repo.write_directory_to_mtree(testDataDir, mtree, null, null);
let [,dirTree] = repo.write_mtree(mtree, null);
let [,commit] = repo.write_commit(null, 'Some subject', 'Some body', null, dirTree, null);
print("commit => " + commit);

repo.commit_transaction(null, null);

let [,root,checksum] = repo.read_commit(commit, null);
let child = root.get_child('some-file');
let info = child.query_info("standard::name,standard::type,standard::size", 0, null);
assertEquals(info.get_size(), 12);

print("test-core complete");
