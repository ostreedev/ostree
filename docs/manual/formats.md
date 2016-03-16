# OSTree data formats

## On the topic of "smart servers"

One really crucial difference between OSTree and git is that git has a
"smart server".  Even when fetching over `https://`, it isn't just a
static webserver, but one that e.g. dynamically computes and
compresses pack files for each client.

In contrast, the author of OSTree feels that for operating system
updates, many deployments will want to use simple static webservers,
the same target most package systems were designed to use.  The
primary advantages are security and compute efficiency.  Services like
Amazon S3 and CDNs are a canonical target, as well as a stock static
nginx server.

## The archive-z2 format

In the [repo](repo) section, the concept of objects was introduced,
where file/content objects are checksummed and managed individually.
(Unlike a package system, which operates on compressed aggregates).

The archive-z2 format simply gzip-compresses each content object.
Metadata objects are stored uncompressed.  This means that it's easy
to serve via static HTTP.

When you commit new content, you will see new `.filez` files appearing
in `objects/`.

## archive-z2 efficiency

The advantages of `archive-z2`:

 - It's easy to understand and implement
 - Can be served directly over plain HTTP by a static webserver
 - Clients can download/unpack updates incrementally
 - Space efficient on the server

The biggest disadvantage of this format is that for a client to
perform an update, one HTTP request per changed file is required.  In
some scenarios, this actually isn't bad at all, particularly with
techniques to reduce HTTP overhead, such as
[HTTP/2](https://en.wikipedia.org/wiki/HTTP/2).

In order to make this format work well, you should design your content
such that large data that changes infrequently (e.g. graphic images)
are stored separately from small frequently changing data (application
code).

Other disadvantages of `archive-z2`:

 - It's quite bad when clients are performing an initial pull (without HTTP/2),
 - One doesn't know the total size (compressed or uncompressed) of content
   before downloading everything

## Aside: the bare and bare-user formats

The most common operation is to pull from an `archive-z2` repository
into a `bare` or `bare-user` formatted repository.  These latter two
are not compressed on disk.  In other words, pulling to them is
similar to unpacking (but not installing) an RPM/deb package.

The `bare-user` format is a bit special in that the uid/gid and xattrs
from the content are ignored.  This is primarily useful if you want to
have the same OSTree-managed content that can be run on a host system
or an unprivileged container.

## Static deltas

OSTree itself was originally focused on a continuous delivery model, where
client systems are expected to update regularly.  However, many OS vendors
would like to supply content that's updated e.g. once a month or less often.

For this model, we can do a lot better to support batched updates than
a basic `archive-z2` repo. However, we still want to preserve the
model of "static webserver only".  Given this, OSTree has gained the
concept of a "static delta".

These deltas are targeted to be a delta between two specific commit
objects, including "bsdiff" and "rsync-style" deltas within a content
object.  Static deltas also support `from NULL`, where the client can
more efficiently download a commit object from scratch.

Effectively, we're spending server-side storage (and one-time compute
cost), and gaining efficiency in client network bandwidth.

## Static delta repository layout

Since static deltas may not exist, the client first needs to attempt
to locate one.  Suppose a client wants to retrieve commit `${new}`
while currently running `${current}`.

The first thing to understand is that in order to save space, these
two commits are "modified base64" - the `/` character is replaced with
`_`.

Like the commit objects, a "prefix directory" is used to make
management easier for filesystem tools

A delta is named `$(mbase64 $from)-$(mbase64 $to)`, for example
`GpTyZaVut2jXFPWnO4LJiKEdRTvOw_mFUCtIKW1NIX0-L8f+VVDkEBKNc1Ncd+mDUrSVR4EyybQGCkuKtkDnTwk`,
which in SHA256 format is
`1a94f265a56eb768d714f5a73b82c988a11d453bcec3f985502b48296d4d217d-2fc7fe5550e410128d73535c77e98352b495478132c9b4060a4b8ab640e74f09`.

Finally, the actual content can be found in
`deltas/$fromprefix/$fromsuffix-$to`.

## Static delta internal structure

A delta is itself a directory.  Inside, there is a file called
`superblock` which contains metadata.  The rest of the files will be
integers bearing packs of content.

The file format of static deltas should be currently considered an
OSTree implementation detail.  Obviously, nothing stops one from
writing code which is compatible with OSTree today.  However, we would
like the flexibility to expand and change things, and having multiple
codebases makes that more problematic.  Please contact the authors
with any requests.

That said, one critical thing to understand about the design is that
delta payloads are a bit more like "restricted programs" than they are
raw data.  There's a "compilation" phase which generates output that
the client executes.

This "updates as code" model allows for multiple content generation
strategies.  The design of this was inspired by that of Chromium:
[http://dev.chromium.org/chromium-os/chromiumos-design-docs/filesystem-autoupdate](ChromiumOS
autoupdate).

### The delta superblock

The superblock contains:

 - arbitrary metadata
 - delta generation timestamp
 - the new commit object
 - An array of recursive deltas to apply
 - An array of per-part metadata, including total object sizes (compressed and uncompressed),
 - An array of fallback objects

Let's define a delta part, then return to discuss details:

## A delta part

A delta part is a combination of a raw blob of data, plus a very
restricted bytecode that operates on it.  Say for example two files
happen to share a common section.  It's possible for the delta
compilation to include that section once in the delta data blob, then
generate instructions to write out that blob twice when generating
both objects.

Realistically though, it's very common for most of a delta to just be
"stream of new objects" - if one considers it, it doesn't make sense
to have too much duplication inside operating system content at this
level.

So then, what's more interesting is that OSTree static deltas support
a per-file delta algorithm called
[bsdiff](https://github.com/mendsley/bsdiff) that most notably works
well on executable code.

The current delta compiler scans for files with matching basenames in
each commit that have a similar size, and attempts a bsdiff between
them.  (It would make sense later to have a build system provide a
hint for this - for example, files within a same package).

A generated bsdiff is included in the payload blob, and applying it is
an instruction.

## Fallback objects

It's possible for there to be large-ish files which might be resistant
to bsdiff.  A good example is that it's common for operating systems
to use an "initramfs", which is itself a compressed filesystem.  This
"internal compression" defeats bsdiff analysis.

For these types of objects, the delta superblock contains an array of
"fallback objects".  These objects aren't included in the delta
parts - the client simply fetches them from the underlying `.filez`
object.
