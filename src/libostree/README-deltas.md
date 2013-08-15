OSTree Static Object Deltas
===========================

Currently, OSTree's "archive-z2" mode stores both metadata and content
objects as individual files in the filesystem.  Content objects are
zlib-compressed.

The advantage of this is model are:

0) It's easy to understand and implement
1) Can be served directly over plain HTTP by a static webserver
2) Space efficient on the server

However, it can be inefficient both for large updates and small ones:

0) For large tree changes (such as going from -runtime to
   -devel-debug, or major version upgrades), this can mean thousands
   and thousands of HTTP requests.  The overhead for that is very
   large (until SPDY/HTTP2.0), and will be catastrophically bad if the
   webserver is not configured with KeepAlive.
1) Small changes (typo in gnome-shell .js file) still require around
   5 metadata HTTP requests, plus a redownload of the whole file.

Why not smart servers?
======================

Smart servers (custom daemons, or just CGI scripts) as git has are not
under consideration for this proposal.  OSTree is designed for the
same use case as GNU/Linux distribution package systems are, where
content is served by a network of volunteer mirrors that will
generally not run custom code.

In particular, Amazon S3 style dumb content servers is a very
important use case, as is being able to apply updates from static
media like DVD-ROM.

Finding Static Deltas
=====================

Since static deltas may not exist, the client first needs to attempt
to locate one.  Suppose a client wants to retrieve commit ${new} while
currently running ${current}.  The first thing to fetch is the delta
metadata, called "meta".  It can be found at
${repo}/deltas/${current}-${new}/meta.

FIXME: GPG signatures (.metameta?)  Or include commit object in meta?
But we would then be forced to verify the commit only after processing
the entirety of the delta, which is dangerous.  I think we need to
require signing deltas.

Delta Bytecode Format
=====================

A delta-part has the following form:

byte compression-type (0 = none, 'g' = gzip')
REPEAT[(varint size, delta-part-content)]

delta-part-content:
  byte[] payload
  ARRAY[operation]

The rationale for having delta-part is that it allows easy incremental
resumption of downloads.  The client can look at the delta descriptor
and skip downloading delta-parts for which it already has the
contained objects.  This is better than simply resuming a gigantic
file because if the client decides to fetch a slightly newer version,
it's very probable that some of the downloading we've already done is
still useful.

For the actual delta payload, it comes as a stream of pair of
(payload, operation) so that it can be processed while being
decompressed.

Finally, the delta-part-content is effectively a high level bytecode
for a stack-oriented machine.  It iterates on the array of objects in
order.  The following operations are available:

FETCH
  Fall back to fetching the current object individually.  Move
  to the next object.

WRITE(array[(varint offset, varint length)])
  Write from current input target (default payload) to output.

GUNZIP(array[(varint offset, varint length)])
  gunzip from current input target (default payload) to output.

CLOSE
  Close the current output target, and proceed to the next; if the
  output object was a temporary, the output resets to the current
  object.

# Change the input source to an object
READOBJECT(csum object)
  Set object as current input target

# Change the input source to payload
READPAYLOAD
  Set payload as current input target

Compiling Deltas
================

After reading the above, you may be wondering how we actually *make*
these deltas.  I envison a strategy similar to that employed by
Chromium autoupdate:
http://www.chromium.org/chromium-os/chromiumos-design-docs/autoupdate-details

Something like this would be a useful initial algorithm:
1) Compute the set of added objects NEW
2) For each object in NEW:
  - Look for a the set of "superficially similar" objects in the
    previous tree, using heuristics based first on filename (including
    prefix), then on size.  Call this set CANDIDATES.
    For each entry in CANDIDATES:
      - Try doing a bup/librsync style rolling checksum, and compute the
        list of changed blocks.
      - Try gzip-compressing it
3) Choose the lowest cost method for each NEW object, and partition
   the program for each method into deltapart-sized chunks.

However, there are many other possibilities, that could be used in a
hybrid mode with the above.  For example, we could try to find similar
objects, and gzip them together.  This would be a *very* useful
strategy for things like the 9000 Boost headers which have massive
amounts of redundant data.

Notice too that the delta format supports falling back to retrieving
individual objects.  For cases like the initramfs which is compressed
inside the tree with gzip, we're not going to find an efficient way to
sync it, so the delta compiler should just fall back to fetching it
individually.

Which Deltas To Create?
=======================

Going back to the start, there are two cases to optimize for:

1) Incremental upgrades between builds
2) Major version upgrades

A command line operation would look something like this:

$ ostree --repo=/path/to/repo gendelta --ref-prefix=gnome-ostree/buildmaster/ --strategy=latest --depth=5

This would tell ostree to generate deltas from each of the last 4
commits to each ref (e.g. gnome-ostree/buildmaster/x86_64-runtime) to
the latest commit.  It might also be possible of course to have
--strategy=incremental where we generate a delta between each commit.
I suspect that'd be something to do if one has a *lot* of disk space
to spend, and there's a reason for clients to be fetching individual
refs.

$ ostree --repo=/path/to/repo gendelta --from=gnome-ostree/3.10/x86_64-runtime --to=gnome-ostree/buildmaster/x86_64-runtime

This is an obvious one - generate a delta from the last stable release
to the current development head.

