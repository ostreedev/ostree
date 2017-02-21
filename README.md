libOSTree
======

New! See the docs online at [Read The Docs (OSTree)](https://ostree.readthedocs.org/en/latest/ )

-----

This project is now known as "libOSTree", renamed from "OSTree"; the focus is on
the shared library. However, in most of the rest of the documentation, we will
use the term "OSTree", since it's slightly shorter, and changing all
documentation at once is impractical. We expect to transition to the new name
over time.

libOSTree is a library and suite of command line tools that combines a
"git-like" model for committing and downloading bootable filesystem trees, along
with a layer for deploying them and managing the bootloader configuration.

The core OSTree model is like git in that it checksums individual files and has
a content-addressed-object store. It's unlike git in that it "checks out" the
files via hardlinks, and they should thus be immutable. Therefore, another way
to think of OSTree is that it's just a more polished version
of
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

[meta-updater](https://github.com/advancedtelematic/meta-updater) is
a layer available for [OpenEmbedded](http://www.openembedded.org/wiki/Main_Page)
systems.

[QtOTA](http://doc.qt.io/QtOTA/) is Qt's over-the-air update framework
which uses libostree.

[rpm-ostree](https://github.com/projectatomic/rpm-ostree) is a next-generation
hybrid package/image system for [Fedora](https://getfedora.org/) and [CentOS](https://www.centos.org/),
used by the [Atomic Host](http://www.projectatomic.io/) project.
By default it uses libostree to atomically replicate a base OS (all dependency
resolution is done on the server), but it supports "package layering", where
additional RPMs can be layered on top of the base.  This brings a "best of both worlds""
model for image and package systems.

[flatpak](https://github.com/flatpak/flatpak) uses libostree for desktop
application containers. Unlike most of the other systems here, flatpak does not
use the "libostree host system" aspects (e.g. bootloader management), just the
"git-like hardlink dedup". For example, flatpak supports a per-user OSTree
repository.

[Endless OS](https://endlessos.com/) uses libostree for their host system as
well as flatpak. See
their [eos-updater](https://github.com/endlessm/eos-updater)
and [deb-ostree-builder](https://github.com/dbnicholson/deb-ostree-builder)
projects.

[GNOME Continuous](https://wiki.gnome.org/Projects/GnomeContinuous) is
where OSTree was born - as a high performance continuous delivery/testing
system for GNOME.

Building
--------

Releases are available as GPG signed git tags, and most recent
versions support extended validation using
[git-evtag](https://github.com/cgwalters/git-evtag).

However, in order to build from a git clone, you must update the
submodules.  If you're packaging OSTree and want a tarball, I
recommend using a "recursive git archive" script.  There are several
available online;
[this code](https://github.com/ostreedev/ostree/blob/master/packaging/Makefile.dist-packaging#L11)
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
