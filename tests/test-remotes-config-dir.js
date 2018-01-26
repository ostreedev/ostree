#!/usr/bin/env gjs
//
// Copyright (C) 2013 Colin Walters <walters@verbum.org>
// Copyright (C) 2017 Dan Nicholson <nicholson@endlessm.com>
//
// SPDX-License-Identifier: LGPL-2.0+

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

print('1..6')

let remotesDir = Gio.File.new_for_path('remotes.d');
remotesDir.make_directory(null);

let remoteConfig = GLib.KeyFile.new()
remoteConfig.set_value('remote "foo"', 'url', 'http://foo')

let remoteConfigFile = remotesDir.get_child('foo.conf')
remoteConfig.save_to_file(remoteConfigFile.get_path())

// Use the full Repo constructor to set remotes-config-dir
let repoFile = Gio.File.new_for_path('repo');
let repo = new OSTree.Repo({path: repoFile,
                            remotes_config_dir: remotesDir.get_path()});
repo.create(OSTree.RepoMode.ARCHIVE_Z2, null);
repo.open(null);

// See if the remotes.d remote exists
let remotes = repo.remote_list()
assertNotEquals(remotes.indexOf('foo'), -1);

print("ok read-remotes-config-dir");

// Adding a remote should not go in the remotes.d dir unless this is a
// system repo or add-remotes-config-dir is set to true
repo.remote_add('bar', 'http://bar', null, null);
remotes = repo.remote_list()
assertNotEquals(remotes.indexOf('bar'), -1);
assertEquals(remotesDir.get_child('bar.conf').query_exists(null), false);

print("ok add-not-in-remotes-config-dir");

// Removing the remotes.d remote should delete the conf file
repo.remote_delete('foo', null);
remotes = repo.remote_list()
assertEquals(remotes.indexOf('foo'), -1);
assertEquals(remotesDir.get_child('foo.conf').query_exists(null), false);

print("ok delete-in-remotes-config-dir");

// Set add-remotes-config-dir to true and check that a remote gets added
// in the config dir
let repoConfig = repo.copy_config();
repoConfig.set_boolean('core', 'add-remotes-config-dir', true);
repo.write_config(repoConfig);
repo.reload_config(null);
repo.remote_add('baz', 'http://baz', null, null);
remotes = repo.remote_list()
assertNotEquals(remotes.indexOf('baz'), -1);
assertEquals(remotesDir.get_child('baz.conf').query_exists(null), true);

print("ok add-in-remotes-config-dir");

// Trying to set a remote config option via write_config() for a remote
// defined in the config file should succeed
let [, gpg_verify] = repo.remote_get_gpg_verify('bar');
assertEquals(gpg_verify, true);
repoConfig = repo.copy_config();
repoConfig.set_boolean('remote "bar"', 'gpg-verify', false);
repo.write_config(repoConfig);
repo.reload_config(null);
[, gpg_verify] = repo.remote_get_gpg_verify('bar');
assertEquals(gpg_verify, false);

print("ok config-remote-in-config-file-succeeds");

// Trying to set a remote config option via write_config() for a remote
// defined in the config dir should fail with G_IO_ERROR_EXISTS
repoConfig = repo.copy_config();
repoConfig.set_boolean('remote "baz"', 'gpg-verify', false);
try {
    if (repo.write_config(repoConfig))
        throw new Error("config of remote in config dir should fail");
} catch (e) {
    if (!(e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS)))
        throw e;
}

print("ok config-remote-in-config-dir-fails");
