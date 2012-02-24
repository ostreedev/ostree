Overview
--------

http://ostree.gnome.org is the sole build/deploy server right now.

Notably http://ostree.gnome.org/repo is an OSTree repo which holds
binaries. 

To install, right now you need to build the 'ostree' git module
somehow.  I personally use jhbuild, but you could make an RPM/.deb or
whatever too.

Replace ~/src with whereever you keep source code.

$ cd ~/src
$ git clone git://git.gnome.org/ostree
$ cd ostree
$ jhbuild buildone -nf $(basename $(pwd))

Now we need to run the install script as root.

$ su -
# ~/src/ostree/gnomeos/yocto/gnomeos-install.sh

Now you may need to edit your GRUB configuration.  This part varies
for GRUB 1 versus GRUB 2.  The GRUB 2 bits in "15_ostree" don't really
work yet.  I'm just manually writing GRUB 1 entries.



