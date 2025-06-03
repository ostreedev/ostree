---
nav_order: 70
---

# OSTree and /var handling

{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

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
because this is more resilient.

Even better, use `StateDirectory=` for systemd units.

## Pitfalls

On subsequent upgrades, normally `/var` would not be empty anymore
(as it's typically expected that basics like `/var/tmp` etc. are created,
 if not also other local state such as `/var/log` etc.).  Hence,
no updates *to existing files* from the commit/container will be applied.

To be clear then:

- Any files which already exist will *not* be updated.
- Any files which are deleted in the new version will not be deleted on existing systems.

## Examples

### debs/RPMs which drop files into `/opt` (i.e. `/var/opt`)

The default OSTree "strict" layout has `/opt` be a symlink to `/var/opt`.
Including any packaged content that "straddles" `/usr` and `/var` (i.e. `/var/opt`)
will over time cause drift because changes in the package will not be reflected on disk.

For situations like this, it's strongly recommended to enable either
`composefs.enabled = true` or the `root.transient = true` option for `ostree-prepare-root.conf`
and change ensure your commit/container image has `/opt` as a plain directory.  In the former case,
content in `/opt` will be immutable at runtime, the same as everything else in `/usr`.
In the latter case content it will be writable but transient.

There's also a currently-experimental [../man/ostree-state-overlay@.service.xml](ostree-state-overlay@.service)
which can manage stateful writable overlays for individual mounts.

### Apache default content in `/var/www/html`

In general, such static content would much better live in `/usr` - or even better, in an application container.

### User home directories and databases

The semantics here are likely OK for the use case of "default users".

### `/var/lib/containers`

Pulling container images into OSTree commits like this would be a bad idea; similar problems as RPM content.

### dnf `/var/lib/dnf/history.sqlite`

For $reasons dnf has its own database for state distinct from the RPM database, which on rpm-ostree systems is in `/usr/share/rpm` (under the read-only bind mount, managed by OS updates).

In an image/container-oriented flow, we don't really care about this database which mainly holds things like "was this package user installed".  This data could move to `/usr`.

## Previous ostree /var and tmpfiles.d /usr/share/factory/var

From OSTree versions 2023.8 to v2024.3 the `/usr/lib/tmpfiles.d/ostree-tmpfiles.conf` file included this snippet:

```text
# Automatically propagate all /var content from /usr/share/factory/var;
# the ostree-container stack is being changed to do this, and we want to
# encourage ostree use cases in general to follow this pattern.
C+! /var - - - - -
```

Until version 0.13.2 of the ostree-ext project, content in `/var` in fetched container images is moved to `/usr/share/factory/var`, but this no longer happens when targeting ostree v2024.3.

Together, this will have the semantic that on OS updates, on the next boot (early in boot), any new files/directories will be copied.  For more information on this, see [`man tmpfiles.d`](https://man7.org/linux/man-pages/man5/tmpfiles.d.5.html).

This has been reverted, and the semantics defer to the above ostree semantic.
