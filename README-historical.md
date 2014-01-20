This file is outdated, but some of the text here is still useful for
historical context.  I'm preserving it (explicitly still in the tree)
for posterity.

OSTree
======

Problem statement
-----------------

Hacking on the core operating system is painful - this includes most
of GNOME from upower and NetworkManager up to gnome-shell.  I want a
system that matches these requirements:

1. Does not disturb your existing OS
2. Is not terribly slow to use
3. Shares your $HOME - you have your data
4. Allows easy rollback
5. Ideally allows access to existing apps

Comparison with existing tools
------------------------------

 - Virtualization

    Fails on points 2) and 3).  Actually qemu-kvm can be pretty fast,
    but in a lot of cases there is no substitute for actually booting
    on bare metal; GNOME 3 really needs some hardware GPU
    acceleration.

 - Rebuilding distribution packages

    Fails on points 1) and 4).  Is also just very annoying: dpkg/rpm
    both want tarballs, which you don't have since you're working from
    git.  The suggested "mock/pbuilder" type chroot builds are *slow*.
    And even with non-chroot builds there is lots of pointless build
    wrapping going on.  Both dpkg and rpm also are poor at helping you
    revert back to the original system.

    All of this can be scripted to be less bad of course - and I have
    worked on such scripts.  But fundamentally you're still fighting
    the system, and if you're hacking on a lowlevel library like say
    glib, you can easily get yourself to the point where you need a
    recovery CD - at that point your edit/compile/debug cycle is just
    too long.

 - "sudo make install"

    Now your system is in an undefined state.  You can use e.g. rpm
    -qV to try to find out what you overwrote, but neither dpkg nor
    rpm will help clean up any files left over that aren't shipped by
    the old package.  

    This is most realistic option for people hacking on system
    components currently, but ostree will be better.

 - LXC / containers

   Fails on 3), and 4) is questionable.  Also shares the annoying part
   of rebuilding distribution packages. LXC is focused on running
   multiple server systems at the *same time*, which isn't what we
   want (at least, not right now), and honestly even trying to support
   that for a graphical desktop would be a lot of tricky work, for
   example getting two GDM instances not to fight over VT
   allocations. But some bits of the technology may make sense to use.

 - jhbuild + distribution packages

    The state of the art in GNOME - but can only build non-root things -
    this means you can't build NetworkManager, and thus are permanently
    stuck on whatever the distro provides.

Who is ostree for?
------------------

First - operating system developers and testers.  I specifically keep
a few people in mind - Dan Williams and Eric Anholt, as well as myself
obviously.  For Eric Anholt, a key use case for him is being able to
try out the latest gnome-shell, and combine it with his work on Mesa,
and see how it works/performs - while retaining the ability to roll
back if one or both breaks.

The rollback concept is absolutely key for shipping anything to
enthusiasts or knowledable testers.  With a system like this, a tester
can easily perform a local rollback - something just not well
supported by dpkg/rpm.  (What about Conary?  See below.)

Also, distributing operating system trees (instead of packages) gives
us a sane place to perform automated QA **before** we ship it to
testers.  We should never be wasting these people's time.

Even better, this system would allow testers to *bisect* across
operating system builds, and do so very efficiently.

The core idea - chroots
-----------------------

chroots are the original lightweight "virtualization".  Let's use
them.  So basically, you install a mainstream distribution (say
Debian).  It has a root filesystem like this:

		/usr
		/etc
		/home
		...

Now, what we can do is have a system that installs chroots as a subdirectory
of the root, like:

		/ostree/gnomeos-3.0-opt-393a4555/{usr,etc,sbin,...}
		/ostree/gnomeos-3.2-opt-7e9788a2/{usr,etc,sbin,...}

