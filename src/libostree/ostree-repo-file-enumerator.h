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

#pragma once

#include "ostree-repo-file.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO_FILE_ENUMERATOR         (_ostree_repo_file_enumerator_get_type ())
#define OSTREE_REPO_FILE_ENUMERATOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_REPO_FILE_ENUMERATOR, OstreeRepoFileEnumerator))
#define OSTREE_REPO_FILE_ENUMERATOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_REPO_FILE_ENUMERATOR, OstreeRepoFileEnumeratorClass))
#define OSTREE_IS_REPO_FILE_ENUMERATOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_REPO_FILE_ENUMERATOR))
#define OSTREE_IS_REPO_FILE_ENUMERATOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_REPO_FILE_ENUMERATOR))
#define OSTREE_REPO_FILE_ENUMERATOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_REPO_FILE_ENUMERATOR, OstreeRepoFileEnumeratorClass))

typedef struct _OstreeRepoFileEnumerator        OstreeRepoFileEnumerator;
typedef struct _OstreeRepoFileEnumeratorClass   OstreeRepoFileEnumeratorClass;

struct _OstreeRepoFileEnumeratorClass
{
  GFileEnumeratorClass parent_class;
};

G_GNUC_INTERNAL
GType   _ostree_repo_file_enumerator_get_type (void) G_GNUC_CONST;

G_GNUC_INTERNAL
GFileEnumerator * _ostree_repo_file_enumerator_new      (OstreeRepoFile       *dir,
							 const char           *attributes,
							 GFileQueryInfoFlags   flags,
							 GCancellable         *cancellable,
							 GError              **error);

G_END_DECLS
