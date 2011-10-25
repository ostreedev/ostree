/* -*- c-file-style: "linux" -*-
 * ostree_switch_root.c - switch to new root directory and start init.
 * Copyright 2011 Colin Walters <walters@verbum.org>
 *
 * Based of switch_root.c from util-linux.
 * Copyright 2002-2009 Red Hat, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *	Peter Jones <pjones@redhat.com>
 *	Jeremy Katz <katzj@redhat.com>
 *	Colin Walters <walters@verbum.org>
 */
#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>

static int
perrorv (const char *format, ...) __attribute__ ((format (printf, 1, 2)));

static int
perrorv (const char *format, ...)
{
	va_list args;
	char buf[PATH_MAX];
	char *p;

	p = strerror_r (errno, buf, sizeof (buf));

	va_start (args, format);

	vfprintf (stderr, format, args);
	fprintf (stderr, ": %s\n", p);
	fflush (stderr);

	va_end (args);

	sleep (3);
	
	return 0;
}

/* remove all files/directories below dirName -- don't cross mountpoints */
static int recursiveRemove(int fd)
{
	struct stat rb;
	DIR *dir;
	int rc = -1;
	int dfd;

	if (!(dir = fdopendir(fd))) {
		perrorv("failed to open directory");
		goto done;
	}

	/* fdopendir() precludes us from continuing to use the input fd */
	dfd = dirfd(dir);

	if (fstat(dfd, &rb)) {
		perrorv("failed to stat directory");
		goto done;
	}

	while(1) {
		struct dirent *d;

		errno = 0;
		if (!(d = readdir(dir))) {
			if (errno) {
				perrorv("failed to read directory");
				goto done;
			}
			break;	/* end of directory */
		}

		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		if (d->d_type == DT_DIR) {
			struct stat sb;

			if (fstatat(dfd, d->d_name, &sb, AT_SYMLINK_NOFOLLOW)) {
				perrorv("failed to stat %s", d->d_name);
				continue;
			}

			/* remove subdirectories if device is same as dir */
			if (sb.st_dev == rb.st_dev) {
				int cfd;

				cfd = openat(dfd, d->d_name, O_RDONLY);
				if (cfd >= 0) {
					recursiveRemove(cfd);
					close(cfd);
				}
			} else
				continue;
		}

		if (unlinkat(dfd, d->d_name,
			     d->d_type == DT_DIR ? AT_REMOVEDIR : 0))
			perrorv("failed to unlink %s", d->d_name);
	}

	rc = 0;	/* success */

done:
	if (dir)
		closedir(dir);
	return rc;
}

