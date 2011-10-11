/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "hacktree-repo.h"

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (HacktreeRepo, hacktree_repo, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), HACKTREE_TYPE_REPO, HacktreeRepoPrivate))

typedef struct _HacktreeRepoPrivate HacktreeRepoPrivate;

struct _HacktreeRepoPrivate {
  char *path;
};

static void
hacktree_repo_finalize (GObject *object)
{
  G_OBJECT_CLASS (hacktree_repo_parent_class)->finalize (object);
}

static void
hacktree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      priv->path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hacktree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, priv->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hacktree_repo_class_init (HacktreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (HacktreeRepoPrivate));

  object_class->finalize = hacktree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
hacktree_repo_init (HacktreeRepo *self)
{
}

HacktreeRepo*
hacktree_repo_new (const char *path)
{
  return g_object_new (HACKTREE_TYPE_REPO, "path", path, NULL);
}

gboolean
hacktree_repo_import (HacktreeRepo *repo,
                      const char   *path)
{

}
