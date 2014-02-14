#!/usr/bin/env gjs
//
// Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

// PURPOSE: This script runs in an infinite loop; it alternates
// upgrading and downgrading.  The idea is that a parent test process
// could watch the output, and assert that the system is in a
// consistent state if this script is killed and restarted. randomly.

const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;

const OSTree = imports.gi.OSTree;

let sysroot = OSTree.Sysroot.new_default();
sysroot.load(null);
let [,repo] = sysroot.get_repo(null);
let deployments = sysroot.get_deployments();
if (deployments.length == 0)
    throw new Error("No deployments");

let firstDeployment = deployments[0];
let startingRevision = firstDeployment.get_csum();
print("Using OS= " + firstDeployment.get_osname() + " revision=" + startingRevision);

let origin = firstDeployment.get_origin();
let refspec = origin.get_string('origin', 'refspec');

let [,remote,ref] = OSTree.parse_refspec(refspec);
print("Using origin remote=" + remote + " ref=" + ref);

repo.pull(remote, [ref], 0, null, null);

let [,newRev] = repo.resolve_rev(refspec, false);

let targetRev = null;

while (true) {
    sysroot.cleanup(null);

    if (startingRevision == newRev) {
	print("Starting revision is current");
	let [,commitObject] = repo.load_variant(OSTree.ObjectType.COMMIT, startingRevision);
	targetRev = OSTree.commit_get_parent(commitObject);
	print("Using parent target revision " + targetRev);
	repo.pull(remote, [targetRev], 0, null, null);
    } else {
	print("Starting revision is older, using target revision " + newRev);
	targetRev = newRev;
    }

    let origin = sysroot.origin_new_from_refspec(refspec);
    print("DEPLOY BEGIN revision=" + targetRev);
    let [,newDeployment] = sysroot.deploy_tree(firstDeployment.get_osname(), targetRev, origin,
					       firstDeployment, null,
					       null);
    print("DEPLOY END revision=" + targetRev);

    let newDeployments = [newDeployment, firstDeployment];
    sysroot.write_deployments(newDeployments, null);

    sysroot.load(null);
    sysroot.cleanup(null);
    
    let deployments = sysroot.get_deployments();
    if (deployments.length == 0)
	throw new Error("No deployments");
    
    firstDeployment = deployments[0];
}
