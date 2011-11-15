#!/bin/sh
### BEGIN INIT INFO
# Provides:          mountvirtfs
# Required-Start:
# Required-Stop:
# Default-Start:     S
# Default-Stop:
# Short-Description: Mount kernel virtual file systems.
# Description:       Mount initial set of virtual filesystems the kernel
#                    provides and that are required by everything.
### END INIT INFO

if [ -e /proc ] && ! [ -e /proc/mounts ]; then
  mount -t proc proc /proc
fi

if [ -e /sys ] && grep -q sysfs /proc/filesystems; then
  mount sysfs /sys -t sysfs
fi
