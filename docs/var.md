---
nav_order: 6
---

# OSTree and /var handling

{: .no_toc }

1. TOC
{:toc}

## Default commit/image /var handling

As of OSTree 2024.3, when a commit is "deployed" (queued to boot),
the initial content of `/var` in a commit will be placed into the
"stateroot" (default `var`) if the stateroot `var` is empty.

The semantics of this are intended to match that of Docker "volumes";
consider that ostree systems have the equivalent of
`VOLUME /var`
by default.

It is still strongly recommended to use systemd `tmpfiles.d` snippets
to populate directory structure and the like in `/var` on firstboot,
because this is more resilent.

Even better, use `StateDirectory=` for systemd units.

### ostree container /var

Some earlier versions of the ostree-container stack migrated content in `/var`
in container images into `/usr/share/factory/var` (per below).  This has
been reverted, and the semantics defer to the above ostree semantic.

## Previous /var handling via /usr/share/factory/var

As of OSTree 2023.8, the `/usr/lib/tmpfiles.d/ostree-tmpfiles.conf` file gained this snippet:

```text
# Automatically propagate all /var content from /usr/share/factory/var;
# the ostree-container stack is being changed to do this, and we want to
# encourage ostree use cases in general to follow this pattern.
C+! /var - - - - -
```

This is inert by default.  As of version 0.13 of the ostree-ext project, content in `/var` in fetched container images is moved to `/usr/share/factory/var`.  This is no longer recommended.

Together, this will have the semantic that on OS updates, on the next boot (early in boot), any new files/directories will be copied.  For more information on this, see [`man tmpfiles.d`](https://man7.org/linux/man-pages/man5/tmpfiles.d.5.html).

However, `tmpfiles.d` is not a package system:

## Pitfalls

- Large amounts of data will slow down firstboot while the content is copied (though reflinks are used if available)
- Any files which already exist will *not* be updated.
- Any files which are deleted in the new version will not be deleted on existing systems.

## Examples

### Apache default content in `/var/www/html`

The `tmpfiles.d` model may work OK for use cases that wants to treat this content as locally mutable state.  But in general, such static content would much better live in `/usr` - or even better, in an application container.

### User home directories and databases

The semantics here are likely OK for the use case of "default users".

### debs/RPMs which drop files into `/opt` (i.e. `/var/opt`)

The default OSTree "strict" layout has `/opt` be a symlink to `/var/opt`.
However, `tmpfiles.d` is not a package system, and so over time these will slowly
break because changes in the package will not be reflected on disk.

For situations like this, it's recommended to enable the `root.transient = true` option for `ostree-prepare-root.conf`
and change your build system to make `/opt` a plain directory.

### `/var/lib/containers`

Pulling container images into OSTree commits like this would be a bad idea; similar problems as RPM content.

### dnf `/var/lib/dnf/history.sqlite`

For $reasons dnf has its own database for state distinct from the RPM database, which on rpm-ostree systems is in `/usr/share/rpm` (under the read-only bind mount, managed by OS updates).

In an image/container-oriented flow, we don't really care about this database which mainly holds things like "was this package user installed".  This data could move to `/usr`.
