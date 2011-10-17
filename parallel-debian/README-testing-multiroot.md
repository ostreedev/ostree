
Experimenting with multiple roots
---------------------------------

<http://wiki.debian.org/QEMU#Setting_up_a_testing.2BAC8-unstable_system>

Follow the steps for making a disk image, downloading the business
card CD, booting it in QEMU and running through the installer.

Test that the image works after installation too, before you start
modifying things below!  Remember to remove the -cdrom and -boot
options from the installation QEMU command.

Modifying the image
-------------------

You now have a disk image in debian.img, and the first partition
should be ext4.

The first thing I did was mount the image, and move almost everythig
(/boot, /var, /etc), except lost+found to a new directory "r0".

	$ mkdir /mnt/debian
	$ modprobe nbd max_part=8
	$ qemu-nbd --connect=/dev/nbd0 debian.qcow
	$ mount /dev/nbd0p1 /mnt/debian/
	$ cd /mnt/debian
	$ mkdir r0
	$ DIRS="bin boot dev etc lib lib64 media mnt opt proc root run sbin selinux srv sys tmp usr var"
	$ mv $DIRS r0 

Now with it still mounted, we need to move on to the next part -
modifying the initrd.  

Then I started hacking on the initrd, making understand how to chroot
to "r0".  I ended up with two patches - one to util-linux, and one to
the "init" script in Debian's initrd.

See:
    0001-switch_root-Add-subroot-option.patch
    0001-Add-support-for-subroot-option.patch

$ git clone --depth=1 git://github.com/karelzak/util-linux.git
$ cd util-linux
$ patch -p1 -i ../0001-switch_root-Add-subroot-option.patch
$ ./autogen.sh; ./configure ; make

Now you have a modified "sys-utils/switch_root" binary.  Let's next
patch the initrd and rebuild it:

Make a backup:

	$ mkdir initrd
	$ cp /mnt/debian/boot/initrd.img-3.0.0-1-amd64{,.orig}

Unpack, and patch:

	$ zcat /mnt/debian/boot/initrd.img-3.0.0-1-amd64 | (cd initrd; cpio -d -i -v)
	$ (cd initrd && patch -p1 -i ../0001-Add-support-for-subroot-option.patch)

Repack:

	$ (cd initrd; find | cpio -o -H newc) | gzip > /mnt/debian/boot/initrd.img-3.0.0-1-amd64.new
	$ mv /mnt/debian/boot/initrd.img-3.0.0-1-amd64{.new,}

Running hacktree inside the system
----------------------------------

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