static int make_readonly(const char *tree)
{
	struct stat stbuf;
	/* Ignore nonexistent directories for now;
	 * some installs won't have e.g. lib64
	 */
	if (stat (tree, &stbuf) < 0)
		return 0;
	if (mount(tree, tree, NULL, MS_BIND, NULL) < 0) {
		perrorv("Failed to do initial RO bind mount %s", tree);
		return -1;
	}
	if (mount(tree, tree, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
		perrorv("Failed to remount RO bind mount %s", tree);
		return -1;
	}
	return 0;
}


static int switchroot(const char *newroot, const char *subroot)
{
	const char *initrd_move_mounts[] = { "/dev", "/proc", "/sys", NULL };
	const char *toproot_bind_mounts[] = { "/boot", "/home", "/root", "/tmp", NULL };
	const char *ostree_bind_mounts[] = { "/var", NULL };
	const char *readonly_bind_mounts[] = { "/bin", "/etc", "/lib",
					       "/lib32", "/lib64", "/sbin",
					       "/usr",
					       NULL };
	int i;
	int orig_cfd;
	int new_cfd;
	int subroot_cfd;
	pid_t pid;
	char subroot_path[PATH_MAX];
	char srcpath[PATH_MAX];
	char destpath[PATH_MAX];

	fprintf (stderr, "switching to root %s subroot: %s\n", newroot, subroot);

	orig_cfd = open("/", O_RDONLY);
	new_cfd = open(newroot, O_RDONLY);

	/* For now just remount the rootfs r/w.  Should definitely
	 * handle this better later... (famous last words)
	 */
	if (mount(newroot, newroot, NULL, MS_REMOUNT, NULL) < 0) {
		perrorv("failed to remount %s read/write", newroot);
		return -1;
	}

	snprintf(subroot_path, sizeof(subroot_path), "%s/ostree/%s", newroot, subroot);
	subroot_cfd = open(subroot_path, O_RDONLY);
	if (subroot_cfd < 0) {
		perrorv("failed to open subroot %s", subroot_path);
		return -1;
	}

	for (i = 0; initrd_move_mounts[i] != NULL; i++) {
		snprintf(destpath, sizeof(destpath), "%s%s", subroot_path, initrd_move_mounts[i]);

		if (mount(initrd_move_mounts[i], destpath, NULL, MS_MOVE, NULL) < 0) {
			perrorv("failed to move initramfs mount %s to %s",
				initrd_move_mounts[i], destpath);
			umount2(initrd_move_mounts[i], MNT_FORCE);
		}
	}

	for (i = 0; toproot_bind_mounts[i] != NULL; i++) {
		snprintf(srcpath, sizeof(srcpath), "%s%s", newroot, toproot_bind_mounts[i]);
		snprintf(destpath, sizeof(destpath), "%s/%s", subroot_path, toproot_bind_mounts[i]);
		if (mount(srcpath, destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0) {
			perrorv("failed to bind mount (class:toproot) %s to %s", srcpath, destpath);
			return -1;
		}
	}

	for (i = 0; ostree_bind_mounts[i] != NULL; i++) {
		snprintf(srcpath, sizeof(srcpath), "%s/ostree%s", newroot, ostree_bind_mounts[i]);
		snprintf(destpath, sizeof(destpath), "%s%s", subroot_path, ostree_bind_mounts[i]);
		if (mount(srcpath, destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0) {
			perrorv("failed to bind mount (class:bind) %s to %s", srcpath, destpath);
			return -1;
		}
	}

	if (chdir(newroot)) {
		perrorv("failed to change directory to %s", newroot);
		return -1;
	}

	if (mount(newroot, "/", NULL, MS_MOVE, NULL) < 0) {
		perrorv("failed to mount moving %s to /", newroot);
		return -1;
	}

	if (fchdir (new_cfd) < 0) {
		perrorv("failed to fchdir back to root");
		return -1;
	}

	snprintf(destpath, sizeof(destpath), "ostree/%s", subroot);
	if (chroot(destpath) < 0) {
		perrorv("failed to change root to %s", destpath);
		return -1;
	}

	if (chdir ("/") < 0) {
		perrorv("failed to chdir to subroot");
		return -1;
	}

	for (i = 0; readonly_bind_mounts[i] != NULL; i++) {
		if (make_readonly(readonly_bind_mounts[i]) < 0) {
			return -1;
		}
	}

	if (orig_cfd >= 0) {
		pid = fork();
		if (pid <= 0) {
			recursiveRemove(orig_cfd);
			if (pid == 0)
				exit(EXIT_SUCCESS);
		}
		close(orig_cfd);
	}
	close(new_cfd);
	return 0;
}

static void usage(FILE *output)
{
	fprintf(output, "usage: %s <newrootdir> <init> <args to init>\n",
			program_invocation_short_name);
	exit(output == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	char *newroot, *subroot, *init, **initargs;

	fflush (stderr);
	if (argc < 4)
		usage(stderr);

	if ((!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
		usage(stdout);

	newroot = argv[1];
	subroot = argv[2];
	init = argv[3];
	initargs = &argv[3];

	if (!*newroot || !*subroot || !*init)
		usage(stderr);

	if (switchroot(newroot, subroot))
		exit(1);

	if (access(init, X_OK))
		perrorv("cannot access %s", init);

	execv(init, initargs);
	perrorv("Failed to exec init '%s'", init);
	exit(1);
}

