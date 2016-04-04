# Atomic Upgrades

## You can turn off the power anytime you want...

OSTree is designed to implement fully atomic and safe upgrades;
more generally, atomic transitions between lists of bootable
deployments.  If the system crashes or you pull the power, you
will have either the old system, or the new one.

## Simple upgrades via HTTP

First, the most basic model OSTree supports is one where it replicates
pre-generated filesystem trees from a server over HTTP, tracking
exactly one ref, which is stored in the `.origin` file for the
deployment.  The command `ostree admin upgrade`
implements this.

To begin a simple upgrade, OSTree fetches the contents of the ref from
the remote server.  Suppose we're tracking a ref named
`exampleos/buildmaster/x86_64-runtime`.  OSTree fetches the URL
`http://example.com/repo/refs/exampleos/buildmaster/x86_64-runtime`,
which contains a SHA256 checksum.  This determines the tree to deploy,
and `/etc` will be merged from currently booted tree.

If we do not have this commit, then, then we perform a pull process.
At present (without static deltas), this involves quite simply just
fetching each individual object that we do not have, asynchronously.
Put in other words, we only download changed files (zlib-compressed).
Each object has its checksum validated and is stored in `/ostree/repo/objects/`.

Once the pull is complete, we have all the objects locally
we need to perform a deployment.

## Upgrades via external tools (e.g. package managers)

As mentioned in the introduction, OSTree is also designed to allow a
model where filesystem trees are computed on the client.  It is
completely agnostic as to how those trees are generated; they could be
computed with traditional packages, packages with post-deployment
scripts on top, or built by developers directly from revision control
locally, etc.

At a practical level, most package managers today (`dpkg` and `rpm`)
operate "live" on the currently booted filesystem.  The way they could
work with OSTree is instead to take the list of installed packages in
the currently booted tree, and compute a new filesystem from that.  A
later chapter describes in more details how this could work:
[adapting-existing.md](Adapting Existing Systems).

For the purposes of this section, let's assume that we have a
newly generated filesystem tree stored in the repo (which shares
storage with the existing booted tree).  We can then move on to
checking it back out of the repo into a deployment.

## Assembling a new deployment directory

Given a commit to deploy, OSTree first allocates a directory for
it.  This is of the form `/boot/loader/entries/ostree-$osname-$checksum.$serial.conf`.
The `$serial` is normally 0, but if a
given commit is deployed more than once, it will be incremented.
This is supported because the previous deployment may have
configuration in `/etc` that we do not want to use or overwrite.

Now that we have a deployment directory, a 3-way merge is
performed between the (by default) currently booted deployment's
`/etc`, its default
configuration, and the new deployment (based on its `/usr/etc`).

## Atomically swapping boot configuration

At this point, a new deployment directory has been created as a
hardlink farm; the running system is untouched, and the bootloader
configuration is untouched.  We want to add this deployment o the
"deployment list".

To support a more general case, OSTree supports atomic transitioning
between arbitrary sets of deployments, with the restriction that the
currently booted deployment must always be in the new set.  In the
normal case, we have exactly one deployment, which is the booted one,
and we want to add the new deployment to the list.  A more complex
command might allow creating 100 deployments as part of one atomic
transaction, so that one can set up an automated system to bisect
across them.

## The bootversion

OSTree allows swapping between boot configurations by implementing the
"swapped directory pattern" in `/boot`.  This means it is a symbolic
link to one of two directories `/ostree/boot.[0|1]`.  To swap the
contents atomically, if the current version is `0`, we create
`/ostree/boot.1`, populate it with the new contents, then atomically
swap the symbolic link.  Finally, the old contents can be garbage
collected at any point.

## The /ostree/boot directory

However, we want to optimize for the case where the set of
kernel/initramfs pairs is the same between both the old and new
deployment lists.  This happens when doing an upgrade that does not
include the kernel; think of a simple translation update.  OSTree
optimizes for this case because on some systems `/boot` may be on a
separate medium such as flash storage not optimized for significant
amounts of write traffic.  Related to this, modern OSTree has support
for having `/boot` be a read-only mount by default - it will
automatically remount read-write just for the portion of time
necessary to update the bootloader configuration.

To implement this, OSTree also maintains the directory
`/ostree/boot.$bootversion`, which is a set
of symbolic links to the deployment directories.  The
`$bootversion` here must match the version of
`/boot`.  However, in order to allow atomic transitions of
*this* directory, this is also a swapped directory,
so just like `/boot`, it has a version of `0` or `1` appended.

Each bootloader entry has a special `ostree=` argument which refers to
one of these symbolic links.  This is parsed at runtime in the
initramfs.
