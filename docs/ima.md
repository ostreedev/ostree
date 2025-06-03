---
nav_order: 110
---

# Using Linux IMA with OSTree
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

## Linux IMA

The [Linux Integrity Measurement Architecture](https://sourceforge.net/p/linux-ima/wiki/Home/)
provides a mechanism to cryptographically sign the digest of a regular
file, and policies can be applied to e.g. require that code executed
by the root user have a valid signed digest.

The alignment between Linux IMA and OSTree is quite strong.  OSTree
provides a content-addressable object store, where files are intended
to be immutable.  This is implemented with a basic read-only bind mount.

While IMA does not actually prevent mutating files, any changed (or unsigned)
files would (depending on policy) not be readable or executable.

## IMA signatures and OSTree checksum

Mechanically, IMA signatures appear as a `security.ima` extended attribute
on the file. This is a signed digest of just the file content (i.e. not including file metadata).

OSTree's checksums in contrast include not just the file content, but also 
metadata such as uid, gid and mode and extended attributes;

Together, this means that adding an IMA signature to a file in the OSTree
model appears as a new object (with a new digest).  A nice property is that
this enables the transactional addition (or removal) of IMA signatures.
However, adding IMA signatures to files that were previously unsigned
also today duplicates disk space.

## Signing 

To apply IMA signatures to an OSTree commit, there is an `ima-sign`
command implemented currently in the [ostree-rs-ext](https://github.com/ostreedev/ostree-rs-ext/)
project.

### Generating a key

There is documentation for this in `man evmctl` and the upstream IMA
page; we will not replicate it here.

### Signing a commit

`ima-sign` requires 4 things:

- An OSTree repository (could be any mode; `archive` or e.g. `bare-user`)
- A ref or commit digest (e.g. `exampleos/x86_64/stable`)
- A digest algorithm (usually `sha256`, but you may use e.g. `sha512` as well)
- An RSA private key

You can then add IMA signatures to all regular files in the commit:

```
$ ostree-ext-cli ima-sign --repo=repo exampleos/x86_64/stable sha256 /path/to/key.pem
```

Many different choices are possible for the signing model.  For example,
your build system could store individual components/packages in their own
ostree refs, and sign them at build time.  This would avoid re-signing
all binaries when creating production builds.  Although note you
still likely want to sign generated artifacts from unioning individual
components, such as a dpkg/rpm database or equivalent and cache files
such as the `ldconfig` and GTK+ icon caches, etc.

### Applying a policy

Signing a commit by itself will have little to no effect.  You will also
need to include in your builds an [IMA policy](https://sourceforge.net/p/linux-ima/wiki/Home/#defining-an-lsm-specific-policy).

### Linux EVM

The EVM subsystem builds on IMA, and adds another signature which 
covers most file data, such as the uid, gid and mode and selected
security-relevant extended attributes.

This is quite close to the ostree native checksum - the ordering
of the fields is different so the checksums are physically different, but
logically they are very close.

However, the focus of the EVM design seems to mostly
be on machine-specific signatures with keys stored in a TPM.
Note that doing this on a per-machine basis would add a new
`security.evm` extended attribute, and crucially that
*changes the ostree digest* - so from ostree's perspective,
these objects will appear corrupt.

In the future, ostree may learn to ignore the presence of `security.evm`
extended attributes.

There is also some support for "portable" EVM signatures - by
default, EVM signatures also include the inode number and generation
which are inherently machine-specific.

A future ostree enhancement may instead also focus on supporting
signing commits with these "portable" EVM signatures in addition to IMA.

## Further references

- https://sourceforge.net/p/linux-ima/wiki/Home/
- https://en.opensuse.org/SDB:Ima_evm
- https://wiki.gentoo.org/wiki/Integrity_Measurement_Architecture
- https://fedoraproject.org/wiki/Changes/Signed_RPM_Contents
- https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/managing_monitoring_and_updating_the_kernel/enhancing-security-with-the-kernel-integrity-subsystem_managing-monitoring-and-updating-the-kernel
