#!/usr/bin/env gjs
//
// Copyright (C) 2013 Colin Walters <walters@verbum.org>
//
// SPDX-License-Identifier: LGPL-2.0+

const Gio = imports.gi.Gio;
const OSTree = imports.gi.OSTree;

function assertEquals(a, b) {
    if (a != b)
	throw new Error("assertion failed " + JSON.stringify(a) + " == " + JSON.stringify(b));
}

print('1..1')

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

// Write a ref and read it back
repo.prepare_transaction(null);
repo.transaction_set_refspec('someref', commit);
repo.commit_transaction(null, null);
let [,readCommit] = repo.resolve_rev('someref', false);
assertEquals(readCommit, commit);

// Delete a ref
repo.prepare_transaction(null);
repo.transaction_set_refspec('someref', null);
repo.commit_transaction(null, null);
[,readCommit] = repo.resolve_rev('someref', true);
assertEquals(readCommit, null);

print("ok test-core");
