/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree-repo-file-enumerator.h"
#include <string.h>

struct _OstreeRepoFileEnumerator
{
  GFileEnumerator parent;

  OstreeRepoFile *dir;
  char *attributes;
  GFileQueryInfoFlags flags;

  int index;
};

#define ostree_repo_file_enumerator_get_type _ostree_repo_file_enumerator_get_type
G_DEFINE_TYPE (OstreeRepoFileEnumerator, ostree_repo_file_enumerator, G_TYPE_FILE_ENUMERATOR);

static GFileInfo *ostree_repo_file_enumerator_next_file (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);
static gboolean   ostree_repo_file_enumerator_close     (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);


static void
ostree_repo_file_enumerator_dispose (GObject *object)
{
  OstreeRepoFileEnumerator *self;

  self = OSTREE_REPO_FILE_ENUMERATOR (object);

  g_clear_object (&self->dir);
  g_free (self->attributes);
  
  if (G_OBJECT_CLASS (ostree_repo_file_enumerator_parent_class)->dispose)
    G_OBJECT_CLASS (ostree_repo_file_enumerator_parent_class)->dispose (object);
}

static void
ostree_repo_file_enumerator_finalize (GObject *object)
{
  OstreeRepoFileEnumerator *self;

  self = OSTREE_REPO_FILE_ENUMERATOR (object);
  (void)self;

  G_OBJECT_CLASS (ostree_repo_file_enumerator_parent_class)->finalize (object);
}


static void
ostree_repo_file_enumerator_class_init (OstreeRepoFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = ostree_repo_file_enumerator_finalize;
  gobject_class->dispose = ostree_repo_file_enumerator_dispose;

  enumerator_class->next_file = ostree_repo_file_enumerator_next_file;
  enumerator_class->close_fn = ostree_repo_file_enumerator_close;
}

static void
ostree_repo_file_enumerator_init (OstreeRepoFileEnumerator *self)
{
}

GFileEnumerator *
_ostree_repo_file_enumerator_new (OstreeRepoFile       *dir,
				  const char           *attributes,
				  GFileQueryInfoFlags   flags,
				  GCancellable         *cancellable,
				  GError              **error)
{
  OstreeRepoFileEnumerator *self;
  
  self = g_object_new (OSTREE_TYPE_REPO_FILE_ENUMERATOR,
		       "container", dir,
		       NULL);

  self->dir = g_object_ref (dir);
  self->attributes = g_strdup (attributes);
  self->flags = flags;
  
  return G_FILE_ENUMERATOR (self);
}

static GFileInfo *
ostree_repo_file_enumerator_next_file (GFileEnumerator  *enumerator,
				       GCancellable     *cancellable,
				       GError          **error)
{
  OstreeRepoFileEnumerator *self = OSTREE_REPO_FILE_ENUMERATOR (enumerator);
  gboolean ret = FALSE;
  GFileInfo *info = NULL;

  if (!ostree_repo_file_tree_query_child (self->dir, self->index,
                                          self->attributes, self->flags,
                                          &info, cancellable, error))
    goto out;

  self->index++;

  ret = TRUE;
 out:
  if (!ret)
    g_clear_object (&info);
  return info;
}

static gboolean
ostree_repo_file_enumerator_close (GFileEnumerator  *enumerator,
				   GCancellable     *cancellable,
				   GError          **error)
{
  return TRUE;
}
