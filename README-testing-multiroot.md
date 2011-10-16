Experimenting with multiple roots
---------------------------------

$ qemu-img create debian.img 600M
$ mkfs.ext2 debian.img
$ mkdir debian-mnt
$ mount -o loop debian.img debian-mnt
$ debootstrap wheezy debian-mnt
$ chroot debian-mnt
$ apt-get install linux-image-3.0.0
Control-d
$ cp debian-mnt/boot/vmlinuz* .
$ cp debian-mnt/boot/initrd* .
$ umount debian-mnt

You now have a Debian disk image in debian.img and a kernel+initrd that are bootable with qemu.

Modifying the image
-------------------

The first thing I did was re-mount the image, and move almost everythig
(/boot, /var, /etc), except lost+found to a new directory "r0".

Then I started hacking on the initrd, making understand how to chroot
to "r0".

This means that after booting, every process would be in /r0 -
including any hacktree process.  Assuming objects live in say
/objects, we need some way for hacktree to switch things.  I think
just chroot breakout would work.  This has the advantage the daemon
can continue to use libraries from the active host.

Note there is a self-reference here (as is present in Debian/Fedora
etc.) - the update system would at present be shipped with the system
itself.  Should they be independent?  That has advantages and
disadvantages.  I think we should just try really really hard to avoid
breaking hacktree in updates.

