Overview
--------

The build process is divided into two levels:

1) Yocto
2) ostbuild

Yocto is used as a reliable, well-maintained bootstrapping tool.  It
provides the basic filesystem layout as well as binaries for core
build utilities like gcc and bash.  This gets us out of circular
dependency problems.

At the end, the Yocto build process generates two tarballs: one for a
base "runtime", and one "devel" with all of the development tools like
gcc.  We then import that into an OSTree branch
e.g. "bases/yocto/gnomeos-3.4-i686-devel".

At present, it's still (mostly) possible to put this data on an ext4
filesystem and boot into it.

We also have a Yocto recipe "ostree-native" which generates (as you
might guess) a native binary of ostree.  That binary is used to import
into an "archive mode" OSTree repository.  You can see it in
$builddir/tmp/deploy/images/repo.

Now that we have an OSTree repository storing a base filesystem, we
can use "ostbuild" which uses "linux-user-chroot" to chroot inside,
run a build on a source tree, and outputs binaries, which we then add
to the build tree for the next module, and so on.

The final result of all of this is that the OSTree repository gains
new commits (which can be downloaded by clients), while still
retaining old build history.

Yocto details
-------------

I have a branch of Yocto here:

https://github.com/cgwalters/poky

It has a collection of patches on top of the "Edison" release of
Yocto, some of which are hacky, others upstreamable.  The most
important part though are the modifications to commit the generated
root filesystem into OSTree.

ostbuild details
----------------

The simple goal of ostbuild is that it only takes as input a
"manifest" which is basically just a list of components to build.  You
can see this here:

http://git.gnome.org/browse/ostree/tree/gnomeos/3.4/gnomeos-3.4-src.json

A component is a pure metadata file which includes the git repository
URL and branch name, as well as ./configure flags (--enable-foo).

There is no support for building from "tarballs" - I want the ability
to review all of the code that goes in, and to efficiently store
source code updates.  It's also just significantly easier from an
implementation perspective, versus having to maintain a version
control abstraction layer.

The result of a build of a component is an OSTree branch like
"artifacts/gnomeos-3.4-i686-devel/libxslt/master".  Then, a "compose"
process merges together the individual filesystem trees into the final
branches (e.g. gnomeos-3.4-i686-devel).

Doing local builds
------------------

This is where you want to modify one (or a few) components on top of
what comes from the ostree.gnome.org server, and test the result
locally.  I'm working on this.

Doing a full build on your system
---------------------------------

Following this process is equivalent to what we have set up on the
ostree.gnome.org build server.  It will generate a completely new
repository.

srcdir=/src
builddir=/src/build

# First, you'll need "http://git.gnome.org/browse/linux-user-chroot/"
# installed as setuid root.

cd $srcdir

git clone gnome:linux-user-chroot
cd linux-user-chroot
NOCONFIGURE=1 ./autogen.sh
./configure
make
sudo make install
sudo chown root:root /usr/local/bin/linux-user-chroot
sudo chmod u+s /usr/local/bin/linux-user-chroot

# We're going to be using Yocto.  You probably want to refer to:
# http://www.yoctoproject.org/docs/current/yocto-project-qs/yocto-project-qs.html
# Next, we're grabbing my Poky branch.

git clone git://github.com/cgwalters/poky.git
cd $builddir

# This command enters the Poky environment, creating
# a directory named gnomeos-build.
. $srcdir/poky/oe-init-build-env gnomeos-build

# Now edit conf/bblayers.conf, and add
#   /src/poky/meta-gnomeos
# to BBLAYERS.
# Make sure tools-profile and tools-testapps are not in EXTRA_IMAGE_FEATURES
# Also, you should choose useful values for BB_NUMBER_THREADS, PARALLEL_MAKE

bitbake ostree-native
bitbake gnomeos-contents-{runtime,devel}

# This bit is just for shorthand convenience, you can skip it 
cd $builddir
ln -s tmp/deploy/images/repo repo

# Now create a file ~/.config/ostbuild.cfg
# example contents:
# [global]
# repo=/src/build/gnomeos-build/build/repo
# mirrordir=/src/build/ostbuild/src
# workdir=/src/build/ostbuild/work
# manifest=/src/ostree/gnomeos/3.4/gnomeos-3.4-src.json

# Now we want to use the "ostbuild" binary that was created
# as part of "bitbake ostree-native".  You can do e.g.:

export PATH=$builddir/build/tmp/sysroots/x86_64-linux/usr/bin:$PATH

# This next command will download all of the source code to the
# modules specified in $srcdir/ostree/gnomeos/3.4/manifest.json,
# and create a file $workdir/manifest.json that has the
# exact git commits we want to build.
ostbuild resolve

# This command builds everything
ostbuild build
