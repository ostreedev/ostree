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
// License along with this library. If not, see <https://www.gnu.org/licenses/>.

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

function validateSizes(repo, commit, expectedObjects) {
    let [,commitVariant] = repo.load_variant(OSTree.ObjectType.COMMIT, commit);
    let metadata = commitVariant.get_child_value(0);
    let sizes = metadata.lookup_value('ostree.sizes', GLib.VariantType.new('aay'));
    let nObjects = sizes.n_children();
    let expectedNObjects = Object.keys(expectedObjects).length
    assertEquals(nObjects, expectedNObjects);

    for (let i = 0; i < nObjects; i++) {
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
        assertEquals(remainingBytes.length, 1);
        let objectType = remainingBytes[0];
        let objectTypeString = OSTree.object_type_to_string(objectType);
        print("compressed = " + compressedSize);
        print("uncompressed = " + uncompressedSize);
        print("objtype = " + objectTypeString + " (" + objectType + ")");
        let objectName = OSTree.object_to_string(checksumString, objectType);
        print("object = " + objectName);

        if (!(objectName in expectedObjects)) {
            throw new Error("Object " + objectName + " not in " +
                            JSON.stringify(expectedObjects));
        }
        let expectedSizes = expectedObjects[objectName];
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

print('1..3')

let testDataDir = Gio.File.new_for_path('test-data');
testDataDir.make_directory(null);
testDataDir.get_child('some-file').replace_contents("hello world!", null, false, 0, null);
testDataDir.get_child('some-file').copy(testDataDir.get_child('duplicate-file'),
                                        Gio.FileCopyFlags.OVERWRITE,
                                        null, null);
testDataDir.get_child('link-file').make_symbolic_link('some-file', null);
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

// Test the sizes metadata. The key is the object and the value is an
// array of compressed size and uncompressed size.
let expectedObjects = {
    'f5ee222a21e2c96edbd6f2543c4bc8a039f827be3823d04777c9ee187778f1ad.file': [
        54, 18
    ],
    'd35bfc50864fca777dbeead3ba3689115b76674a093210316589b1fe5cc3ff4b.file': [
        48, 12
    ],
    '8322876a078e79d8c960b8b4658fe77e7b2f878f8a6cf89dbb87c6becc8128a0.file': [
        43, 9
    ],
    '1c77033ca06eae77ed99cb26472969964314ffd5b4e4c0fd3ff6ec4265c86e51.dirtree': [
        185, 185
    ],
    '446a0ef11b7cc167f3b603e585c7eeeeb675faa412d5ec73f62988eb0b6c5488.dirmeta': [
        12, 12
    ],
};
validateSizes(repo, commit, expectedObjects);

print("ok test-sizes");

// Remove a file to make sure that metadata is not reused from the
// previous commit. Remove that file from the expected metadata and
// replace the dirtree object.
testDataDir.get_child('another-file').delete(null);
delete expectedObjects['f5ee222a21e2c96edbd6f2543c4bc8a039f827be3823d04777c9ee187778f1ad.file'];
delete expectedObjects['1c77033ca06eae77ed99cb26472969964314ffd5b4e4c0fd3ff6ec4265c86e51.dirtree'];
expectedObjects['a384660cc18ffdb60296961dde9a2d6f78f4fec095165652cb53aa81f6dc7539.dirtree'] = [
    138, 138
];

repo.prepare_transaction(null);
mtree = OSTree.MutableTree.new();
repo.write_directory_to_mtree(testDataDir, mtree, commitModifier, null);
[,dirTree] = repo.write_mtree(mtree, null);
[,commit] = repo.write_commit(null, 'Some subject', 'Some body', null, dirTree, null);
print("commit => " + commit);
repo.commit_transaction(null);

validateSizes(repo, commit, expectedObjects);

print("ok test-sizes file deleted");

// Repeat the commit now that all the objects are cached and ensure the
// metadata is still correct
repo.prepare_transaction(null);
mtree = OSTree.MutableTree.new();
repo.write_directory_to_mtree(testDataDir, mtree, commitModifier, null);
[,dirTree] = repo.write_mtree(mtree, null);
[,commit] = repo.write_commit(null, 'Another subject', 'Another body', null, dirTree, null);
print("commit => " + commit);
repo.commit_transaction(null);

validateSizes(repo, commit, expectedObjects);

print("ok test-sizes repeated");
