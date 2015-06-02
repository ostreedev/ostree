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

struct linux_dirent {
  long           d_ino;
  off_t          d_off;
  unsigned short d_reclen;
  char           d_name[];
};

#define BUF_SIZE 1024

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
  struct dirent *(*real_readdir)(DIR *dirp) = dlsym (RTLD_NEXT, "readdir");
  struct dirent *ret;
  gboolean doloop = TRUE;
  
  ensure_initialized ();

  while (doloop)
    {
      DirEntries *de;
      GSList *l;

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
	      if (!de)
		{
		  de = dir_entries_new ();
		  g_hash_table_insert (direntcache, dirp, de);
		}
	      struct dirent *copy;
	      copy = g_memdup (ret, sizeof (struct dirent));
	      g_ptr_array_add (de->entries, copy);
	    }
	  else
	    {
	      doloop = FALSE;
	    }
	}
      else
	{
	  if (de && de->offset < de->entries->len)
	    {
	      ret = de->entries->pdata[de->offset];
	      de->offset++;
	    }
	  doloop = FALSE;
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
