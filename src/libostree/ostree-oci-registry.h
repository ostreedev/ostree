/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#pragma once

#include "libglnx/libglnx.h"

#include <glib.h>
#include <gio/gio.h>
#include <archive.h>
#include "ostree-json-oci.h"

#define OSTREE_TYPE_OCI_REGISTRY ostree_oci_registry_get_type ()
#define OSTREE_OCI_REGISTRY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_OCI_REGISTRY, OstreeOciRegistry))
#define OSTREE_IS_OCI_REGISTRY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_OCI_REGISTRY))

_OSTREE_PUBLIC
GType ostree_oci_registry_get_type (void);

typedef struct OstreeOciRegistry OstreeOciRegistry;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeOciRegistry, g_object_unref)

#define OSTREE_TYPE_OCI_LAYER_WRITER ostree_oci_layer_writer_get_type ()
#define OSTREE_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_OCI_LAYER_WRITER, OstreeOciLayerWriter))
#define OSTREE_IS_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_OCI_LAYER_WRITER))

_OSTREE_PUBLIC
GType ostree_oci_layer_writer_get_type (void);

typedef struct OstreeOciLayerWriter OstreeOciLayerWriter;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeOciLayerWriter, g_object_unref)

_OSTREE_PUBLIC
OstreeOciRegistry  *ostree_oci_registry_new                  (const char           *uri,
                                                              gboolean              for_write,
                                                              int                   tmp_dfd,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
OstreeOciRef       *ostree_oci_registry_load_ref             (OstreeOciRegistry    *self,
                                                              const char           *ref,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
gboolean            ostree_oci_registry_set_ref              (OstreeOciRegistry    *self,
                                                              const char           *ref,
                                                              OstreeOciRef         *data,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
void                ostree_oci_registry_download_blob        (OstreeOciRegistry    *self,
                                                              const char           *digest,
                                                              GCancellable         *cancellable,
                                                              GAsyncReadyCallback   callback,
                                                              gpointer              user_data);
_OSTREE_PUBLIC
int                 ostree_oci_registry_download_blob_finish (OstreeOciRegistry    *self,
                                                              GAsyncResult         *result,
                                                              GError              **error);
_OSTREE_PUBLIC
GBytes             *ostree_oci_registry_load_blob            (OstreeOciRegistry    *self,
                                                              const char           *digest,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
char *              ostree_oci_registry_store_blob           (OstreeOciRegistry    *self,
                                                              GBytes               *data,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
OstreeOciRef *      ostree_oci_registry_store_json           (OstreeOciRegistry    *self,
                                                              OstreeJson           *json,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
OstreeOciVersioned *ostree_oci_registry_load_versioned       (OstreeOciRegistry    *self,
                                                              const char           *digest,
                                                              GCancellable         *cancellable,
                                                              GError              **error);
_OSTREE_PUBLIC
OstreeOciLayerWriter *ostree_oci_registry_write_layer        (OstreeOciRegistry    *self,
                                                              GCancellable         *cancellable,
                                                              GError              **error);


_OSTREE_PUBLIC
struct archive *ostree_oci_layer_writer_get_archive (OstreeOciLayerWriter  *self);
_OSTREE_PUBLIC
gboolean        ostree_oci_layer_writer_close       (OstreeOciLayerWriter  *self,
                                                     char                 **uncompressed_digest_out,
                                                     OstreeOciRef         **ref_out,
                                                     GCancellable          *cancellable,
                                                     GError               **error);
