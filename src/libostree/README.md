Repository design
-----------------

At the heart of OSTree is the repository.  It's very similar to git,
with the idea of content-addressed storage.  However, OSTree is
designed to store operating system binaries, not source code.  There
are several consequences to this.  The key difference as compared to
git is that the OSTree definition of "content" includes key Unix
metadata such as owner uid/gid, as well as all extended attributes.

Essentially OSTree is designed so that if two files have the same
OSTree checksum, it's safe to replace them with a hard link.  This
fundamental design means that an OSTree repository imposes negligible
overhead.  In contrast, a git repository stores copies of
zlib-compressed data.

Key differences versus git
--------------------------

 * As mentioned above, extended attributes and owner uid/gid are versioned
 * Optimized for Unix hardlinks between repository and checkout
 * SHA256 instead of SHA1
 * Support for empty directories

Binary files
------------

While this is still in planning, I plan to heavily optimize OSTree for
versioning ELF operating systems.  In industry jargon, this would be
"content-aware storage".

Trimming history
----------------

OSTree will also be optimized to trim intermediate history; in theory
one can regenerate binaries from corresponding (git) source code, so
we don't need to keep all possible builds over time.

MILESTONE 1
-----------
* Basic pack files (like git)

MILESTONE 2
-----------
* Store checksums as ay
* Drop version/metadata from tree/dirmeta objects
* Add index size to superindex, pack size to index
  - So pull can calculate how much we need to download
* Split pack files into metadata/data
* pull: Extract all we can from each packfile one at a time, then delete it
* Restructure repository so that links can be generated as a cache;
  i.e. objects/raw, pack files are now the canonical
* For files, checksum combination of metadata variant + raw data 
  - i.e. there is only OSTREE_OBJECT_TYPE_FILE (again)

MILESTONE 3
-----------

* Drop archive/raw distinction - archive repositories always generate
  packfiles per commit
* Include git packv4 ideas:
  - metadata packfiles have string dictionary (tree filenames and checksums)
  - data packfiles match up similar objects
* Rolling checksums for partitioning large files?  Kernel debuginfo
* Improved pack clustering
  - file fingerprinting?
* ELF-x86 aware deltas

Related work in storage
-----------------------

git: http://git-scm.com/
Venti: http://plan9.bell-labs.com/magic/man2html/6/venti
Elephant FS: http://www.hpl.hp.com/personal/Alistair_Veitch/papers/elephant-hotos/index.html

Compression
-----------

xdelta: http://xdelta.org/
Bsdiff: http://www.daemonology.net/bsdiff/
xz: http://tukaani.org/xz/
