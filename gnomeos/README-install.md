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

After you've installed, you can download updates like this:

$ su -
# cd /ostree
# ostree-pull --repo=repo origin gnomeos-3.4-i686-{runtime,devel}

This just pulls data into your local repository; if you want a
checked-out filesystem root for them, the easiest is to use the
gnomeos-update-branches script that comes with ostree. The script
currently assumes that the repository is in the current directory.

# ~/src/ostree/gnomeos/yocto/gnomeos-update-branches.sh

In the future this will be part of a system administrator oriented
utility (e.g. "ostreeadm").

Next steps
----------

Now that you have your OSTree install and know how to update
it, you probably want to do something useful with it. OSTrees
main mission is to assist developers and testers, so lets explain
how it lets a tester isolate a problem.

Bisecting
---------

Say you've updated your OSTree installation, and after booting
it, you notice a new problem. What now ? You probably want to
identify exactly when this problem was introduced. A good technique
for doing so is known as 'bisection'. Here is how it works:

[...sadly I don't know how to do this with OSTree]

Once you have identified the binary revision that introduced the
problem, you can go one step further. OSTree stores the source
revisions that each commit has been built from, so you can retrieve
the exact source changes that are likely responsible for
the problem you've just tracked down.

[...fill me in]

Local changes
-------------

If are a developer, at this point you may try your hand at fixing
the problem in the source. Of course, you want to build the module
with your change, and add it to your OSTree installation to verify
that it fixes the problem. Here is how:

[...?]

