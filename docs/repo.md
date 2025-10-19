---
nav_order: 30
---

# Anatomy of an OSTree repository
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

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
subdirectories. This type of object is stored as files
ending with `.dirtree` in the objects directory.

### Dirmeta objects

In git, tree objects contain the metadata such as permissions
for their children.  But OSTree splits this into a separate
object to avoid duplicating extended attribute listings.
These type of objects are stored as files ending with `.dirmeta`
in the objects directory.

### Content objects

Unlike the first three object types which are metadata, designed to be
`mmap()`ed, the content object has a separate internal header and
payload sections.  The header contains uid, gid, mode, and symbolic
link target (for symlinks), as well as extended attributes.  After the
header, for regular files, the content follows. These parts together
form the SHA256 hash for content objects. The content type objects in
this format exist only in `archive` OSTree repositories. Today the
content part is gzip'ed and the objects are stored as files ending
with `.filez` in the objects directory. Because the SHA256 hash is
formed over the uncompressed content, these files do not match the
hash they are named as.

The OSTree data format intentionally does not contain timestamps. The reasoning
is that data files may be downloaded at different times, and by different build
systems, and so will have different timestamps but identical physical content.
These files may be large, so most users would like them to be shared, both in
the repository and between the repository and deployments.

This could cause problems with programs that check if files are out-of-date by
comparing timestamps. For Git, the logical choice is to not mess with
timestamps, because unnecessary rebuilding is better than a broken tree.
However, OSTree has to hardlink files to check them out, and commits are assumed
to be internally consistent with no build steps needed. For this reason, OSTree
acts as though all timestamps are set to time_t 0, so that comparisons will be
considered up-to-date.  Note that for a few releases, OSTree used 1 to fix
warnings such as GNU Tar emitting "implausibly old time stamp" with 0; however,
until we have a mechanism to transition cleanly to 1, for compatibilty OSTree
is reverted to use zero again.

### Xattrs objects

In some repository modes (e.g. `bare-split-xattrs`), xattrs are stored on the
side of the content objects they refer to. This is done via two dedicated
object types, `file-xattrs` and `file-xattrs-link`.

`file-xattrs` store xattrs data, encoded as GVariant. Each object is keyed by
the checksum of the xattrs content, allowing for multiple references.

`file-xattrs-link` are hardlinks which are associated to file objects.
Each object is keyed by the same checksum of the corresponding file
object. The target of the hardlink is an existing `file-xattrs` object.
In case of reaching the limit of too many links, this object could be
a plain file too.

# Repository types and locations

Also unlike git, an OSTree repository can be in one of five separate
modes: `bare`, `bare-split-xattrs`, `bare-user`, `bare-user-only`, and
`archive`.

A `bare` repository is one where content files are just stored as regular
files; it's designed to be the source of a "hardlink farm", where each
operating system checkout is merely links into it.  If you want to store files
owned by e.g. root in this mode, you must run OSTree as root.

The `bare-split-xattrs` mode is similar to the above one, but it does store
xattrs as separate objects.  This is meant to avoid conflicts with
kernel-enforced constraints (e.g. on SELinux labels) and with other softwares
that may perform ephemeral changes to xattrs (e.g. container runtimes).

The `bare-user` mode is a later addition that is like `bare` in that
files are unpacked, but it can (and should generally) be created as
non-root.  In this mode, extended metadata such as owner uid, gid, and
extended attributes are stored in extended attributes under the name
`user.ostreemeta` but not actually applied.
The `bare-user` mode is useful for build systems that run as non-root
but want to generate root-owned content, as well as non-root container
systems.

The `bare-user-only` mode is a variant to the `bare-user` mode. Unlike
`bare-user`, neither ownership nor extended attributes are stored. These repos
are meant to to be checked out in user mode (with the `-U` flag), where this
information is not applied anyway. Hence this mode may lose metadata.
The main advantage of `bare-user-only` is that repos can be stored on
filesystems which do not support extended attributes, such as tmpfs.

In contrast, the `archive` mode is designed for serving via plain
HTTP.  Like tar files, it can be read/written by non-root users.

On an OSTree-deployed system, the "system repository" is `/ostree/repo`. It can
be read by any uid, but only written by root. The `ostree` command will by
default operate on the system repository; you may provide the `--repo` argument
to override this, or set the `$OSTREE_REPO` environment variable.

## Refs

Like git, OSTree uses the terminology "references" (abbreviated
"refs") which are text files that name (refer to) particular
commits.  See the
[Git Documentation](https://git-scm.com/book/en/v2/Git-Internals-Git-References)
for information on how git uses them.  Unlike git though, it doesn't
usually make sense to have a "main" branch.  There is a convention
for references in OSTree that looks like this:
`exampleos/buildmain/x86_64-runtime` and
`exampleos/buildmain/x86_64-devel-debug`.  These two refs point to
two different generated filesystem trees.  In this example, the
"runtime" tree contains just enough to run a basic system, and
"devel-debug" contains all of the developer tools and debuginfo.

The `ostree` supports a simple syntax using the caret `^` to refer to
the parent of a given commit.  For example,
`exampleos/buildmain/x86_64-runtime^` refers to the previous build,
and `exampleos/buildmain/x86_64-runtime^^` refers to the one before
that.

## The summary file

A later addition to OSTree is the concept of a "summary" file, created
via the `ostree summary -u` command.  This was introduced for a few
reasons.  A primary use case is to be compatible with
[Metalink](https://en.wikipedia.org/wiki/Metalink), which requires a
single file with a known checksum as a target.

The summary file primarily contains two mappings:

 - A mapping of the refs and their checksums, equivalent to fetching
   the ref file individually
 - A list of all static deltas, along with their metadata checksums

This currently means that it grows linearly with both items.  On the
other hand, using the summary file, a client can enumerate branches.

Further, fetching the summary file over e.g. pinned TLS creates a strong
end-to-end verification of the commit or static delta.

The summary file can also be GPG signed (detached). This is currently
the only way to provide GPG signatures (transitively) on deltas.

If a repository administrator creates a summary file, they must
thereafter run `ostree summary -u` to update it whenever a ref is
updated or a static delta is generated.
