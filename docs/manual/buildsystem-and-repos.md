# Writing a buildsystem and managing repositories

OSTree is not a package system.  It does not directly support building
source code.  Rather, it is a tool for transporting and managing
content, along with package-system independent aspects like bootloader
management for updates.

We'll assume here that we're planning to generate commits on a build
server, then have client systems replicate it.  Doing client-side
assembly is also possible of course, but this discussion will focus
primarily on server-side concerns.

## Build vs buy

Therefore, you need to either pick an existing tool for writing
content into an OSTree repository, or to write your own.  An example
tool is [rpm-ostree](https://github.com/projectatomic/rpm-ostree) - it
takes as input RPMs, and commits them (currently oriented for a server
side, but aiming to do client side too).

## Initializing

For this initial discussion, we're assuming you have a single
`archive-z2` repository:

```
mkdir repo
ostree --repo=repo init --mode=archive-z2
```

You can export this via a static webserver, and configure clients to
pull from it.

## Writing your own OSTree buildsystem

There exist many, many systems that basically follow this pattern:

```
$pkg --installroot=/path/to/tmpdir install foo bar baz
$imagesystem commit --root=/path/to/tmpdir
```

For various values of `$pkg` such as `yum`, `apt-get`, etc., and
values of `$imagesystem` could be simple tarballs, Amazon Machine
Images, ISOs, etc.

Now obviously in this document, we're going to talk about the
situation where `$imagesystem` is OSTree.  The general idea with
OSTree is that wherever you might store a series of tarballs for
applications or OS images, OSTree is likely going to be better.  For
example, it supports GPG signatures, binary deltas, writing bootloader
configuration, etc.

OSTree does not include a package/component build system simply
because there already exist plenty of good ones - rather, it is
intended to provide an infrastructure layer.

The above mentioned `rpm-ostree compose tree` chooses RPM as the value
of `$pkg` - so binaries are built as RPMs, then committed as a whole
into an OSTree commit.

But let's discuss building our own.  If you're just experimenting,
it's quite easy to start with the command line.  We'll assume for this
purpose that you have a build process that outputs a directory tree -
we'll call this tool `$pkginstallroot` (which could be `yum
--installroot` or `debootstrap`, etc.).

Your initial prototype is going to look like:

```
$pkginstallroot /path/to/tmpdir
ostree --repo=repo commit -s 'build' -b exampleos/x86_64/standard --tree=dir=/path/to/tmpdir
```

Alternatively, if your build system can generate a tarball, you can
commit that tarball into OSTree.  For example,
[OpenEmbedded](http://www.openembedded.org/) can output a tarball, and
one can commit it via:

```
ostree commit -s 'build' -b exampleos/x86_64/standard --tree=tar=myos.tar
```

## Constructing trees from unions

The above is a very simplistic model, and you will quickly notice that
it's slow.  This is because OSTree has to re-checksum and recompress
the content each time it's committed.  (Most of the CPU time is spent
in compression which gets thrown away if the content turns out to be
already stored).

A more advanced approach is to store components in OSTree itself, then
union them, and recommit them.  At this point, we recommend taking a
look at the OSTree API, and choose a programming language supported by
[GObject Introspection](https://wiki.gnome.org/Projects/GObjectIntrospection)
to write your buildsystem scripts.  Python may be a good choice, or
you could choose custom C code, etc.

For the purposes of this tutorial we will use shell script, but it's
strongly recommended to choose a real programming language for your
build system.

Let's say that your build system produces separate artifacts (whether
those are RPMs, zip files, or whatever).  These artifacts should be
the result of `make install DESTDIR=` or similar.  Basically
equivalent to RPMs/debs.

Further, in order to make things fast, we will need a separate
`bare-user` repository in order to perform checkouts quickly via
hardlinks.  We'll then export content into the `archive-z2` repository
for use by client systems.

```
mkdir build-repo
ostree --repo=build-repo init --mode=bare-user
```

You can begin committing those as individual branches:

```
ostree --repo=build-repo commit -b exampleos/x86_64/bash --tree=tar=bash-4.2-bin.tar.gz
ostree --repo=build-repo commit -b exampleos/x86_64/systemd --tree=tar=systemd-224-bin.tar.gz
```

Set things up so that whenever a package changes, you redo the
`commit` with the new package version - conceptually, the branch
tracks the individual package versions over time, and defaults to
"latest".  This isn't required - one could also include the version in
the branch name, and have metadata outside to determine "latest" (or
the desired version).

Now, to construct our final tree:

```
rm -rf exampleos-build
for package in bash systemd; do
  ostree --repo=build-repo checkout -U --union exampleos/x86_64/${package} exampleos-build
done
# Set up a "rofiles-fuse" mount point; this ensures that any processes
# we run for post-processing of the tree don't corrupt the hardlinks.
mkdir -p mnt
rofiles-fuse exampleos-build mnt
# Now run global "triggers", generate cache files:
ldconfig -r mnt
  (Insert other programs here)
fusermount -u mnt
ostree --repo=build-repo commit -b exampleos/x86_64/standard --link-checkout-speedup exampleos-build
```

There are a number of interesting things going on here.  The major
architectural change is that we're using `--link-checkout-speedup`.
This is a way to tell OSTree that our checkout is made via hardlinks,
and to scan the repository in order to build up a reverse `(device,
inode) -> checksum` mapping.

In order for this mapping to be accurate, we needed the `rofiles-fuse`
to ensure that any changed files had new inodes (and hence a new
checksum).

## Migrating content between repositories

Now that we have content in our `build-repo` repository (in
`bare-user` mode), we need to move the `exampleos/x86_64/standard`
branch content into the repository just named `repo` (in `archive-z2`
mode) for export, which will involve zlib compression of new objects.
We likely want to generate static deltas after that as well.

Let's copy the content:

```
ostree --repo=repo pull-local build-repo exampleos/x86_64/standard
```

Clients can now incrementally download new objects - however, this
would also be a good time to generate a delta from the previous
commit.

```
ostree --repo=repo static-delta generate exampleos/x86_64/standard
```

## More sophisticated repository management

Next, see [Repository Management](repository-management.md) for the
next steps in managing content in OSTree repositories.
