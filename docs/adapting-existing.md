---
nav_order: 70
---

# Adapting existing mainstream distributions
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

## System layout

First, OSTree encourages systems to implement
[UsrMove](http://www.freedesktop.org/wiki/Software/systemd/TheCaseForTheUsrMerge/)
This is simply to avoid the need for more bind mounts.  By default
OSTree's dracut hook creates a read-only bind mount over `/usr`; you
can of course generate individual bind-mounts for `/bin`, all the
`/lib` variants, etc.  So it is not intended to be a hard requirement.

Remember, because by default the system is booted into a `chroot`
equivalent, there has to be some way to refer to the actual physical
root filesystem.  Therefore, your operating system tree should contain
an empty `/sysroot` directory; at boot time, OSTree will make this a
bind mount to the physical / root directory.  There is precedent for
this name in the initramfs context.  You should furthermore make a
toplevel symbolic link `/ostree` which points to `/sysroot/ostree`, so
that the OSTree tool at runtime can consistently find the system data
regardless of whether it's operating on a physical root or inside a
deployment.

Because OSTree only preserves `/var` across upgrades (each
deployment's chroot directory will be garbage collected
eventually), you will need to choose how to handle other
toplevel writable directories specified by the [Filesystem Hierarchy Standard](http://www.pathname.com/fhs/).
Your operating system may of course choose
not to support some of these such as `/usr/local`, but following is the
recommended set:

 - `/home` → `/var/home`
 - `/opt` → `/var/opt`
 - `/srv` → `/var/srv`
 - `/root` → `/var/roothome`
 - `/usr/local` → `/var/usrlocal`
 - `/mnt` → `/var/mnt`
 - `/tmp` → `/sysroot/tmp`

Furthermore, since `/var` is empty by default, your operating system
will need to dynamically create the *targets* of these at boot.  A
good way to do this is using `systemd-tmpfiles`, if your OS uses
systemd.  For example:

```
d /var/log/journal 0755 root root -
L /var/home - - - - ../sysroot/home
d /var/opt 0755 root root -
d /var/srv 0755 root root -
d /var/roothome 0700 root root -
d /var/usrlocal 0755 root root -
d /var/usrlocal/bin 0755 root root -
d /var/usrlocal/etc 0755 root root -
d /var/usrlocal/games 0755 root root -
d /var/usrlocal/include 0755 root root -
d /var/usrlocal/lib 0755 root root -
d /var/usrlocal/man 0755 root root -
d /var/usrlocal/sbin 0755 root root -
d /var/usrlocal/share 0755 root root -
d /var/usrlocal/src 0755 root root -
d /var/mnt 0755 root root -
d /run/media 0755 root root -
```

However, as of OSTree 2023.9 there is support for a `root.transient`
model, which can increase compatibility in some scenarios.  For more
information, see `man ostree-prepare-root.conf`.

Particularly note here the double indirection of `/home`.  By default,
each deployment will share the global toplevel `/home` directory on
the physical root filesystem.  It is then up to higher levels of
management tools to keep `/etc/passwd` or equivalent synchronized
between operating systems.  Each deployment can easily be reconfigured
to have its own home directory set simply by making `/var/home` a real
directory.

## Booting and initramfs technology

OSTree comes with optional dracut+systemd integration code which follows
this logic:

- Parse the `ostree=` kernel command line argument in the initramfs
- Set up a read-only bind mount on `/usr`
- Bind mount the deployment's `/sysroot` to the physical `/`
- Use `mount(MS_MOVE)` to make the deployment root appear to be the root filesystem

After these steps, systemd switches root.

If you are not using dracut or systemd, using OSTree should still be
possible, but you will have to write the integration code. See the
existing sources in
[src/switchroot](https://github.com/ostreedev/ostree/tree/main/src/switchroot)
as a reference.

Patches to support other initramfs technologies and init systems, if
sufficiently clean, will likely be accepted upstream.

A further specific note regarding `sysvinit`: OSTree used to support
recording device files such as the `/dev/initctl` FIFO, but no longer
does.  It's recommended to just patch your initramfs to create this at
boot.

## System users and groups

Unlike traditional package systems, OSTree trees contain *numeric* uid
and gids (the same is true of e.g. OCI).

Furthermore, OSTree does not have a `%post` type mechanism
where `useradd` could be invoked.  In order to ship an OS that
contains both system users and users dynamically created on client
machines, you will need to choose a solution for `/etc/passwd`.  The
core problem is that if you add a user to the system for a daemon, the
OSTree upgrade process for `/etc` will simply notice that because
`/etc/passwd` differs from the previous default, it will keep the
modified config file, and your new OS user will not be visible.

First, consider using [systemd DynamicUser=yes](https://0pointer.net/blog/dynamic-users-with-systemd.html)
where applicable.  This entirely avoids problems with static
allocations.

### Static users and groups

For users which must be allocated statically (for example, they
are used by setuid executables in `/usr/bin`, there are two
primary wants to handle this.

The [nss-altfiles](https://github.com/aperezdc/nss-altfiles)
was created to pair with image-based update systems like OSTree,
and is used by many operating systems and distributions today.

More recently, [nss-systemd](https://www.freedesktop.org/software/systemd/man/nss-systemd.html)
gained support for statically allocated users and groups in
a JSON format stored in `/usr/lib/userdb`.

### sysusers.d

Some users and groups can be assigned dynamically via [sysusers.d](https://www.freedesktop.org/software/systemd/man/sysusers.d.html).  This means users and groups are maintained per-machine and may drift (unless statically assigned in sysusers).

But this model is suitable for users and groups which must always be present,
but do not have file content in the image.

## Adapting existing package managers

The largest endeavor is likely to be redesigning your distribution's
package manager to be on top of OSTree, particularly if you want to
keep compatibility with the "old way" of installing into the physical
`/`.  This section will use examples from both `dpkg` and `rpm` as the
author has familiarity with both; but the abstract concepts should
apply to most traditional package managers.

There are many levels of possible integration; initially, we will
describe the most naive implementation which is the simplest but also
the least efficient.  We will assume here that the admin is booted
into an OSTree-enabled system, and wants to add a set of packages.

Many package managers store their state in `/var`; but since in the
OSTree model that directory is shared between independent versions,
the package database must first be found in the per-deployment `/usr`
directory.  It becomes read-only; remember, all upgrades involve
constructing a new filesystem tree, so your package manager will also
need to create a copy of its database.  Most likely, if you want to
continue supporting non-OSTree deployments, simply have your package
manager fall back to the legacy `/var` location if the one in `/usr`
is not found.

To install a set of new packages (without removing any existing ones),
enumerate the set of packages in the currently booted deployment, and
perform dependency resolution to compute the complete set of new
packages.  Download and unpack these new packages to a temporary
directory.

Now, because we are merely installing new packages and not
removing anything, we can make the major optimization of reusing
our existing filesystem tree, and merely
*layering* the composed filesystem tree of
these new packages on top.  A command like this:

```
ostree commit -b osname/releasename/description
--tree=ref=$osname/$releasename/$description
--tree=dir=/var/tmp/newpackages.13A8D0/
```

will create a new commit in the `$osname/$releasename/$description`
branch.  The OSTree SHA256 checksum of all the files in
`/var/tmp/newpackages.13A8D0/` will be computed, but we will not
re-checksum the present existing tree.  In this layering model,
earlier directories will take precedence, but files in later layers
will silently override earlier layers.

Then to actually deploy this tree for the next boot:
`ostree admin deploy $osname/$releasename/$description`

This is essentially what [rpm-ostree](https://github.com/projectatomic/rpm-ostree/)
does to support its [package layering model](https://coreos.github.io/rpm-ostree/administrator-handbook/#hybrid-imagepackaging-via-package-layering).
