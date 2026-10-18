/* SPDX-License-Identifier: LGPL-2.0+ */

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

/* Preloaded only by test-delta-fallocate.sh. The reference object has 65538
 * bytes; the patch output has 65537, so faults affect only the patch tempfile.
 * Mock the posix_fallocate contract, not libc's internal fallback implementation.
 */
int
posix_fallocate (int fd, off_t offset, off_t len)
{
  if (len == 65537)
    {
      int called = getenv ("OSTREE_TEST_FALLOCATE_CALLED") != NULL;
      setenv ("OSTREE_TEST_FALLOCATE_CALLED", "yes", 1);
      const char *fault = getenv ("OSTREE_TEST_FALLOCATE");
      /* Do not set errno: callers must use the returned error number. */
      if (strcmp (fault, "unsupported") == 0)
        return EOPNOTSUPP;
      if (strcmp (fault, "enosys") == 0)
        return ENOSYS;
      if (strcmp (fault, "enospc") == 0)
        return ENOSPC;
      if (strcmp (fault, "eio") == 0)
        return EIO;
      if (strcmp (fault, "eintr") == 0 && !called)
        return EINTR;
    }
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
  const char *symbol = "posix_fallocate64";
#else
  const char *symbol = "posix_fallocate";
#endif
  int (*real_posix_fallocate) (int, off_t, off_t) = dlsym (RTLD_NEXT, symbol);
  if (!real_posix_fallocate)
    abort ();
  return real_posix_fallocate (fd, offset, len);
}
