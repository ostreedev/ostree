---
nav_order: 10
---

# Using composefs with OSTree
{: .no_toc }

1. TOC
{:toc}

## composefs

The [composefs](github.com/containers/composefs) project is a new
hybrid Linux stacking filesystem that provides many benefits when
used for bootable host systems, such as a strong story for integrity.

At the current time, integration of composefs and ostree is experimental.
[This issue](https://github.com/ostreedev/ostree/issues/2867) tracks the latest status.

### Enabling composefs (unsigned)

When building a disk image *or* to transition an existing system, run:

```
ostree config --repo=/ostree/repo set ex-integrity.composefs yes
```

This will ensure that any future deployments (e.g. created by `ostree admin upgrade`)
have a `.ostree.cfs` file in the deployment directory which is a mountable
composefs metadata file, with a "backing store" directory also shared with the current `/ostree/repo/objects`.

**IMPORTANT** The integration with composefs is experimental and subject to change.  Please
try it and report issues but do not deploy to production systems yet.

## Comparison with other approaches

There is also support for using [IMA](ima.md) with ostree.  In short, composefs
provides much stronger and more efficient integrity:

- composefs validates an entire filesystem tree, not just individual files
- composefs makes files actually read-only, whereas IMA does not by default
- composefs uses fs-verity which does on-demand verification

## Further references

- https://github.com/containers/composefs
- https://www.kernel.org/doc/html/next/filesystems/fsverity.html

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

