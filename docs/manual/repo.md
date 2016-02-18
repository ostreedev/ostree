# Anatomy of an OSTree repository

## Core object types and data model

OSTree is deeply inspired by git; the core layer is a userspace
content-addressed versioning filesystem.  It is worth taking some time
to familiarize yourself with
[Git Internals](http://git-scm.com/book/en/Git-Internals), as this
section will assume some knowledge of how git works.

Its object types are similar to git; it has commit objects and content
objects.  Git has "tree" objects, whereas OSTree splits them into
"dirtree" and "dirmeta" objects.  But unlike git, OSTree's checksums
are SHA256.  And most crucially, its content objects include uid, gid,
and extended attributes (but still no timestamps).

### Commit objects

A commit object contains metadata such as a timestamp, a log
message, and most importantly, a reference to a
dirtree/dirmeta pair of checksums which describe the root
directory of the filesystem.
Also like git, each commit in OSTree can have a parent.  It is
designed to store a history of your binary builds, just like git
stores a history of source control.  However, OSTree also makes
it easy to delete data, under the assumption that you can
regenerate it from source code.

### Dirtree objects

A dirtree contains a sorted array of (filename, checksum)
pairs for content objects, and a second sorted array of
(filename, dirtree checksum, dirmeta checksum), which are
subdirectories.

### Dirmeta objects

In git, tree objects contain the metadata such as permissions
for their children.  But OSTree splits this into a separate
object to avoid duplicating extended attribute listings.

### Content objects

Unlike the first three object types which are metadata, designed to be
`mmap()`ed, the content object has a separate internal header and
payload sections.  The header contains uid, gid, mode, and symbolic
link target (for symlinks), as well as extended attributes.  After the
header, for regular files, the content follows.

# Repository types and locations

Also unlike git, an OSTree repository can be in one of three separate
modes: `bare`, `bare-user`, and `archive-z2`.  A bare repository is
one where content files are just stored as regular files; it's
designed to be the source of a "hardlink farm", where each operating
system checkout is merely links into it.  If you want to store files
owned by e.g. root in this mode, you must run OSTree as root.  

The `bare-user` is a later addition that is like `bare` in that files
are unpacked, but it can (and should generally) be created as
non-root.  In this mode, extended metadata such as owner uid, gid, and
extended attributes are stored but not actually applied.  
The `bare-user` mode is useful for build systems that run as non-root
but want to generate root-owned content, as well as non-root container
systems.

In contrast, the `archive-z2` mode is designed for serving via plain
HTTP.  Like tar files, it can be read/written by non-root users.

On an OSTree-deployed system, the "system repository" is
`/ostree/repo`.  It can be read by any uid, but only written by root.
Unless the `--repo` argument is given to the <command>ostree</command>
command, it will operate on the system repository.

## Refs

Like git, OSTree uses "refs" to which are text files that point to
particular commits (i.e. filesystem trees).  For example, the
gnome-ostree operating system creates trees named like
`exampleos/buildmaster/x86_64-runtime` and
`exampleos/buildmaster/x86_64-devel-debug`.  These two refs point to
two different generated filesystem trees.  In this example, the
"runtime" tree contains just enough to run a basic system, and
"devel-debug" contains all of the developer tools and debuginfo.

The `ostree` supports a simple syntax using the carat `^` to refer to
the parent of a given commit.  For example,
`exampleos/buildmaster/x86_64-runtime^` refers to the previous build,
and `exampleos/buildmaster/x86_64-runtime^^` refers to the one before
that.
