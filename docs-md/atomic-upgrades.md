Atomic upgrades and rollback
----------------------------

Traditional package managers operate "live" on the running system.
This means it's possible for interrupted updates to result in a
half-updated system.  This model also makes it significantly harder to
support rollbacks when updates fail.

In contrast, OSTree always creates a *new* root whenever it's
performing an update.  This new root shares storage via hardlinks with
the current system.  Upon success, the bootloader configuration will
be updated.
