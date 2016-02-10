rofiles-fuse
============

Create a mountpoint that represents an underlying directory hierarchy,
but where non-directory inodes cannot have content or xattrs changed.
Files can still be unlinked, and new ones created.

This filesystem is designed for OSTree and other systems that create
"hardlink farms", i.e. filesystem trees deduplicated via hardlinks.

Normally with hard links, if you change one, you change them all.

There are two approaches to dealing with that:
 - Copy on write: implemented by BTRFS, overlayfs, and http://linux-vserver.org/util-vserver:Vhashify
 - Make them read-only: what this FUSE mount does

Usage
=====

Let's say that you have immutable data in `/srv/backups/20150410`, and
you want to update it with a new version, storing the result in
`/srv/backups/20150411`.  Further assume that all software operating
on the directory does the "create tempfile and `rename()`" dance
rather than in-place edits.

    $ mkdir -p /srv/backups/mnt   # Ensure we have a mount point
    $ cp -al /srv/backups/20150410 /srv/backups/20150411
    $ rofiles-fuse /srv/backups/20150411 /srv/backups/mnt

Now we have a "rofiles" mount at `/srv/backups/mnt`.  If we try this:

    $ echo new doc content > /srv/backups/mnt/document
    bash: /srv/backups/mnt/document: Read-only file system

It failed because the `>` redirection operator will try to truncate
the existing file.  If instead we create `document.tmp` and then
rename it atomically over the old one, it will work:

    $ echo new doc content > /srv/backups/mnt/document.tmp
    $ mv /srv/backups/mnt/document.tmp /srv/backups/mnt/document

Let's unmount:

    $ fusermount -u /srv/backups/mnt

Now we have two directories `/srv/backups/20150410`
`/srv/backups/20150411` which share all file storage except for the
new document.
