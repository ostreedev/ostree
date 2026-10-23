---
nav_order: 125
---

# Extension BLS keys and staged deployments
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

Bootloader entries written by OSTree follow the
[Boot Loader Specification](https://uapi-group.org/specifications/specs/boot_loader_specification/)
(BLS). Besides the keys the specification defines, an entry can carry
*extension* keys that tools use to keep their own bookkeeping next to
the kernel arguments. This page describes how OSTree treats those keys
and what a tool that stages deployments has to do to keep them intact.

OSTree does not interpret extension keys. The consumer that defines them
today is bootc, which records which kernel arguments a *source* such as
TuneD owns in `x-options-source-NAME` keys; see
[`bootc loader-entries set-options-for-source`](https://github.com/bootc-dev/bootc/blob/main/docs/src/man/bootc-loader-entries-set-options-for-source.8.md).
Everything below applies to any extension key.

## Standard and extension keys

An entry written by OSTree looks like this:

```
title Fedora Linux 43 (ostree:fedora:0)
version 1
options root=UUID=... rw ostree=/ostree/boot.1/fedora/<checksum>/0 nohz=full isolcpus=1-3
linux /ostree/fedora-<checksum>/vmlinuz-6.17.1-300.fc43.x86_64
initrd /ostree/fedora-<checksum>/initramfs-6.17.1-300.fc43.x86_64.img
x-options-source-tuned nohz=full isolcpus=1-3
x-options-source-dracut 
```

The *standard* keys are `title`, `version`, `options`, `linux`,
`initrd`, `devicetree`, `fdtdir`, `aboot` and `abootcfg`. OSTree owns
them and rebuilds them every time it writes an entry, from the
deployment's kernel layout and kernel arguments. Every other key is an
*extension* key: OSTree preserves it verbatim and never looks at its
value. To see which ones a system has:

```
grep '^x-' /boot/loader/entries/ostree-*.conf
```

A key with an empty value is written as the key followed by a single
space and nothing else, as `x-options-source-dracut` in the example, and
is read back as an empty string. That is how a key is retired. There is
no separate remove operation, so a tool that stops owning a key sets it
to an empty value. Note that a line consisting of the key alone, without
the trailing space, is not a valid entry line and is dropped when the
entry is read.

## Staged deployments

A non-staged deployment (`ostree admin deploy`) is created with an entry
that carries no extension keys. A tool adds keys to it afterwards by
setting them on the deployment's bootconfig and writing the deployments
out again, the same way kernel arguments are edited, and the entry on
disk changes right away.

A [staged deployment](deployment.md#staged-deployments) only gets its
entry at finalization, when the system shuts down. Until then, everything
finalization needs is kept in a private file under `/run/ostree`, and
since 2026.1 that includes the extension keys. The keys come from the
*merge deployment* a tool passes in when staging, normally the booted
one, because the new deployment's own bootconfig is built from scratch
at finalization.

Only one staged deployment exists at a time. Staging again in the same
boot replaces it, and the tool doing so may be a different one that
knows nothing about extension keys: `rpm-ostree kargs` after
`bootc loader-entries set-options-for-source`, for example. OSTree
decides which keys the new staging carries as follows:

1. If the tool set an extension key on the merge deployment's
   bootconfig before staging, the keys of that bootconfig are used as
   they are. That includes the keys of the merge deployment's on-disk
   entry that the tool did not touch. Keys that only existed in an
   earlier staging of this boot are dropped unless the tool set them
   again.
2. Otherwise, if a deployment staged earlier in this boot carries keys,
   those are used. They are newer than whatever is on disk.
3. Otherwise the keys of the merge deployment's on-disk entry are used.

One set is used whole; sets are never merged key by key. The reasoning
behind these rules and the scenarios they cover are documented with the
implementation in `src/libostree/ostree-bootconfig-parser.c`.

## What a tool has to do

If your tool **manages** extension keys:

* Before staging, read the current state: the booted entry *and*, if a
  deployment is already staged, its keys. Through libostree that is
  `ostree_sysroot_get_staged_deployment()` followed by
  `ostree_deployment_get_bootconfig()`.
* Compute the complete set you want and set every key on the merge
  deployment's bootconfig with `ostree_bootconfig_parser_set()` before
  you stage. Do not expect OSTree to accumulate keys across calls. To
  retire a key, set it to an empty value.
* Build the kernel arguments on the deployment you are replacing, the
  staged one if it exists, or a change that was only staged is lost.
  This part is outside OSTree's control.

If your tool only changes the tree or the kernel arguments, there is
nothing to do: the keys that were staged before, or that are on disk,
are carried over.

## Compatibility

The behaviour described here requires libostree 2026.5.

* 2026.1, 2026.3 and 2026.4 carry the keys of a staged deployment, but
  when a tool that does not manage keys stages again in the same boot
  they fall back to the booted entry's keys, so the staged ones are lost.
* 2026.2 merged the sets key by key. A tool that does not manage keys
  kept the staged keys, but a stale value staged earlier could override
  a managing tool's later update.
* Older versions do not carry extension keys through staging at all; a
  staged deployment's entry is written without them.

The extension keys are an optional part of the staged-deployment data,
so upgrading or downgrading libostree while a deployment is staged is
safe; readers that do not know about them ignore them.

## Known limitations

* A staged deployment is replaced whole. Two tools that both manage keys
  must each read the current staged state before writing, or the earlier
  change is lost together with its keys.
* Retiring a key leaves it in the entry with an empty value; there is no
  way to drop a key from an entry entirely.
* With staged deployments, extension keys are inherited from the booted
  entry into every new deployment of the stateroot. They are not scoped
  per deployment.
* If something edits `options` directly, OSTree does not reconcile it
  with the bookkeeping a tool keeps in extension keys.