These live in the same root filesystem as your regular distribution
(Note though, the root partition should be reasonably sized, or
hopefully you've used just one big partition).

You should be able to boot into one of these roots.  Since ostree
lives inside a distro created partition, a tricky part here is that we
need to know how to interact with the installed distribution's grub.
This is an annoying but tractable problem.

First, we install a kernel+initramfs alongside the distribution's.
Then, we have a "trampoline" ostree-init binary which is statically
linked, and boot the kernel with init=/ostree/ostree-init.  This then
takes care of chrooting and running the init binary.

An important note here is that we bind mount the real /home.  This
means you have your data.  This also implies we share uid/gid, so
/etc/passwd will have to be in sync.  Probably what we'll do is have a
script to pull the data from the "host" OS.

I've decided for now to move /var into /ostree to avoid sharing it
with the "host" distribution, because in practice we're likely
to hit incompatibilities.

Do note however /etc lives *inside* the OSTree; it's presently
versioned and readonly like everything else.

On a pure OSTree system, the filesystem layout will look like this:

		.
		|-- boot
		|-- home
		|-- ostree
		|   |-- var
		|   |-- current -> gnomeos-3.2-opt-7e9788a2
		|   |-- gnomeos-3.0-opt-393a4555
		|   |   |-- etc
		|   |   |-- lib
		|   |   |-- mnt
		|   |   |-- proc
		|   |   |-- run
		|   |   |-- sbin
		|   |   |-- srv
		|   |   |-- sys
		|   |   `-- usr
		|   `-- gnomeos-3.2-opt-7e9788a2
		|       |-- etc
		|       |-- lib
		|       |-- mnt
		|       |-- proc
		|       |-- run
		|       |-- sbin
		|       |-- srv
		|       |-- sys
		|       `-- usr
		|-- root
		

Making this efficient
---------------------

One of the first things you'll probably ask is "but won't that use a
lot of disk space"?  Indeed, it will, if you just unpack a set of RPMs
or .debs into each root.

Besides chroots, there's another old Unix idea we can take advantage
of - hard links.  These allow sharing the underlying data of a file,
with the tradeoff that changing any one file will change all names
that point to it.  This mutability means that we have to either:

 1. Make sure everything touching the operating system breaks hard links
    This is probably tractable over a long period of time, but if anything
    has a bug, then it corrupts the file effectively.
 2. Make the core OS read-only, with a well-defined mechanism for mutating
    under the control of ostree.

I chose 2.

A userspace content-addressed versioning filesystem
---------------------------------------------------

At its very core, that's what ostree is.  Just like git.  If you
understand git, you know it's not like other revision control systems.
git is effectively a specialized, userspace filesystem, and that is a
very powerful idea.

At the core of git is the idea of "content-addressed objects".  For
background on this, see <http://book.git-scm.com/7_how_git_stores_objects.html>

Why not just use git?  Basically because git is designed mainly for
source trees - it goes to effort to be sure it's compressing text for
example, under the assumption that you have a lot of text.  Its
handling of binaries is very generic and unoptimized.

In contrast, ostree is explicitly designed for binaries, and in
particular one type of binary - ELF executables (or it will be once we
start using bsdiff).  

Another big difference versus git is that ostree uses hard links
between "checkouts" and the repository.  This means each checkout uses
almost no additional space, and is *extremely* fast to check out.  We
can do this because again each checkout is designed to be read-only.

So we mentioned above there are:

		/ostree/gnomeos-3.2-opt-7e9788a2
		/ostree/gnomeos-3.2-opt-393a4555

There is also a "repository" that looks like this:

		/ostree/repo/objects/17/a95e8ca0ba655b09cb68d7288342588e867ee0.file
		/ostree/repo/objects/17/68625e7ff5a8db77904c77489dc6f07d4afdba.meta
		/ostree/repo/objects/17/cc01589dd8540d85c0f93f52b708500dbaa5a9.file
		/ostree/repo/objects/30
		/ostree/repo/objects/30/6359b3ca7684358a3988afd005013f13c0c533.meta
		/ostree/repo/objects/30/8f3c03010cedd930b1db756ce659c064f0cd7f.meta
		/ostree/repo/objects/30/8cf0fd8e63dfff6a5f00ba5a48f3b92fb52de7.file
		/ostree/repo/objects/30/6cad7f027d69a46bb376044434bbf28d63e88d.file

Each object is either metadata (like a commit or tree), or a hard link
to a regular file.

Note that also unlike git, the checksum here includes *metadata* such
as uid, gid, permissions, and extended attributes.  (It does not include
file access times, since those shouldn't matter for the OS)

This is another important component to allowing the hardlinks.  We
wouldn't want say all empty files to be shared necessarily.  (Though
maybe this is wrong, and since the OS is readonly, we can make all
files owned by root without loss of generality).

However this tradeoff means that we probably need a second index by
content, so we don't have to redownload the entire OS if permissions
change =)

Atomic upgrades, rollback
-------------------------

OSTree is designed to atomically swap operating systems - such that
during an upgrade and reboot process, you either get the full new
system, or the old one.  There is no "Please don't turn off your
computer".  We do this by simply using a symbolic link like:

/ostree/current -> /ostree/gnomeos-3.4-opt-e3b0c4429

Where gnomeos-e3b0c4429 has the full regular filesystem tree with usr/
etc/ directories as above.  To upgrade or rollback (there is no
difference internally), we simply check out a new tree into
gnomeos-b90ae4763 for example, then swap the "current" symbolic link,
then remove the old tree.

But does this mean you have to reboot for OS upgrades?  Very likely,
yes - and this is no different from RPM/deb or whatever.  They just
typically lie to you about it =)

A typical model with RPM/deb is to unpack the new files, then use some
IPC mechanism (SIGHUP, a control binary like /usr/sbin/apachectl) to
signal the running process to reload.  There are multiple problems
with this - one is that in the new state, daemon A may depend on the
updated configuration in daemon B.  This may not be particularly
common in default configurations, but it's highly likely that that
some deployments will have e.g. apache talking to a local MySQL
instance.  So you really want to do is only apply the updated
configuration when all the files are in place; not after each RPM or
.deb is installed.

What's even harder is the massive set of race conditions that are
possible while RPM/deb are in the process of upgrading.  Cron jobs are
very likely to hit this.  If we want the ability to apply updates to a
live system, we could first pause execution of non-upgrade userspace
tasks.  This could be done via SIGSTOP for example.  Then, we can swap
around the filesystem tree, and then finally attempt to apply updates
via SIGHUP, and if possible, restart processes.

Configuration Management
------------------------

By now if you've thought about this problem domain before, you're wondering
about configuration management.  In other words, if the OS is read only,
how do I edit /etc/sudoers?

Well, have you ever been a system administrator on a zypper/yum
system, done an RPM update, which then drops .rpmnew files in your
/etc/ that you have to go and hunt for with "find" or something, and
said to yourself, "Wow, this system is awesome!!!" ?  Right, that's
what I thought.

Configuration (and systems) management is a tricky problem, and I
certainly don't have a magic bullet.  However, one large conceptual
improvement I think is defaulting to "rebase" versus "merge".

This means that we won't permit direct modification of /etc - instead,
you HAVE to write a script which accomplishes your goals.  To generate
a tree, we check out a new copy, then run your script on top.

If the script fails, we can roll back the update, or drop to a shell
if interactive.

However, we also need to consider cases where the administrator
modifies state indirectly by a program.  Think "adduser" for example.

Possible approaches:

 1. Patch all of these programs to know how to write to the writable
    location, instead of the R/O bind mount overlay.
 2. Move the data to /var

What about "packages"?
----------------------

There are several complex and separate issues hiding in this seemingly
simple question. 

I think OSTree always makes sense to use as a core operating system
builder and updater.  By "core" here I mean the parts that aren't
removable.  Debian has Essential: yes, any other distribution has this
too implicitly in the set of dependencies for their updater tool.

Now, let me just say I will absolutely support using something like
apt/yum/zypper (and consequently deb/rpm) on top of OSTree.  This
isn't trivial, but there aren't any conceptual issues.

Concretely for example, RPM or .deb might make sense as a delivery
vehicle for third party OS extensions.  A canoncial example is the
NVidia graphics driver.

If one is using OSTree to build an *operating system*, then there has
to be some API for applications.  And that demands its own targeted
solution - something like an evolved glick (zeroinstall is also
similar).

Current package systems are totally broken for application deployment
though; for example, they will remove files away from under running
applications on update.  And we clearly need the ability to install
and upgrade applications without rebooting the OS.

Details of RPM installation
---------------------------

We should be able to install LSB rpms.  This implies providing "rpm".
The tricky part here is since the OS itself is not assembled via RPMs,
we need to fake up a database of "provides" as if we were.  Even
harder would be maintaining binary compatibilty with any arbitrary
%post scripts that may be run.

What about BTRFS?  Doesn't it solve everything?
-----------------------------------------------

In short, BTRFS is not a magic bullet, but yes - it helps
significantly.  The obvious thing to do is layer BTRFS under dpkg/rpm,
and have a separate subvolume for /home so rollbacks don't lose your
data.  See e.g.
<http://fedoraproject.org/wiki/Features/SystemRollbackWithBtrfs>

As a general rule an issue with the BTRFS is that it can't roll back
just changes to things installed by RPM (i.e. what's in rpm -qal).

For example, it's possible to e.g. run yum update, then edit something
in /etc, reboot and notice things are broken, then roll back and have
silently lost your changes to /etc.

Another example is adding a new binary in /usr/local.  You could say,
"OK, we'll use subvolumes for those!".  But then what about /var (and
your VM images that live in /var/lib/libvirt ?)

Finally, probably the biggest disadvantage of the rpm/dpkg + BTRFS
approach is it doesn't solve the race conditions that happen when
unpacking packages into the live system.  This problem is really
important to me.

Note though ostree can definitely take advantage of BTRFS features!
In particular, we could use "reflink"
<http://lwn.net/Articles/331808/> instead of hard links, and avoid
having the object store corrupted if somehow the files are modified
directly.

Other systems
-------------

I've spent a long time thinking about this problem, and here are some
of the other possible solutions out there I looked at, and why I
didn't use them:

 - Git: <http://git-scm.com/>

    Really awesome, and the core inspiration here.  But like I mentioned
    above, not at all designed for binaries - we can make different tradeoffs.

 - bup: <https://github.com/apenwarr/bup>

    bup is cool.  But it shares the negative tradeoffs with git, though it
    does add positives of its own.  It also inspired me.

 - git-annex: <http://git-annex.branchable.com/git-annex/>

   Looks interesting; I think this will share the same negative tradeoffs with git
   as far as using it for an OS goes.

 - schroot: <http://www.debian-administration.org/articles/566>

   Like LXC/containers, but just using a chroot.

 - NixOS: <http://nixos.org/>

    The NixOS people have a lot of really good ideas, and they've definitely
    thought about the problem space.  However, their approach of checksumming
    all inputs to a package is pretty wacky.  I don't see the point, and moreover
    it uses gobs of disk space.

 - Conary: <http://wiki.rpath.com/wiki/Conary:Updates_and_Rollbacks>

   If rpm/dpkg are like CVS, Conary is closer to Subversion.  It's not
   bad, but ostree is better than it for the exact same reasons git
   is better than Subversion.

 - BTRFS: <http://en.wikipedia.org/wiki/Btrfs>

   See above.

 - Solaris IPS: <http://hub.opensolaris.org/bin/view/Project+pkg/>

   Rollback is ZFS level, so I think this shares the same tradeoffs as BTRFS+RPM/deb.
   They probably have some vertical integration though which definitely helps.
   Obviously we can't use ZFS.

 - Jhbuild: <https://live.gnome.org/Jhbuild>

   What we've been using in GNOME, and has the essential property of allowing you
   to "fall back" to a stable system.  But ostree will blow it out of the water.

Development
-----------

 - OSTree wiki page: <https://live.gnome.org/OSTree>

 - ostbuild wiki page: <https://live.gnome.org/OSTree/Ostbuild>

 - Git repository: <http://git.gnome.org/browse/ostree/>

 - Deploying OSTree in the Gnome servers: <https://bugzilla.gnome.org/show_bug.cgi?id=669772>
