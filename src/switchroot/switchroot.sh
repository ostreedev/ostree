#!/bin/sh

# This demonstration script is an implementation in shell
# similar to ostree-prepare-root.c.  For a bit more information,
# see adapting-existing.md.

## the ostree boot parameter is avaialbe during the init
env | grep ostree
# ostree=/ostree/boot.1/.../.../0
## bind mount the ostree deployment to prepare it for move
mount --bind $sysroot$ostree $sysroot$ostree
## bind mount read-only /usr
mount --bind $sysroot$ostree/usr $sysroot$ostree/usr
mount --bind -o remount,ro $sysroot$ostree/usr $sysroot$ostree/usr
## bind mount the physical root
mount --bind $sysroot $sysroot$ostree/sysroot
## bind mount the var directory which is preserved between deployments
mount --bind $sysroot/ostree/deploy/os/var $sysroot$ostree/var
## make sure target directories are present within var
cd $sysroot$ostree/var
mkdir -p roothome mnt opt home
cd -
## move the deployment to the sysroot
mount --move $sysroot$ostree $sysroot
## after these the init system should start the switch root process
