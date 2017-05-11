/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <glib.h>

/* Glibc uses readdir64 when _FILE_OFFSET_BITS == 64 as set by
 * AC_SYS_LARGEFILE on 32 bit systems.
 */
#if defined(_FILE_OFFSET_BITS) && (_FILE_OFFSET_BITS == 64)
# define READDIR "readdir64"
# define READDIR_R "readdir64_r"
#else
# define READDIR "readdir"
# define READDIR_R "readdir_r"
#endif

static GHashTable *direntcache;
static GMutex direntcache_lock;
static gsize initialized;

typedef struct {
  GPtrArray *entries;
  guint offset;
} DirEntries;

static void
dir_entries_free (gpointer data)
{
  DirEntries *d = data;
  g_ptr_array_unref (d->entries);
  g_free (d);
}

static DirEntries *
dir_entries_new (void)
{
  DirEntries *d = g_new0 (DirEntries, 1);
  d->entries = g_ptr_array_new_with_free_func (g_free);
  return d;
}

static void
ensure_initialized (void)
{
  if (g_once_init_enter (&initialized))
    {
      direntcache = g_hash_table_new_full (NULL, NULL, NULL, dir_entries_free);
      g_mutex_init (&direntcache_lock);
      g_once_init_leave (&initialized, 1);
    }
}

struct dirent *
readdir (DIR *dirp)
{
  struct dirent *(*real_readdir)(DIR *dirp) = dlsym (RTLD_NEXT, READDIR);
  struct dirent *ret;
  gboolean cache_another = TRUE;

  ensure_initialized ();

  /* The core idea here is that each time through the loop, we read a
   * directory entry.  If there is one, we choose whether to cache it
   * or to return it.  Because multiple entries can be cached,
   * ordering is randomized.  Statistically, the order will still be
   * *weighted* towards the ordering returned from the
   * kernel/filesystem, but the goal here is just to provide some
   * randomness in order to trigger bugs, not to be perfectly random.
   */
  while (cache_another)
    {
      DirEntries *de;

      errno = 0;
      ret = real_readdir (dirp);
      if (ret == NULL && errno != 0)
        goto out;

      g_mutex_lock (&direntcache_lock);
      de = g_hash_table_lookup (direntcache, dirp);
      if (ret)
        {
          if (g_random_boolean ())
            {
              struct dirent *copy;
              if (!de)
                {
                  de = dir_entries_new ();
                  g_hash_table_insert (direntcache, dirp, de);
                }
              copy = g_memdup (ret, sizeof (struct dirent));
              g_ptr_array_add (de->entries, copy);
            }
          else
            {
              cache_another = FALSE;
            }
        }
      else
        {
          if (de && de->offset < de->entries->len)
            {
              ret = de->entries->pdata[de->offset];
              de->offset++;
            }
          cache_another = FALSE;
        }
      g_mutex_unlock (&direntcache_lock);
    }

 out:
  return ret;
}

int
closedir (DIR *dirp)
{
  int (*real_closedir)(DIR *dirp) = dlsym (RTLD_NEXT, "closedir");

  ensure_initialized ();

  g_mutex_lock (&direntcache_lock);
  g_hash_table_remove (direntcache, dirp);
  g_mutex_unlock (&direntcache_lock);

  return real_closedir (dirp);
}

static void
assert_no_cached_entries (DIR *dirp)
{
  DirEntries *de;
  g_mutex_lock (&direntcache_lock);
  de = g_hash_table_lookup (direntcache, dirp);
  g_assert (!de || de->entries->len == 0);
  g_mutex_unlock (&direntcache_lock);
}

void
seekdir (DIR *dirp, long loc)
{
  void (*real_seekdir)(DIR *dirp, long loc) = dlsym (RTLD_NEXT, "seekdir");

  ensure_initialized ();

  /* For now, crash if seekdir is called when we have cached entries.
   * If some app wants to use this and seekdir() we can implement it.
   */
  assert_no_cached_entries (dirp);

  real_seekdir (dirp, loc);
}

void
rewinddir (DIR *dirp)
{
  void (*real_rewinddir)(DIR *dirp) = dlsym (RTLD_NEXT, "rewinddir");

  ensure_initialized ();

  /* Blow away the cache */
  g_mutex_lock (&direntcache_lock);
  g_hash_table_remove (direntcache, dirp);
  g_mutex_unlock (&direntcache_lock);

  real_rewinddir (dirp);
}

int
readdir_r (DIR *dirp, struct dirent *entry, struct dirent **result)
{
  int (*real_readdir_r)(DIR *dirp, struct dirent *entry, struct dirent **result) = dlsym (RTLD_NEXT, READDIR_R);

  ensure_initialized ();

  /* For now, assert that no one is mixing readdir_r() with readdir().
   * It'd be broken to do so, and very few programs use readdir_r()
   * anyways. */
  assert_no_cached_entries (dirp);

  return real_readdir_r (dirp, entry, result);
}
