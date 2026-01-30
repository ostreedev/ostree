# The ostree boot deployment transaction

## Why this exists

This document explains the process by which a new ostree deployment is made
bootable.  This is a critical, multi-step operation that can involve
coordinating changes across two different filesystems: the root filesystem
(containing `/ostree`) and the boot filesystem (`/boot`).

The core logic for this process can be found in
`src/libostree/ostree-sysroot-deploy.c`.

## The problem space: Two filesystems

ostree's design supports having `/boot` be a separate filesystem from the
root.  This is a common and recommended configuration for many operating
systems.  However, this means we can no longer rely on a single atomic
filesystem operation (like `rename()`) to update the system.

Instead, we have a multi-step process that involves writing to both
filesystems and carefully synchronizing the underlying storage.

The two filesystems and their roles are:

- **Root Filesystem (`/`)**: Contains the ostree repository and deployments in `/ostree/`.
- **Boot Filesystem (`/boot`)**: Holds the bootloader, kernel, initramfs, and bootloader configuration.

And the key objects we are manipulating are:

- `/ostree/boot.{0,1}`: A set of numbered symbolic links which point to the active deployments.
- `/boot/loader`: A symbolic link which points to the bootloader configuration to use.

```
/ (rootfs)                            /boot (bootfs)
+-- ostree/                           +-- loader -> loader.0
   +-- deploy/                        +-- loader.0/
   |   +-- <hash1>/          |            +-- entries/
   |   +-- <hash2>/          |            +-- ...
   +-- boot.0 -> deploy/<csum>/0          +-- loader.1/
   +-- boot.1 -> deploy/<csum>/0      +-- ostree/
                                      +-- ...
```

## The `bootcsum` and shared boot artifacts

A key concept in ostree's boot process is the "boot checksum", or `bootcsum`.
This is a SHA256 checksum of the kernel, initramfs, and any device tree files.
The primary purpose of the `bootcsum` is to allow multiple deployments to share
the same set of boot artifacts if they haven't changed.

This is particularly useful in dual-boot scenarios (e.g., A/B deployments)
where two different OS versions might be using the exact same kernel. Instead
of duplicating the kernel and initramfs in `/boot`, ostree creates a directory
named `/boot/ostree/<osname>-<bootcsum>/` which contains these shared files.

The `bootcsum` is calculated during the deployment process, primarily within
the `ostree_sysroot_deploy_tree()` function. The checksum is then stored as
part of the deployment's metadata and can be retrieved using
`ostree_deployment_get_bootcsum()`.

When the bootloader configuration is generated, the kernel and initramfs paths
will point to the files within this shared `bootcsum` directory. The boot
symlinks (`/ostree/boot.{0,1}`) will also include the `bootcsum` in their
target path, as can be seen in the diagram above. The function
`_ostree_sysroot_parse_bootlink()` is used to parse this information from the
symlink.

## Deployment Scenarios

The key decision in the deployment process is whether a full "bootswap" is required. This is determined by the `requires_new_bootversion` flag in the deployment logic. A bootswap is a more involved process that changes the active bootloader state directory via the `/boot/loader` symlink.

A bootswap is **required** if any of the following conditions are met:

- The number of deployments changes (e.g., adding a new deployment, removing an old one).
- For any existing deployment, its boot-related configuration changes. This is checked by the `deployment_bootconfigs_equal()` function, which compares:
    - The boot checksum (kernel, initramfs, devicetree).
    - The kernel arguments.
    - The overlay initrds.
    - The stateroot (`osname`).
    - The version string used in the bootloader menu title.

### Scenario 1: No bootswap required (simple symlink update)

This scenario occurs only when the set of deployments and all their boot-related metadata are identical to the current state. This happens in a relatively common case where e.g. there are two deployments (a current and rollback) and we have a new staged update. Then the staged will become current, and current will become the rollback, keeping two deployments.

1.  **Create New Bootlinks**: A new temporary directory of symlinks is created (e.g., `/ostree/boot.0.1`) pointing to the new deployment content.
2.  **System Sync**: `full_system_sync()` is called to ensure all filesystem changes are persisted to disk.
3.  **Atomic Swap**: The primary boot symlink (e.g., `/ostree/boot.0`) is atomically updated to point to the new temporary directory.

This process does not alter the `/boot/loader` symlink or the contents of the `/boot` partition at all.

### Scenario 2: Bootswap required (swapping bootloader state)

This is the common path for almost all upgrades and changes. It is handled by the `write_deployments_bootswap` function.

The sequence is as follows:

1.  **Write New Artifacts**: First, all of the new files and directories are written to the filesystem. This includes the new kernel and initramfs in `/boot`, the new bootloader configuration in a temporary directory (e.g. `/boot/loader.1`), and the new `/ostree/boot.1` symlink.

2.  **Prepare for Final Swap**: A temporary symbolic link, `/boot/loader.tmp`, is created. It points to the new bootloader directory from the previous step.

3.  **The Critical Sync (`full_system_sync`)**: This is a crucial step for ensuring robustness against crashes. The goal is to ensure all changes from steps 1 & 2 are durably persisted to the physical disk *before* performing the final, non-reversible bootloader switch. To do this, we call `syncfs()` on the root filesystem, and issue an `ioctl(FIFREEZE)` on the `/boot` filesystem. This is a strong synchronization primitive that forces the filesystem to flush all of its data and metadata, including its journal. This is important for filesystems like XFS, as bootloaders often cannot read a "dirty" journal.

4.  **The Atomic Switch (`swap_bootloader`)**: With all of the data safely on disk, we can now perform the "point of no return". An atomic `renameat()` is performed to rename `/boot/loader.tmp` to `/boot/loader`. After this point, the bootloader will use the new configuration on the next boot.

5.  **Final Sync**: To be fully robust, another `fsfreeze`/`thaw` cycle is performed on `/boot` to ensure the `renameat()` operation from step 4 is also durably persisted to disk.
