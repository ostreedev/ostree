/* SPDX-License-Identifier: LGPL-2.0+ */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Preloaded only by test-delta-fallocate.sh. The reference object has 65538
 * bytes; the patch output has 65537, so faults affect only the patch tempfile.
 */
static int patch_fd = -1;

int
fallocate (int fd, int mode, off_t offset, off_t len)
{
  if (len == 65537)
    {
      patch_fd = fd;
      setenv ("OSTREE_TEST_FALLOCATE_CALLED", "yes", 1);
    }
  const char *fault = getenv ("OSTREE_TEST_FALLOCATE");
  errno = fault && strcmp (fault, "enosys") == 0 ? ENOSYS : EOPNOTSUPP;
  return -1;
}

ssize_t
write (int fd, const void *buf, size_t len)
{
  if (fd == patch_fd)
    {
      setenv ("OSTREE_TEST_FALLOCATE_WRITTEN", "yes", 1);
      if (strcmp (getenv ("OSTREE_TEST_FALLOCATE"), "write") == 0)
        {
          errno = ENOSPC;
          return -1;
        }
    }
  return syscall (SYS_write, fd, buf, len);
}

int
fdatasync (int fd)
{
  if (fd == patch_fd)
    {
      setenv ("OSTREE_TEST_FALLOCATE_SYNCED", "yes", 1);
      if (strcmp (getenv ("OSTREE_TEST_FALLOCATE"), "sync") == 0)
        {
          errno = EIO;
          return -1;
        }
    }
  return syscall (SYS_fdatasync, fd);
}
