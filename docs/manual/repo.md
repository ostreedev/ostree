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
acts as though all timestamps are set to time_t 1, so that comparisons will be
considered up-to-date. 1 is a better choice than 0 because some programs use 0
as a special value; for example, GNU Tar warns of an "implausibly old time
stamp" with 0.

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

Like git, OSTree uses the terminology "references" (abbreviated
"refs") which are text files that name (refer to) to particular
commits.  See the
[Git Documentation](https://git-scm.com/book/en/v2/Git-Internals-Git-References)
for information on how git uses them.  Unlike git though, it doesn't
usually make sense to have a "master" branch.  There is a convention
for references in OSTree that looks like this:
`exampleos/buildmaster/x86_64-runtime` and
`exampleos/buildmaster/x86_64-devel-debug`.  These two refs point to
two different generated filesystem trees.  In this example, the
"runtime" tree contains just enough to run a basic system, and
"devel-debug" contains all of the developer tools and debuginfo.

The `ostree` supports a simple syntax using the caret `^` to refer to
the parent of a given commit.  For example,
`exampleos/buildmaster/x86_64-runtime^` refers to the previous build,
and `exampleos/buildmaster/x86_64-runtime^^` refers to the one before
that.

## The summary file

A later addition to OSTree is the concept of a "summary" file, created
via the `ostree summary -u` command.  This was introduced for a few
reasons.  A primary use case is to be a target a
[Metalink](https://en.wikipedia.org/wiki/Metalink), which requires a
single file with a known checksum as a target.

The summary file primarily contains two mappings:

 - A mapping of the refs and their checksums, equivalent to fetching
   the ref file individually
 - A list of all static deltas, along with their metadata checksums

This currently means that it grows linearly with both items.  On the
other hand, using the summary file, a client can enumerate branches.

Further, the summary file is fetched over e.g. pinned TLS, this
creates a strong end-to-end verification of the commit or static delta.

The summary file can also be GPG signed (detached), and currently this
is the only way provide GPG signatures (transitively) on deltas.

If a repository administrator creates a summary file, they must
thereafter run `ostree summary -u` to update it whenever a commit is
made or a static delta is generated.
