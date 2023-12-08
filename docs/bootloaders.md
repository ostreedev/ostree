---
nav_order: 11
---

# Bootloaders
{: .no_toc }

1. TOC
{:toc}

## OSTree and bootloaders

The intended design of OSTree is that it just writes new files into `/boot/loader/entries`.  There is a legacy GRUB script (shipped on Fedora as `ostree-grub2`) that is intended only for the cases where the system GRUB does not support the `blscfg` verb.

In the happy path then, the flow of an OS update is just:

- ostree writes a new set of files in `/boot/loader/entries` (during `ostree-finalize-staged.service` on system shutdown)
- On system start, GRUB reads those files

And that's it.

## OSTree and grub

For historical reasons, OSTree defaults to detecting the bootloader; if some GRUB files are present then OSTree will default to executing `grub2-mkconfig`.

[Commented out for now, as this can lead to the system not booting in some cases.]: #
[This can be avoided by setting `sysroot.bootloader=none` (except this should not be set on s390x).]: # 

## GRUB and os-prober

A specific component of GRUB that can significantly impede the reliability of OS updates is the `os-prober` aspect, which scans all system block devices.  If one doesn't care about dual booting, avoiding this is a good idea.

## Anaconda

Until very recently, the Anaconda project only supported setting up the bootloader (e.g. GRUB) on its own, which requires `grub2-mkconfig` etc.  As of recently, Anaconda now [supports bootupd](https://github.com/rhinstaller/anaconda/pull/5298).

## bootupd

As of recently, [the bootupd project](https://github.com/coreos/bootupd/) ships [static grub configs](https://github.com/coreos/bootupd/tree/main/src/grub2) and in this case, the `sysroot.bootloader` should be set to `none` (except on s390x).
And assuming that the system grub has the `blscfg` support, which it does on Fedora derivatives per above.
