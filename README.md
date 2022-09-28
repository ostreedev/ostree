# libostree

This project is now known as "libostree", though it is still appropriate to use
the previous name: "OSTree" (or "ostree"). The focus is on projects which use
libostree's shared library, rather than users directly invoking the command line
tools (except for build systems). However, in most of the rest of the
documentation, we will use the term "OSTree", since it's slightly shorter, and
changing all documentation at once is impractical. We expect to transition to
the new name over time.

As implied above, libostree is both a shared library and suite of command line
tools that combines a "git-like" model for committing and downloading bootable
filesystem trees, along with a layer for deploying them and managing the
bootloader configuration.

The core OSTree model is like git in that it checksums individual files and has
a content-addressed-object store. It's unlike git in that it "checks out" the
files via hardlinks, and they thus need to be immutable to prevent corruption.
Therefore, another way to think of OSTree is that it's just a more polished
version of
[Linux VServer hardlinks](http://linux-vserver.org/index.php?title=util-vserver:Vhashify&oldid=2285).

**Features:**

 - Transactional upgrades and rollback for the system
 - Replicating content incrementally over HTTP via GPG signatures and "pinned TLS" support
 - Support for parallel installing more than just 2 bootable roots
 - Binary history on the server side (and client)
 - Introspectable shared library API for build and deployment systems
 - Flexible support for multiple branches and repositories, supporting
   projects like [Flatpak](https://github.com/flatpak/flatpak) which
   use libostree for applications, rather than hosts.

## Documentation

For more information, see the [project documentation](docs/index.md) or the
[project documentation website](https://ostreedev.github.io/ostree).

## Operating systems and distributions using OSTree

[Apertis](https://www.apertis.org/) uses libostree for their host system as
well as Flatpak. See [update documentation](https://www.apertis.org/guides/ostree/) and
[apertis-update-manager](https://gitlab.apertis.org/pkg/apertis-update-manager)

[Endless OS](https://endlessos.com/) uses libostree for their host system as
well as Flatpak. See
their [eos-updater](https://github.com/endlessm/eos-updater)
and [deb-ostree-builder](https://github.com/dbnicholson/deb-ostree-builder)
projects.

For Debian/apt, see also https://github.com/stb-tester/apt2ostree
and the LWN article [Merkle trees and build systems](https://lwn.net/Articles/821367/).

Fedora derivatives use rpm-ostree (noted below); there are 4 variants using OSTree:

 - [Fedora CoreOS](https://getfedora.org/en/coreos/)
 - [Fedora Silverblue](https://silverblue.fedoraproject.org/)
 - [Fedora Kinoite](https://kinoite.fedoraproject.org/)
 - [Fedora IoT](https://iot.fedoraproject.org/)

Red Hat Enterprise Linux CoreOS is a derivative of Fedora CoreOS, used in [OpenShift 4](https://try.openshift.com/).
The [machine-config-operator](https://github.com/openshift/machine-config-operator/blob/master/docs/OSUpgrades.md)
manages upgrades.  RHEL CoreOS is also the successor to RHEL Atomic Host, which
uses rpm-ostree as well.

[GNOME Continuous](https://wiki.gnome.org/Projects/GnomeContinuous) is
where OSTree was born - as a high performance continuous delivery/testing
system for GNOME.

[GNOME OS](https://os.gnome.org/) is a testing OS that uses libostree for
their host system as well as Flatpak.

[Liri OS](https://liri.io/download/silverblue/) has the option to install
their distribution using ostree.

[TorizonCore](https://developer.toradex.com/torizon/working-with-torizon/torizoncore-technical-overview/) is a Linux distribution for embedded systems that updates via OSTree images delivered via [Uptane](https://uptane.github.io/) and [aktualizr](https://github.com/uptane/aktualizr/).

## Distribution build tools

[meta-updater](https://github.com/advancedtelematic/meta-updater) is
a layer available for [OpenEmbedded](http://www.openembedded.org/wiki/Main_Page)
systems.

[QtOTA](http://doc.qt.io/QtOTA/) is Qt's over-the-air update framework
which uses libostree.

The [BuildStream](https://gitlab.com/BuildStream/buildstream) build and
integration tool supports importing and exporting from libostree repos.

[fedora-iot/otto](https://github.com/fedora-iot/otto) is a tool that helps
ship ostree commits inside Docker/OCI containers and run a webserver
to serve the commits.

Fedora [coreos-assembler](https://github.com/coreos/coreos-assembler) is
the build tool used to generate Fedora CoreOS derivatives.

[debos](https://github.com/go-debos/debos) is a tool-chain for simplifying the
process of building a Debian-based OS image.

## Projects linking to libostree

[rpm-ostree](https://github.com/projectatomic/rpm-ostree) is used by the
Fedora-derived operating systems listed above.  It is a full hybrid
image/package system.  By default it uses libostree to atomically replicate a base OS
(all dependency resolution is done on the server), but it supports "package layering", where
additional RPMs can be layered on top of the base.  This brings a "best of both worlds""
model for image and package systems.

[eos-updater](https://github.com/endlessm/eos-updater) is a daemon that implements updates
on EndlessOS.

[Flatpak](https://github.com/flatpak/flatpak) uses libostree for desktop
application containers. Unlike most of the other systems here, Flatpak does not
use the "libostree host system" aspects (e.g. bootloader management), just the
"git-like hardlink dedup". For example, Flatpak supports a per-user OSTree
repository.

[aktualizr](https://github.com/uptane/aktualizr/) is an [Uptane](https://uptane.github.io/)-conformant software update client library intended for use in automotive and other security-sensitive embedded devices. It uses OSTree to manage the OS of the host device by default.

## Language bindings

libostree is accessible via [GObject Introspection](https://gi.readthedocs.io/en/latest/);
any language which has implemented the GI binding model should work.
For example, Both [pygobject](https://pygobject.readthedocs.io/en/latest/)
and [gjs](https://gitlab.gnome.org/GNOME/gjs) are known to work
and further are actually used in libostree's test suite today.

Some bindings take the approach of using GI as a lower level and
write higher level manual bindings on top; this is more common
for statically compiled languages.  Here's a list of such bindings:

 - [ostree-go](https://github.com/ostreedev/ostree-go/)
 - [ostree-rs](./rust-bindings)

## Building

Releases are available as GPG signed git tags, and most recent
versions support extended validation using
[git-evtag](https://github.com/cgwalters/git-evtag).

However, in order to build from a git clone, you must update the
submodules.  If you're packaging OSTree and want a tarball, I
recommend using a "recursive git archive" script.  There are several
available online;
[this code](https://github.com/ostreedev/ostree/blob/main/ci/Makefile.dist-packaging#L18)
in OSTree is an example.

Once you have a git clone or recursive archive, building is the
same as almost every autotools project:

```
git submodule update --init
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=...
make
make install DESTDIR=/path/to/dest
```

## Contact and discussion forums

OSTree has a [mailing list](https://mail.gnome.org/archives/ostree-list/) and
there is also an `#ostree` channel on [Libera.Chat](ircs://irc.libera.chat/ostree).  However, asynchronous+logged
communication is preferred for nontrivial questions.

## Contributing

See [Contributing](CONTRIBUTING.md).

## Licensing

The licensing for the *code* of libostree can be canonically found in the individual files;
and the overall status in the [COPYING](https://github.com/ostreedev/ostree/blob/main/COPYING)
file in the source.  Currently, that's LGPLv2+.  This also covers the man pages and API docs.

The license for the manual documentation in the `doc/` directory is:
`SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later)`
This is intended to allow use by Wikipedia and other projects.

In general, files should have a `SPDX-License-Identifier` and that is canonical.
