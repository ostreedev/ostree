---
nav_order: 40
---

# Deployments
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

## Overview

Built on top of the OSTree versioning filesystem core is a layer
that knows how to deploy, parallel install, and manage Unix-like
operating systems (accessible via `ostree admin`).  The core content of these operating systems
are treated as read-only, but they transparently share storage.

A deployment is physically located at a path of the form
`/ostree/deploy/$stateroot/deploy/$checksum`.
OSTree is designed to boot directly into exactly one deployment
at a time; each deployment is intended to be a target for
`chroot()` or equivalent.

### "stateroot" (AKA "osname"): Group of deployments that share /var

Each deployment is grouped in exactly one "stateroot" (also known as an "osname");
the former term is preferred.

From above, you can see that a stateroot is physically represented in the
`/ostree/deploy/$stateroot` directory. For example, OSTree can allow parallel
installing Debian in `/ostree/deploy/debian` and Red Hat Enterprise Linux in
`/ostree/deploy/rhel` (subject to operating system support, present released
versions of these operating systems may not support this).

Each stateroot has exactly one copy of the traditional Unix `/var`,
stored physically in `/ostree/deploy/$stateroot/var`.  OSTree provides
support tools for `systemd` to create a Linux bind mount that ensures
the booted deployment sees the shared copy of `/var`.

OSTree does not touch the contents of `/var`.  Operating system
components such as daemon services are required to create any
directories they require there at runtime
(e.g. `/var/cache/$daemonname`), and to manage upgrading data formats
inside those directories.

### Contents of a deployment

A deployment begins with a specific commit (represented as a
SHA256 hash) in the OSTree repository in `/ostree/repo`.  This commit refers
to a filesystem tree that represents the underlying basis of a
deployment.  For short, we will call this the "tree", to
distinguish it from the concept of a deployment.

First, the tree must include a kernel (and optionally an initramfs).  The
current standard locations for these are `/usr/lib/modules/$kver/vmlinuz` and
`/usr/lib/modules/$kver/initramfs.img`.  The "boot checksum" will be computed
automatically.  This follows the current Fedora kernel layout, and is
the current recommended path.  However, older versions of libostree don't
support this; you may need to also put kernels in the previous (legacy)
paths, which are `vmlinuz(-.*)?-$checksum` in either `/boot` or `/usr/lib/ostree-boot`.
The checksum should be a SHA256 hash of the kernel contents; it must be
pre-computed before storing the kernel in the repository.  Optionally,
the directory can also contain an initramfs, stored as
`initramfs(-.*)?-$checksum` and/or a device tree, stored as
`devicetree(-.*)?-$checksum`.  If an initramfs or devicetree exist,
the checksum must include all of the kernel, initramfs and devicetree contents.
OSTree will use this to determine which kernels are shared.  The rationale for
this is to avoid computing checksums on the client by default.

The deployment should not have a traditional UNIX `/etc`; instead, it
should include `/usr/etc`.  This is the "default configuration".  When
OSTree creates a deployment, it performs a 3-way merge using the
*old* default configuration, the active system's `/etc`, and the new
default configuration.  In the final filesystem tree for a deployment
then, `/etc` is a regular writable directory.

Besides the exceptions of `/var` and `/etc` then, the rest of the
contents of the tree are checked out as hard links into the
repository.  It's strongly recommended that operating systems ship all
of their content in `/usr`, but this is not a hard requirement.

Finally, a deployment may have a `.origin` file, stored next to its
directory.  This file tells `ostree admin upgrade` how to upgrade it.
At the moment, OSTree only supports upgrading a single refspec.
However, in the future OSTree may support a syntax for composing
layers of trees, for example.

### Staged deployments

As mentioned above, when OSTree creates a new deployment, a 3-way merge is done
to update its `/etc`. Depending on the nature of the system, this can cause an
issue: if a user or program modifies the booted `/etc` *after* the pending
deployment is created but *before* rebooting, those modifications will be lost.
OSTree does not do a second `/etc` merge on reboot.

To counter this, OSTree supports staged deployments. In this flow, deployments
are created using e.g. `ostree admin upgrade --stage` on the CLI. The new
deployment is still created when the command is invoked, but the 3-way `/etc`
merge is delayed until the system is rebooted or shut down. Additionally,
updating the bootloader is also delayed. This is done by the
`ostree-finalize-staged.service` systemd unit.

The main disadvantage of this approach is that rebooting can take longer and the
failure mode can be confusing (the machine will reboot into the same
deployment). In systems where the workload is well-understood and not subject to
the `/etc` issue above, it may be better to not stage deployments.

### The system /boot

While OSTree parallel installs deployments cleanly inside the
`/ostree` directory, ultimately it has to control the system's `/boot`
directory.  The way this works is via the
[Boot Loader Specification](https://uapi-group.org/specifications/specs/boot_loader_specification/),
which is a standard for bootloader-independent drop-in configuration
files.

When a tree is deployed, it will have a configuration file generated
of the form
`/boot/loader/entries/ostree-$stateroot-$checksum.$serial.conf`.  This
configuration file will include a special `ostree=` kernel argument
that allows the initramfs to find (and `chroot()` into) the specified
deployment.

At present, not all bootloaders implement the BootLoaderSpec, so
OSTree contains code for some of these to regenerate native config
files (such as `/boot/syslinux/syslinux.conf`) based on the entries.
