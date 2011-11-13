Setup
-----

We're going to be using Yocto.  You probably want to refer to:
http://www.yoctoproject.org/docs/current/yocto-project-qs/yocto-project-qs.html

The first part of this setup just repeats that.

Choose a directory for git sources, and a different directory for
builds. I use: /src/ for git checkouts, and /src/build for builds.

Get a Yocto checkout:

cd /src
git clone -b edison git://git.yoctoproject.org/poky.git

mkdir -p /src/build/gnomeos
cd /src/build/gnomeos
. oe-init-build-env

If you want at this point, you can run 'bitbake core-image-minimal'
and you'll get an image bootable in QEMU.  However, our next step
is to set up the gnomeos layer on top.

You'll need a checkout of ostree:

cd /src
git clone git://git.gnome.org/ostree

Now, edit /src/build/gnomeos/build/conf/layers.conf

Add /src/ostree/gnomeos/yocto as a layer.  I also recommend editing
conf/local.conf and doing the following:

 * remove tools-profile and tools-testapps from EXTRA_IMAGE_FEATURES
 * choose useful values for BB_NUMBER_THREADS, PARALLEL_MAKE


