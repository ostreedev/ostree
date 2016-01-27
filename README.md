OSTree
======

OSTree is a tool that combines a "git-like" model for committing and
downloading bootable filesystem trees, along with a layer for
deploying them and managing the bootloader configuration.

Traditional package managers (dpkg/rpm) build filesystem trees on the
client side.  In contrast, the primary focus of OSTree is on
replicating trees composed on a server.

**Features:**

 - Atomic upgrades and rollback
 - GPG signatures and "pinned TLS" support
 - Support for parallel installing more than just 2 bootable roots
 - Binary history on the server side
 - Introspectable shared library API for build and deployment systems

Projects using OSTree
---------------------

[rpm-ostree](https://github.com/projectatomic/rpm-ostree) is a tool
that uses OSTree as a shared library, and supports committing RPMs
into an OSTree repository, and deploying them on the client.

[Project Atomic](http://www.projectatomic.io/) uses rpm-ostree
to provide a minimal host for Docker formatted Linux containers.

[xdg-app](https://github.com/alexlarsson/xdg-app) uses OSTree 
for desktop application containers.

[GNOME Continuous](https://wiki.gnome.org/Projects/GnomeContinuous) is
a custom build system designed for OSTree, using
[OpenEmbedded](http://www.openembedded.org/wiki/Main_Page) in concert
with a custom build system to do continuous delivery from hundreds of
git repositories.

Building
--------

Releases are available as GPG signed git tags, and most recent
versions support extended validation using
[git-evtag](https://github.com/cgwalters/git-evtag).

However, in order to build from a git clone, you must update the
submodules.  If you're packaging OSTree and want a tarball, I
recommend using a "recursive git archive" script.  There are several
available online;
[this code](https://git.gnome.org/browse/ostree/tree/packaging/Makefile.dist-packaging#n11)
in OSTree is an example.

Once you have a git clone or recursive archive, building is the
same as almost every autotools project:

```
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=...
make
make install DESTDIR=/path/to/dest
```

More documentation
------------------

Some more information is available on the old wiki page:
https://wiki.gnome.org/Projects/OSTree

The intent is for that wiki page content to be migrated into Markdown
in this git repository.

Contributing
------------

See [Contributing](CONTRIBUTING.md).
