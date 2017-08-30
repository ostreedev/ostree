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

function assertNotEquals(a, b) {
    if (a == b)
	throw new Error("assertion failed " + JSON.stringify(a) + " != " + JSON.stringify(b));
}

function libtestExec(shellCode) {
    let testdatadir = GLib.getenv("G_TEST_SRCDIR");
    let libtestPath = GLib.build_filenamev([testdatadir, 'tests/libtest.sh'])
    let proc = Gio.Subprocess.new(['bash', '-c', 'set -xeuo pipefail; . ' + GLib.shell_quote(libtestPath) + '; ' + shellCode], 0);
    proc.wait_check(null);
}

print('1..1')

libtestExec('setup_os_repository archive-z2 syslinux');

GLib.setenv("OSTREE_SYSROOT_DEBUG", "mutable-deployments", true);

let upstreamRepo = OSTree.Repo.new(Gio.File.new_for_path('testos-repo'));
upstreamRepo.open(null);

let runtimeRef = 'testos/buildmaster/x86_64-runtime';
let [,rev] = upstreamRepo.resolve_rev(runtimeRef, false);

print("testos => " + rev);

//// TEST: We should have no deployments

let sysroot = OSTree.Sysroot.new(Gio.File.new_for_path('sysroot'));
sysroot.load(null);
let deployments = sysroot.get_deployments();
assertEquals(deployments.length, 0);

//// Add the remote, and do a pull

let [,sysrootRepo] = sysroot.get_repo(null);
sysrootRepo.remote_add('testos', 'file://' + upstreamRepo.get_path().get_path(),
		       GLib.Variant.new('a{sv}', {'gpg-verify': GLib.Variant.new('b', false),
						  'branches': GLib.Variant.new('as', [runtimeRef])}), null);
sysrootRepo.pull('testos', null, 0, null, null);

//// TEST: We can deploy one tree

let mergeDeployment = sysroot.get_merge_deployment('testos');

let origin = sysroot.origin_new_from_refspec(runtimeRef);
let [,deployment] = sysroot.deploy_tree('testos', rev, origin,
					mergeDeployment, null,
					null);
let newDeployments = deployments;
deployments = null;
newDeployments.unshift(deployment);
sysroot.write_deployments(newDeployments, null);
deployments = sysroot.get_deployments();
assertEquals(deployments.length, newDeployments.length);
assertEquals(deployments[0].get_csum(), deployment.get_csum());

let deploymentPath = sysroot.get_deployment_directory(deployment);
assertEquals(deploymentPath.query_exists(null), true);

print("OK one deployment");

/// TEST: We can delete the deployment, going back to empty
sysroot.write_deployments([], null);

print("OK empty deployments");

assertEquals(deploymentPath.query_exists(null), false);

//// Ok, redeploy, then add a new revision upstream and pull it

let [,deployment] = sysroot.deploy_tree('testos', rev, origin,
					mergeDeployment, null,
					null);
newDeployments = deployments;
deployments = null;
newDeployments.unshift(deployment);
print(JSON.stringify(newDeployments));
sysroot.write_deployments(newDeployments, null);

libtestExec('os_repository_new_commit');

sysrootRepo.pull('testos', null, 0, null, null);

let [,newRev] = upstreamRepo.resolve_rev(runtimeRef, false);

print("testos => " + newRev);
assertNotEquals(rev, newRev);

mergeDeployment = sysroot.get_merge_deployment('testos');
assertEquals(mergeDeployment.get_csum(), deployment.get_csum());
let [,newDeployment] = sysroot.deploy_tree('testos', newRev, origin,
					   mergeDeployment, null,
					   null);
newDeployments = [newDeployment, mergeDeployment];
assertNotEquals(mergeDeployment.get_bootcsum(), newDeployment.get_bootcsum());
assertNotEquals(mergeDeployment.get_csum(), newDeployment.get_csum());
sysroot.write_deployments(newDeployments, null);
deployments = sysroot.get_deployments();
assertEquals(deployments.length, 2);
assertEquals(deploymentPath.query_exists(null), true);
let newDeploymentPath = sysroot.get_deployment_directory(newDeployment);
assertEquals(newDeploymentPath.query_exists(null), true);

print("OK two deployments");

libtestExec('os_repository_new_commit 0 1');

sysrootRepo.pull('testos', null, 0, null, null);

let [,thirdRev] = sysrootRepo.resolve_rev(runtimeRef, false);
assertNotEquals(newRev, thirdRev);

mergeDeployment = sysroot.get_merge_deployment('testos');
let [,thirdDeployment] = sysroot.deploy_tree('testos', thirdRev, origin,
					     mergeDeployment, null,
					     null);
assertEquals(mergeDeployment.get_bootcsum(), thirdDeployment.get_bootcsum());
assertNotEquals(mergeDeployment.get_csum(), thirdDeployment.get_csum());
newDeployments = [deployment, newDeployment, thirdDeployment];
sysroot.write_deployments(newDeployments, null);
deployments = sysroot.get_deployments();
assertEquals(deployments.length, 3);

print("ok test-sysroot")
