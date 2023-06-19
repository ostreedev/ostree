---
nav_order: 10
---

# Using composefs with OSTree
{: .no_toc }

1. TOC
{:toc}

## composefs

The [composefs](https://github.com/containers/composefs) project is a new
hybrid Linux stacking filesystem that provides many benefits when
used for bootable host systems, such as a strong story for integrity.

At the current time, integration of composefs and ostree is experimental.
[This issue](https://github.com/ostreedev/ostree/issues/2867) tracks the latest status.

### Enabling composefs (unsigned)

When building a disk image *or* to transition an existing system, run:

```
ostree config --repo=/ostree/repo set ex-integrity.composefs true
```

This will ensure that any future deployments (e.g. created by `ostree admin upgrade`)
have a `.ostree.cfs` file in the deployment directory which is a mountable
composefs metadata file, with a "backing store" directory that is
shared with the current `/ostree/repo/objects`.

### Kernel argument ot-composefs

The `ostree-prepare-root` binary will look for a kernel argument called `ot-composefs`.

The default value is `maybe` (this will likely become a build and initramfs-configurable option)
in the future too.

The possible values are:

- `off`: Never use composefs
- `maybe`: Use composefs if supported and there is a composefs image in the deployment directory
- `on`: Require composefs
- `digest=<sha256>`: Require the mounted composefs image to have a particular digest
- `signed`: This option will be documented in the future; don't use it right now

### Injecting composefs digests

When generating an OSTree commit, there is a CLI switch `--generate-composefs-metadata`
and a corresponding C API `ostree_repo_commit_add_composefs_metadata`.  This will
inject the composefs digest as metadata into the ostree commit under a metadata
key `ostree.composefs.v0`.  Because an OSTree commit can be signed, this allows
covering the composefs fsverity digest with a signature.  

At the current time, ostree does not directly support verifying the signature on
the commit object before mounting, but that is in progress.

## Requirements

The current default composefs integration in ostree does not have any requirements
from the underlying kernel and filesystem other than having the following
kernel options set:

- `CONFIG_OVERLAY_FS`
- `CONFIG_BLK_DEV_LOOP`
- `CONFIG_EROFS_FS`

At the current time, there are no additional userspace runtime requirements.

## Status

**IMPORTANT** The integration with composefs is experimental and subject to change.  Please
try it and report issues but do not deploy to production systems yet.

## Comparison with other approaches

There is also support for using [IMA](ima.md) with ostree.  In short, composefs
provides much stronger and more efficient integrity:

- composefs validates an entire filesystem tree, not just individual files
- composefs makes files actually read-only, whereas IMA does not by default
- composefs uses fs-verity which does on-demand verification (IMA by default does a full readahead of every file accessed, though IMA can also use fs-verity as a backend)

## Further references

- https://github.com/containers/composefs
- https://www.kernel.org/doc/html/next/filesystems/fsverity.html

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

