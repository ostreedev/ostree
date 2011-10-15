NOTE THIS STUFF IS OUT OF DATE!  I'm working on merging some of these
ideas into jhbuild for now.

== The recipe set ==

A recipe is similar to Bitbake's format, except just have metadata -
we don't allow arbitrary Python scripts.  Also, we assume
autotools. Example:

SUMMARY = "The basic file, shell and text manipulation utilities."
HOMEPAGE = "http://www.gnu.org/software/coreutils/"
BUGTRACKER = "http://debbugs.gnu.org/coreutils"
LICENSE = "GPLv3+"
LIC_FILES_CHKSUM = "file://COPYING;md5=d32239bcb673463ab874e80d47fae504\
                    file://src/ls.c;startline=5;endline=16;md5=e1a509558876db58fb6667ba140137ad"
SRC_URI = "${GNU_MIRROR}/coreutils/${BP}.tar.gz \
           file://remove-usr-local-lib-from-m4.patch \
          "
DEPENDS = "gmp foo"

Each recipe will output one or more artifacts.


In GNOME, we will have a root per:
 - major version (3.0, 3.2)
 - "runtime", "sdk", and "devel"
 - Build type (opt, debug)
 - Architecture (ia32, x86_64)

/gnome/root-3.2-runtime-opt-x86_64/{etc,bin,share,usr,lib}
/gnome/root-3.2-devel-debug-x86_64/{etc,bin,share,usr,lib}
/gnome/.real/root-3.2-runtime-opt-x86_64
/gnome/.real/root-3.2-devel-debug-x86_64

A "runtime" root is what's necessary to run applications.  A SDK root
is that, plus all the command line developer tools (gcc, gdb, make,
strace).  And finally the "devel" root has all the API-unstable
headers not necessary for applications (NetworkManager.h etc.)

Hmm, maybe we should punt developer tools into a Unix app framework.

== Artifact ==

An artifact is a binary result of compiling a recipe (there may be
multiple).  Think of an artifact as like a Linux distribution
"package", except there are no runtime dependencies, conflicts, or
pre/post scripts.  It's basically just a gzipped tarball, and we
encode metadata in the filename.

Example:

gdk-pixbuf-runtime,o=master,r=3.2-opt-x86_64,b=opt,v=2.24.0-10-g1d39095,.tar.gz

This is an artifact from the gdk-pixbuf component.  Here's a decoding of the key/value pairs:

o: The origin of the build - there are just "master" and "local"
r: The name of the root this artifact was compiled against
b: The name of the build flavor (known values are "opt" and "debug")
v: The output of "git describe".

To build a root, we simply unpack the artifacts that compose it, and
run "git commit".

hacktree will default to splitting up shared libraries' unversioned .so
link and header files into -devel, and the rest into -runtime.

All binaries default to runtime.

Local modifications ==

A key point of this whole endeavour is that we want developers to be
able to do local builds.  This is surprisingly something not well
supported by the Debian/Fedora's tools at least.

=== Updating a root with a new local artifact ===

Whenever you install a local artifact, if no "local" branch exists for
that root, it's created.

Let's say we're debugging gdk-pixbuf, tracking down a memory
corruption bug.  We've added a few printfs, and want to rerun things.
GCC optimization is screwing us, so we build it in debug mode (-O0).
The active root is root-3.2-opt.

$ pwd
~/src/gdk-pixbufroot
$ echo $HACKTREE_ROOT
/gnome/root-3.2-opt
<hack hack hack>
$ hacktree make debug
<time passes, hopefully not too much>
$ ls gdk-pixbuf*.tar.gz
gdk-pixbuf-runtime,o=master,r=3.2-opt,b=debug,v=2.24.0-10-g1d39095,.tar.gz
gdk-pixbuf-devel,o=master,r=3.2-opt,b=debug,v=2.24.0-10-g1d39095,.tar.gz
gdk-pixbuf-manifests,o=master,r=3.2-opt,b=debug,v=2.24.0-10-g1d39095,.tar.gz
$ hacktree install gdk-pixbuf*,o=master,r=3.2-opt,b=debug,v=2.24.0-10-g1d39095,.tar.gz
<policykit auth dialog>

Now here's where the cool stuff happens.  hacktree takes
/gnome/root-3.2-opt (the which is given in the r= above), and looks
for the corresponding git branch (root-3.2-opt).  Now hacktree notices
there's no corresponding "local" branch, i.e. local-3.2-opt.  One is
created and checked out:

# pwd
/gnome/repo.git
# git branch local-3.2-opt root-3.2-opt
# git clone --branch local-3.2-opt /gnome/repo.git /gnome/.real-local-3.2-opt

Now, the artifacts specified are overlaid:

# cd /gnome/.real-local-3.2-opt
# tar xvf

Ok, now we need to remove old no longer shipped files from the root.
Thus, we need a list of files corresponding to each original artifact,
and to know which artifacts are in a root.  Note above that one of the
artifacts produced was "manifests".  This contains files like:

/meta/manifests/gdk-pixbuf-runtime.list
/meta/manifests/gdk-pixbuf-devel.list

Thus we diff the manifests, and clean up any leftover files.

# git commit -a -m "Install artifact gdk-pixbuf-runtime,o=master,r=3.2-opt,b=debug,v=2.24.0-10-g1d39095,.tar.gz"
# git checkout 


