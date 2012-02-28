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

Running
-------

Once you have a GRUB entry set up, just reboot.  Log in as root,
there's no password.

One of the first things you'll need to do is add a user.  The userid
must match the one from your "host" distribution, since we share
/home.  For example, let's say my login "walters" has uid/gid 500.
You should run:

$ groupadd -g 500 walters
$ useradd -u 500 -g 500 walters
$ passwd walters
<type in a new password here>

Finally, you can start gdm:

$ /usr/sbin/gdm

Updating
--------

You need to pull the latest updates to your repos (assuming that
you have your repos in /ostree)

$ ostree-pull --repo=/ostree/repo origin gnomeos-3.4-i686-runtime
$ ostree-pull --repo=/ostree/repo origin gnomeos-3.4-i686-devel

And then you need to update your branches: to point to the new
stuff. That is a little cumbersome, and the easiest is to use
the gnomeos-update-branches script that comes with ostree. The
script currently assumes that the repository is in the current
directory.

$ su -
# cd /ostree
# ~/src/ostree/gnomeos/yocto/gnomeos-update-branches.sh
