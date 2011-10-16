Hacktree
========

Problem statement
-----------------

Hacking on the core operating system is painful - this includes most
of GNOME from upower and NetworkManager up to gnome-shell.  I want a
system that matches these requirements:

0. Does not disturb your existing OS
1. Is not terribly slow to use
2. Shares your $HOME - you have your data
3. Allows easy rollback
4. Ideally allows access to existing apps

Comparison with existing tools
------------------------------

 - Virtualization

    Fails on points 2) and 3).

 - Rebuilding OS packages

    Fails on points 1) and 4).  Is also just very annoying.

 - "sudo make install"

    Now your system is in an undefined state - it's very possble left over files here
    will come back later to screw you.

 - LXC / containers

   Focused on running multiple systems at the *same time*, which isn't
   what we want (at least, not right now), and honestly even trying to
   support that for a graphical desktop would be a lot of tricky work,
   for example getting two GDM instances not to fight over VT
   allocations. But some bits of the technology may make sense to use.

 - jhbuild + OS packages

    The state of the art in GNOME - but can only build non-root things -
    this means you can't build NetworkManager, and thus are permanently
    stuck on whatever the distro provides.

Who is hacktree for?
------------------------------

First - operating system developers and testers.  I specifically keep
a few people in mind - Dan Williams and Eric Anholt, as well as myself
obviously.  For Eric Anholt, a key use case for him is being able to
try out the latest gnome-shell, and combine it with his work on Mesa,
and see how it works/performs - while retaining the ability to roll
back if one or both breaks.

The rollback concept is absolutely key for shipping anything to
enthusiasts or knowledable testers.  With a system like this, a tester
can easily perform a local rollback - something just not well
supported by dpkg/rpm.  (Why not Conary?  AIUI Conary is targeted at
individual roots, so while you could roll back a given root, it would
use significantly more disk space than hacktree)

Also, distributing operating system trees (instead of packages) gives
us a sane place to perform automated QA **before** we ship it to
testers.  We should never be wasting these people's time.

Even better, this system would allow testers to bisect across
operating system builds efficiently.

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

		/usr
		/etc
		/home
		/gnomeos/root-3.0-opt/{usr,etc,var,...}
		/gnomeos/root-3.2-opt/{usr,etc,var,...}

These live in the same root filesystem as your regular distribution
(Note though, the root partition should be reasonably sized, or
hopefully you've used just one big partition).

You should be able to boot into one of these roots.  Since hacktree
lives inside a distro created partition, a tricky part here is that we
need to know how to interact with the installed distribution's grub.
This is an annoying but tractable problem.

Hacktree will allow efficiently parallel installing and downloading OS
builds.

An important note here is that we explicitly link /home in each root
to the real /home.  This means you have your data.  This also implies
we share uid/gid, so /etc/passwd will have to be in sync.  Probably
what we'll do is have a script to pull the data from the "host" OS.

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
    under the control of hacktree.

I chose 2.

A userspace content-addressed versioning filesystem
---------------------------------------------------

At its very core, that's what hacktree is.  Just like git.  If you
understand git, you know it's not like other revision control systems.
git is effectively a specialized, userspace filesystem, and that is a
very powerful idea.

At the core of git is the idea of "content-addressed objects".  For
background on this, see <http://book.git-scm.com/7_how_git_stores_objects.html>

Why not just use git?  Basically because git is designed mainly for
source trees - it goes to effort to be sure it's compressing text for
example, under the assumption that you have a lot of text.  Its
handling of binaries is very generic and unoptimized.

In contrast, hacktree is explicitly designed for binaries, and in
particular one type of binary - ELF executables (or it will be once we
start using bsdiff).  

Another big difference versus git is that hacktree uses hard links
between "checkouts" and the repository.  This means each checkout uses
almost no additional space, and is *extremely* fast to check out.  We
can do this because again each checkout is designed to be read-only.

So we mentioned above the 

		/gnomeos/root-3.0-opt
		/gnomeos/root-3.2-opt

There is also a "repository" that looks like this:

		.ht/objects/17/a95e8ca0ba655b09cb68d7288342588e867ee0.file
		.ht/objects/17/68625e7ff5a8db77904c77489dc6f07d4afdba.meta
		.ht/objects/17/cc01589dd8540d85c0f93f52b708500dbaa5a9.file
		.ht/objects/30
		.ht/objects/30/6359b3ca7684358a3988afd005013f13c0c533.meta
		.ht/objects/30/8f3c03010cedd930b1db756ce659c064f0cd7f.meta
		.ht/objects/30/8cf0fd8e63dfff6a5f00ba5a48f3b92fb52de7.file
		.ht/objects/30/6cad7f027d69a46bb376044434bbf28d63e88d.file

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

Hacktree is designed to atomically swap operating systems - such that
during an upgrade and reboot process, you either get the full new
system, or the old one.  There is no "Please don't turn off your
computer".  We do this by simply using a symbolic link like:

/gnomeos -> /gnomeos-e3b0c4429

Where /gnomeos-e3b0c4429/ has the full regular filesystem tree with
usr/ etc/ directories as above.  To upgrade or rollback (there is no
difference internally), we simply check out a new tree into
/gnomeos-b90ae4763 for example, then swap the symbolic link, then
remove the old tree.

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

Basically I think they're a broken idea.  There are several different
classes of things that demand targeted solutions:

 1. Managing and upgrading the core OS (hacktree)
 2. Managing and upgrading desktop applications (gnome-shell, glick?)
 3. System extensions - these are arbitrary RPMs like say the nVidia driver.
    We apply them after constructing each root.  Media codecs also fall
    into this category.

How one might install say Apache on top of hacktree is an open
question - I think it probably makes sense honestly to ship services
like this with no configuration - just the binaries.  Then admins can
do whatever they want.

Downloads
---------

I'm pretty sure hacktree should be significantly better than RPM with
deltarpms.  Note we only download changed objects.  This means that if
OpenOffice is rebuilt and just the binary changes, but no data files,
we don't redownload ANY of that data.  And bsdiff is supposedly very
good for binaries.

Upstream branches
----------------

Note that this system will make it easy to have multiple *upstream* roots too.
For example, something like:

 - builds

   A filesystem tree generated after every time an artifact is built.

 - fastqa

   After each root is built, a very quick test suite is run in it;
   probably this is booting to GDM.  If that works, a new commit is
   made here.  Hopefully we can get fastqa under 2 minutes.

 - dailyqa

   Much more extensive tests, let's say they take 24 hours.

RPM Compatibility
-----------------

We should be able to install LSB rpms.  This implies providing "rpm".
The tricky part here is since the OS itself is not assembled via RPMs,
we need to fake up a database of "provides" as if we were.  Even
harder would be maintaining binary compatibilty with any arbitrary
%post scripts that may be run.

Note these RPMs act like local configuration - they would be
reinstalled every time you switch roots.

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

 - NixOS: <http://nixos.org/>

    The NixOS people have a lot of really good ideas, and they've definitely
    thought about the problem space.  However, their approach of checksumming
    all inputs to a package is pretty wacky.  I don't see the point, and moreover
    it uses gobs of disk space.

 - Conary: <http://wiki.rpath.com/wiki/Conary:Updates_and_Rollbacks>

   If rpm/dpkg are like CVS, Conary is closer to Subversion.  It's not
   bad, but hacktree is better than it for the exact same reasons git
   is better than Subversion.

 - Jhbuild: <https://live.gnome.org/Jhbuild>

   What we've been using in GNOME, and has the essential property of allowing you
   to "fall back" to a stable system.  But hacktree will blow it out of the water.
