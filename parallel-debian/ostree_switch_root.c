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
perrorv (const char *format, ...)
{
	va_list args;
	char buf[PATH_MAX];
	char *p;

	p = strerror_r (errno, buf, sizeof (buf));

	va_start (args, format);

	vfprintf (stderr, format, args);
	fprintf (stderr, ": %s\n", p);

	va_end (args);
	
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
	const char *root_bind_mounts[] = { "/home", "/root", "/var", NULL };
	int i;
	int orig_cfd;
	int new_cfd;
	pid_t pid;
	char srcpath[PATH_MAX];
	char destpath[PATH_MAX];

	orig_cfd = open("/", O_RDONLY);
	new_cfd = open(newroot, O_RDONLY);

	/* For now just remount the rootfs r/w.  Should definitely
	 * handle this better later... (famous last words)
	 */
	if (mount(newroot, newroot, NULL, MS_REMOUNT & ~MS_RDONLY, NULL) < 0) {
		perrorv("failed to remount / read/write", newroot);
		return -1;
	}

	for (i = 0; root_bind_mounts[i] != NULL; i++) {
		snprintf(srcpath, sizeof(srcpath), "%s%s", newroot, root_bind_mounts[i]);
		snprintf(destpath, sizeof(destpath), "%s/%s%s", newroot, subroot, root_bind_mounts[i]);
		if (mount(srcpath, destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0) {
			perrorv("failed to bind mount %s to %s", srcpath, destpath);
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
		perrorv("failed to fchdir back to initrd");
		return -1;
	}

	if (chroot(subroot) < 0) {
		perrorv("failed to change root to %s", subroot);
		return -1;
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
	fprintf(output, "usage: %s <newrootdir> <subroot> <init> <args to init>\n",
			program_invocation_short_name);
	exit(output == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	char *newroot, *subroot, *init, **initargs;

	if (argc < 3)
		usage(stderr);

	if ((!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
		usage(stdout);

	newroot = argv[1];
	subroot = argv[2];
	init = argv[3];
	initargs = &argv[3];

	if (!*newroot || !*init)
		usage(stderr);

	if (switchroot(newroot, subroot))
		exit(1);

	if (access(init, X_OK))
		perrorv("cannot access %s", init);

	execv(init, initargs);
	exit(1);
}

