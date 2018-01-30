#!/usr/bin/env gjs
//
// Copyright (C) 2013 Colin Walters <walters@verbum.org>
//
// SPDX-License-Identifier: LGPL-2.0+
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

print('1..1')

let testDataDir = Gio.File.new_for_path('test-data');
testDataDir.make_directory(null);
testDataDir.get_child('some-file').replace_contents("hello world!", null, false, 0, null);
testDataDir.get_child('another-file').replace_contents("hello world again!", null, false, 0, null);

let repoPath = Gio.File.new_for_path('repo');
let repo = OSTree.Repo.new(repoPath);
repo.create(OSTree.RepoMode.ARCHIVE_Z2, null);

repo.open(null);

let commitModifier = OSTree.RepoCommitModifier.new(OSTree.RepoCommitModifierFlags.GENERATE_SIZES, null);

assertEquals(repo.get_mode(), OSTree.RepoMode.ARCHIVE_Z2);

repo.prepare_transaction(null);

let mtree = OSTree.MutableTree.new();
repo.write_directory_to_mtree(testDataDir, mtree, commitModifier, null);
let [,dirTree] = repo.write_mtree(mtree, null);
let [,commit] = repo.write_commit(null, 'Some subject', 'Some body', null, dirTree, null);
print("commit => " + commit);

repo.commit_transaction(null, null);

// Test the sizes metadata
let [,commitVariant] = repo.load_variant(OSTree.ObjectType.COMMIT, commit);
let metadata = commitVariant.get_child_value(0);
let sizes = metadata.lookup_value('ostree.sizes', GLib.VariantType.new('aay'));
let nSizes = sizes.n_children();
assertEquals(nSizes, 2);
let expectedUncompressedSizes = [12, 18];
let foundExpectedUncompressedSizes = 0;
for (let i = 0; i < nSizes; i++) {
    let sizeEntry = sizes.get_child_value(i).deep_unpack();
    assertEquals(sizeEntry.length, 34);
    let compressedSize = sizeEntry[32];
    let uncompressedSize = sizeEntry[33];
    print("compressed = " + compressedSize);
    print("uncompressed = " + uncompressedSize);
    for (let j = 0; j < expectedUncompressedSizes.length; j++) {
	let expected = expectedUncompressedSizes[j];
	if (expected == uncompressedSize) {
	    print("Matched expected uncompressed size " + expected);
	    expectedUncompressedSizes.splice(j, 1);
	    break;
	}
    }
}
if (expectedUncompressedSizes.length > 0) {
    throw new Error("Failed to match expectedUncompressedSizes: " + JSON.stringify(expectedUncompressedSizes));
}

print("ok test-sizes");
