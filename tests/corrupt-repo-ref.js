#!/usr/bin/env gjs
//
// Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

let repoPathArg = ARGV[0];
let refToCorrupt = ARGV[1];

let repo = OSTree.Repo.new(Gio.File.new_for_path(repoPathArg));

repo.open(null);

function listObjectChecksumsRecurse(dir, allObjects) {
    dir.ensure_resolved();
    allObjects[dir.tree_get_contents_checksum() + '.dirtree'] = true;
    allObjects[dir.get_checksum() + '.dirmeta'] = true;
    let e = dir.enumerate_children('standard::name,standard::type', 0, null);
    let info;
    while ((info = e.next_file(null)) != null) {
	let child = e.get_child(info);
	child.ensure_resolved();
	print(info.get_name() + " is " + info.get_file_type());
	if (info.get_file_type() == Gio.FileType.DIRECTORY) {
	    listObjectChecksumsRecurse(child, allObjects);
	} else {
	    allObjects[child.get_checksum() + '.filez'] = true;
	}
    } 
    e.close(null);
} 

let [,commit] = repo.resolve_rev(refToCorrupt, false);
let [,root,commit] = repo.read_commit(refToCorrupt, null);
let allObjects = {};
allObjects[commit + '.commit'] = true;
listObjectChecksumsRecurse(root, allObjects);
let i = 0;
for (let v in allObjects) 
    i++;
print("commit " + commit + " refers to " + i + " objects");
let offset = GLib.random_int_range(0, i);
let objectToCorrupt = null;
for (let v in allObjects) {
    if (offset <= 0) {
	objectToCorrupt = v;
	break;
    }
    offset--;
}
print("Choosing " + objectToCorrupt + " to corrupt");

let loosePath = repo.get_path().resolve_relative_path('objects/' + objectToCorrupt.substring(0, 2) + "/" + objectToCorrupt.substring(2));

let iostream = loosePath.open_readwrite(null);
let info = iostream.query_info('standard::size', null);
let size = info.get_size();
let datain = Gio.DataInputStream.new(iostream.get_input_stream());
let dataout = Gio.DataOutputStream.new(iostream.get_output_stream());
let bytesToChange = 10;
let status = "";
var bytesChanged = {}
for (i = 0; i < bytesToChange; i++) {
    let byteOffsetToCorrupt;
    do {
        byteOffsetToCorrupt = GLib.random_int_range(0, size);
    } while (byteOffsetToCorrupt in bytesChanged);
    iostream.seek(byteOffsetToCorrupt, GLib.SeekType.SET, null);
    let inbyte = datain.read_byte(null);
    let outbyte = (inbyte + 1) % 255;
    dataout.put_byte(outbyte, null);
    bytesChanged[byteOffsetToCorrupt] = byteOffsetToCorrupt;
    status += "Changed byte offset " + byteOffsetToCorrupt + " from " + inbyte + " to " + outbyte + "\n";
}
dataout.flush(null);
iostream.close(null);

print(status);
let successFile = Gio.File.new_for_path('corrupted-status.txt');
successFile.replace_contents(status, null, false, 0, null);
