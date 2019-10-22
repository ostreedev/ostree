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

function assertGreater(a, b) {
    if (a <= b)
	throw new Error("assertion failed " + JSON.stringify(a) + " > " + JSON.stringify(b));
}

function assertGreaterEquals(a, b) {
    if (a < b)
	throw new Error("assertion failed " + JSON.stringify(a) + " >= " + JSON.stringify(b));
}

// Adapted from _ostree_read_varuint64()
function readVarint(buffer) {
    let result = 0;
    let count = 0;
    let len = buffer.length;
    let cur;

    do {
        assertGreater(len, 0);
        cur = buffer[count];
        result = result | ((cur & 0x7F) << (7 * count));
        count++;
        len--;
    } while (cur & 0x80);

    return [result, count];
}

// There have been various bugs with byte array unpacking in GJS, so
// just do it manually.
function unpackByteArray(variant) {
    let array = [];
    let nBytes = variant.n_children();
    for (let i = 0; i < nBytes; i++) {
        array.push(variant.get_child_value(i).get_byte());
    }
    return array;
}

function validateSizes(repo, commit, expectedFiles) {
    let [,commitVariant] = repo.load_variant(OSTree.ObjectType.COMMIT, commit);
    let metadata = commitVariant.get_child_value(0);
    let sizes = metadata.lookup_value('ostree.sizes', GLib.VariantType.new('aay'));
    let nSizes = sizes.n_children();
    let expectedNSizes = Object.keys(expectedFiles).length
    assertEquals(nSizes, expectedNSizes);

    for (let i = 0; i < nSizes; i++) {
        let sizeEntry = sizes.get_child_value(i);
        assertGreaterEquals(sizeEntry.n_children(), 34);
        let entryBytes = unpackByteArray(sizeEntry);
        let checksumBytes = entryBytes.slice(0, 32);
        let checksumString = OSTree.checksum_from_bytes(checksumBytes);
        print("checksum = " + checksumString);

        // Read the sizes from the next 2 varints
        let remainingBytes = entryBytes.slice(32);
        assertGreaterEquals(remainingBytes.length, 2);
        let varintRead;
        let compressedSize;
        let uncompressedSize;
        [compressedSize, varintRead] = readVarint(remainingBytes);
        remainingBytes = remainingBytes.slice(varintRead);
        assertGreaterEquals(remainingBytes.length, 1);
        [uncompressedSize, varintRead] = readVarint(remainingBytes);
        remainingBytes = remainingBytes.slice(varintRead);
        assertEquals(remainingBytes.length, 0);
        print("compressed = " + compressedSize);
        print("uncompressed = " + uncompressedSize);

        if (!(checksumString in expectedFiles)) {
            throw new Error("Checksum " + checksumString + " not in " +
                            JSON.stringify(expectedFiles));
        }
        let expectedSizes = expectedFiles[checksumString];
        let expectedCompressedSize = expectedSizes[0];
        let expectedUncompressedSize = expectedSizes[1];
        if (compressedSize != expectedCompressedSize) {
            throw new Error("Compressed size " + compressedSize +
                            " for checksum " + checksumString +
                            " does not match expected " + expectedCompressedSize);
        }
        if (uncompressedSize != expectedUncompressedSize) {
            throw new Error("Uncompressed size " + uncompressedSize +
                            " for checksum " + checksumString +
                            " does not match expected " + expectedUncompressedSize);
        }
    }
}

print('1..2')

let testDataDir = Gio.File.new_for_path('test-data');
testDataDir.make_directory(null);
testDataDir.get_child('some-file').replace_contents("hello world!", null, false, 0, null);
testDataDir.get_child('another-file').replace_contents("hello world again!", null, false, 0, null);

let repoPath = Gio.File.new_for_path('repo');
let repo = OSTree.Repo.new(repoPath);
repo.create(OSTree.RepoMode.ARCHIVE_Z2, null);

repo.open(null);

let commitModifierFlags = (OSTree.RepoCommitModifierFlags.GENERATE_SIZES |
                           OSTree.RepoCommitModifierFlags.SKIP_XATTRS |
                           OSTree.RepoCommitModifierFlags.CANONICAL_PERMISSIONS);
let commitModifier = OSTree.RepoCommitModifier.new(commitModifierFlags, null);

assertEquals(repo.get_mode(), OSTree.RepoMode.ARCHIVE_Z2);

repo.prepare_transaction(null);

let mtree = OSTree.MutableTree.new();
repo.write_directory_to_mtree(testDataDir, mtree, commitModifier, null);
let [,dirTree] = repo.write_mtree(mtree, null);
let [,commit] = repo.write_commit(null, 'Some subject', 'Some body', null, dirTree, null);
print("commit => " + commit);

repo.commit_transaction(null);

// Test the sizes metadata
let expectedFiles = {
    'f5ee222a21e2c96edbd6f2543c4bc8a039f827be3823d04777c9ee187778f1ad': [54, 18],
    'd35bfc50864fca777dbeead3ba3689115b76674a093210316589b1fe5cc3ff4b': [48, 12],
};
validateSizes(repo, commit, expectedFiles);

print("ok test-sizes");

// Repeat the commit now that all the objects are cached and ensure the
// metadata is still correct
repo.prepare_transaction(null);
mtree = OSTree.MutableTree.new();
repo.write_directory_to_mtree(testDataDir, mtree, commitModifier, null);
[,dirTree] = repo.write_mtree(mtree, null);
[,commit] = repo.write_commit(null, 'Another subject', 'Another body', null, dirTree, null);
print("commit => " + commit);
repo.commit_transaction(null);

validateSizes(repo, commit, expectedFiles);

print("ok test-sizes repeated");
