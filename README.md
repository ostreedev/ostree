OSTree
======

New! See the docs online at [Read The Docs (OSTree)](https://ostree.readthedocs.org/en/latest/ )

-----

OSTree is a tool that combines a "git-like" model for committing and
downloading bootable filesystem trees, along with a layer for
deploying them and managing the bootloader configuration.

OSTree is like git in that it checksums individual files and has a
content-addressed-object store.  It's unlike git in that it "checks
out" the files via hardlinks, and they should thus be immutable.
Therefore, another way to think of OSTree is that it's just a more
polished version of
[Linux VServer hardlinks](http://linux-vserver.org/index.php?title=util-vserver:Vhashify&oldid=2285).

**Features:**

 - Atomic upgrades and rollback for the system
 - Replicating content incrementally over HTTP via GPG signatures and "pinned TLS" support
 - Support for parallel installing more than just 2 bootable roots
 - Binary history on the server side (and client)
 - Introspectable shared library API for build and deployment systems

This last point is important - you should think of the OSTree command
line as effectively a "demo" for the shared library.  The intent is that
package managers, system upgrade tools, container build tools and the like
use OSTree as a "deduplicating hardlink store".

Projects using OSTree
---------------------

[rpm-ostree](https://github.com/projectatomic/rpm-ostree) is a tool
that uses OSTree as a shared library, and supports committing RPMs
into an OSTree repository, and deploying them on the client.  This is
appropriate for "fixed purpose" systems.  There is in progress work
for more sophisticated hybrid models, deeply integrating the RPM
packaging with OSTree.

[Project Atomic](http://www.projectatomic.io/) uses rpm-ostree to
provide a minimal host for Docker formatted Linux containers.
Replicating a base immutable OS, then using Docker for applications
meshes together two different tools with different tradeoffs.

[flatpak](https://github.com/alexlarsson/xdg-app) uses OSTree
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

New! See the docs online at [Read The Docs (OSTree)](https://ostree.readthedocs.org/en/latest/ )

Some more information is available on the old wiki page:
<https://wiki.gnome.org/Projects/OSTree>

Contributing
------------

See [Contributing](CONTRIBUTING.md).
