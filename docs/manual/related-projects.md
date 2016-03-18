# Related Projects

OSTree is in many ways very evolutionary.  It builds on concepts and
ideas introduced from many different projects such as
[Systemd Stateless](http://0pointer.net/blog/projects/stateless.html),
[Systemd Bootloader Spec](https://www.freedesktop.org/wiki/Specifications/BootLoaderSpec/),
[Chromium Autoupdate](http://dev.chromium.org/chromium-os/chromiumos-design-docs/filesystem-autoupdate),
the much older
[Fedora/Red Hat Stateless Project](https://fedoraproject.org/wiki/StatelessLinux),
[Linux VServer](http://linux-vserver.org/index.php?title=util-vserver:Vhashify&oldid=2285)
and many more.

As mentioned elsewhere, OSTree is strongly influenced by package
manager designs as well.  This page is not intended to be an
exhaustive list of such projects, but we will try to keep it up to
date, and relatively agnostic.

Broadly speaking, projects in this area fall into two camps; either
a tool to snapshot systems on the client side (dpkg/rpm + BTRFS/LVM),
or a tool to compose on a server and replicate (ChromiumOS, Clear
Linux).  OSTree is flexible enough to do both. 

## Combining dpkg/rpm + (BTRFS/LVM)

In this approach, one uses a block/filesystem snapshot tool underneath
the system package manager.

The
[oVirt Node imgbased](https://gerrit.ovirt.org/gitweb?p=imgbased.git)
tool is an example of this approach, as are a few others below.

Regarding [BTRFS](https://btrfs.wiki.kernel.org/index.php/Main_Page)
in particular - the OSTree author believes that Linux storage is a
wide world, and while BTRFS is quite good, it is not everywhere now,
nor will it be in the near future. There are other recently developed
filesystems like [f2fs](https://en.wikipedia.org/wiki/F2FS), and Red
Hat Enterprise Linux still defaults to
[XFS](https://en.wikipedia.org/wiki/XFS).

Using a snapshot tool underneath a package manager does help
significantly.  In the rest of this text, we will use "BTRFS" as a
mostly generic tool for filesystem snapshots.

The obvious thing to do is layer BTRFS under dpkg/rpm, and have a
separate subvolume for `/home` so rollbacks don't lose your data. See
e.g. [Fedora BTRFS Rollback Feature](http://fedoraproject.org/wiki/Features/SystemRollbackWithBtrfs).

More generally, if you want to use BTRFS to roll back changes made by
dpkg/rpm, you have to carefully set up the partition layout so that
the files laid out by dpkg/rpm are installed in a subvolume to
snapshot.

This problem in many ways is addressed by the changes OSTree forces,
such as putting all local state in `/var` (e.g. `/usr/local` ->
`/var/usrlocal`). Then one can BTRFS snapshot `/usr`. This gets pretty
far, except handling `/etc` is messy. This is something OSTree does
well.

In general, if one really tries to flesh out the BTRFS approach, a
nontrivial middle layer of code between dpkg/rpm and BTRFS (or deep
awareness of BTRFS in dpkg/rpm itself) will be required.

The OSTree author believes that having total freedom at the block
storage layer is better for general purpose operating systems. For
example, with OSTree, one is free to use BTRFS in any way you like -
you can use a subvolume for `/home`, or you can not.

Furthermore, in its most basic incarnation, the rpm/dpkg + BTRFS
doesn't solve the race conditions that happen when unpacking packages
into the live system, such as deleting the files underneath Firefox
while it's running. One could unpack packages into a separate root,
and switch to that, which gets closer to the OSTree architecture.

Note though OSTree does take advantage of BTRFS if installed on top of
it!  In particular, it will use reflink for the copies of `/etc` if
available.

All of the above also applies if one replaces "BTRFS" with "LVM
snapshots" except for the reflinks.

Finally, see the next portion around ChromiumOS for why a hybrid but
integrated package/image system improves on this.

## ChromiumOS updater

Many people who look at OSTree are most interested in using
it as an updater for embedded or fixed-purpose systems, similar to use cases
from the [ChromiumOS updater](http://dev.chromium.org/chromium-os/chromiumos-design-docs/filesystem-autoupdate).

The ChromiumOS approach uses two partitions that are swapped via the
bootloader.  It has a very network-efficient update protocol, using a
custom binary delta scheme between filesystem snapshots.

This model even allows for switching filesystem types in an update.

A major downside of this approach is that the OS size is doubled on
disk always.  In contrast, OSTree uses plain Unix hardlinks, which
means it essentially only requires disk space proportional to the
changed files, plus some small fixed overhead.

This means with OSTree, one can easily have more than two trees
(deployments).  Another example is that the system OSTree repository
could *also* be used for application containers.

Finally, the author of OSTree believes that what one really wants for
many cases is image replication *with* the ability to layer on some
additional components (e.g. packages) - a hybrid model.  This is what
[rpm-ostree](https://github.com/projectatomic/rpm-ostree/) is aiming
to support.

## Ubuntu Image Based Updates

See <https://wiki.ubuntu.com/ImageBasedUpgrades>. Very architecturally
similar to ChromeOS, although more interesting is discussion for
supporting package installation on top, similar to
[rpm-ostree package layering](https://github.com/projectatomic/rpm-ostree/pull/107).

## Clear Linux Software update

The
[Clear Linux Software update](https://clearlinux.org/features/software-update)
system is not very well documented.
[This mailing list post](https://lists.clearlinux.org/pipermail/dev/2016-January/000159.html)
has some reverse-engineered design documentation.

Like OSTree static deltas, it also uses bsdiff for network efficiency.

More information will be filled in here over time.  The OSTree author
believes that at the moment, the "CL updater" is not truly atomic in
the sense that because it applies updates live, there is a window
where the OS root may be inconsistent.

## OLPC update

OSTree is basically a generalization of olpc-update, except using
plain HTTP instead of rsync.  OSTree has the notion of separate trees
that one can track independently or parallel install, while still
sharing storage via the hardlinked repository, whereas olpc-update
uses version numbers for a single OS.

OSTree has built-in plain old HTTP replication which can be served
from a static webserver, whereas olpc-update uses `rsync` (more server
load, but more efficient on the network side).  The OSTree solution to
improving network bandwidth consumption is via static deltas.

See
[this comment](http://blog.verbum.org/2013/08/26/ostree-v2013-6-released/#comment-1169)
for a comparison.

## NixOS

See [NixOS](http://nixos.org/). It was a very influential project for
OSTree.  NixOS and OSTree both support the idea of independent "roots"
that are bootable.

In NixOS, the entire system is based on checksums of package inputs
(build dependencies) - see [Nix store](http://nixos.org/nix/manual/#chap-package-management/). A both
positive and negative of the Nix model is that a change in the build
dependencies (e.g. being built with a newer gcc), requires a cascading
rebuild of everything.

In OSTree, the checksums are of object *content* (including extended
attributes). This means that any data that's identical is
transparently, automatically shared on disk. It's possible to ask the
Nix store to deduplicate, (via hard links and immutable bit), but this
is significantly less efficient than the OSTree approach. The Nix use
of the ext immutable bit is racy, since it has to be briefly removed
to make a hard link.

At the lowest level, OSTree is just "git for binaries" - it isn't tied
strongly to any particular build system. You can put whatever data you
want inside an OSTree repository, built however you like. So for
example, while one could make a build system that did the "purely
functional" approach of Nix, it also works to have a build system that
just rebuilds individual components (packages) as they change, without
forcing a rebuild of their dependencies.

The author of OSTree believes that while Nix has some good ideas,
forcing a rebuild of everything for a security update to e.g. glibc is
not practical at scale.

## Solaris IPS

See
[Solaris IPS](http://hub.opensolaris.org/bin/view/Project+pkg/). Broadly,
this is a similar design as to a combination of BTRFS+RPM/deb.  There
is a bootloader management system which combines with the snapshots.
It's relatively well thought through - however, it is a client-side
system assembly.  If one wants to image servers and replicate
reliably, that'd be a different system.


## Conary

See
[Conary Updates and Rollbacks](http://wiki.rpath.com/wiki/Conary:Updates_and_Rollbacks). If
rpm/dpkg are like CVS, Conary is closer to Subversion. It's not bad,
but e.g. its rollback model is rather ad-hoc and not atomic.  It also
is a fully client side system and doesn't have an image-like
replication with deltas.

## bmap

See
[bmap](https://source.tizen.org/documentation/reference/bmaptool/introduction).
A tool for optimized copying of disk images. Intended for offline use,
so not directly comparable.
