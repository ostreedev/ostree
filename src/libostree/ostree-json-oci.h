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

#include "ostree-json.h"

G_BEGIN_DECLS

#define OSTREE_OCI_MEDIA_TYPE_DESCRIPTOR "application/vnd.oci.descriptor.v1+json"
#define OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFEST "application/vnd.oci.image.manifest.v1+json"
#define OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFESTLIST "application/vnd.oci.image.manifest.list.v1+json"
#define OSTREE_OCI_MEDIA_TYPE_IMAGE_LAYER "application/vnd.oci.image.layer.v1.tar+gzip"
#define OSTREE_OCI_MEDIA_TYPE_IMAGE_LAYER_NONDISTRIBUTABLE "application/vnd.oci.image.layer.nondistributable.v1.tar+gzip"
#define OSTREE_OCI_MEDIA_TYPE_IMAGE_CONFIG "application/vnd.oci.image.config.v1+json"

#define OSTREE_TYPE_OCI_REF ostree_oci_ref_get_type ()
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeOciRef, ostree_oci_ref, OSTREE_OCI, REF, OstreeJson)

_OSTREE_PUBLIC
OstreeOciRef * ostree_oci_ref_new (const char *mediatype,
                                   const char *digest,
                                   gint64 size);
_OSTREE_PUBLIC
const char * ostree_oci_ref_get_mediatype (OstreeOciRef *self);
_OSTREE_PUBLIC
const char * ostree_oci_ref_get_digest (OstreeOciRef *self);
_OSTREE_PUBLIC
gint64 ostree_oci_ref_get_size (OstreeOciRef *self);
_OSTREE_PUBLIC
const char ** ostree_oci_ref_get_urls (OstreeOciRef *self);
_OSTREE_PUBLIC
void ostree_oci_ref_set_urls (OstreeOciRef *self,
                              const char **urls);

#define OSTREE_TYPE_OCI_VERSIONED ostree_oci_versioned_get_type ()
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeOciVersioned, ostree_oci_versioned, OSTREE_OCI, VERSIONED, OstreeJson)

_OSTREE_PUBLIC
OstreeOciVersioned * ostree_oci_versioned_from_json (GBytes *bytes,
                                                     GError **error);

_OSTREE_PUBLIC
const char * ostree_oci_versioned_get_mediatype (OstreeOciVersioned *self);
_OSTREE_PUBLIC
gint64 ostree_oci_versioned_get_version (OstreeOciVersioned *self);

#define OSTREE_TYPE_OCI_MANIFEST ostree_oci_manifest_get_type ()
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeOciManifest, ostree_oci_manifest, OSTREE, OCI_MANIFEST, OstreeOciVersioned)

_OSTREE_PUBLIC
OstreeOciManifest * ostree_oci_manifest_new (void);
_OSTREE_PUBLIC
void ostree_oci_manifest_set_config (OstreeOciManifest *self,
                                     OstreeOciRef *ref);
_OSTREE_PUBLIC
void ostree_oci_manifest_set_layers (OstreeOciManifest *self,
                                     OstreeOciRef **refs);
_OSTREE_PUBLIC
int ostree_oci_manifest_get_n_layers (OstreeOciManifest *self);
_OSTREE_PUBLIC
const char *ostree_oci_manifest_get_layer_digest (OstreeOciManifest *self,
                                                  int i);

_OSTREE_PUBLIC
GHashTable * ostree_oci_manifest_get_annotations (OstreeOciManifest *self);


#define OSTREE_TYPE_OCI_MANIFEST_LIST ostree_oci_manifest_list_get_type ()
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeOciManifestList, ostree_oci_manifest_list, OSTREE, OCI_MANIFEST_LIST, OstreeOciVersioned)

#define OSTREE_TYPE_OCI_IMAGE ostree_oci_image_get_type ()
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeOciImage, ostree_oci_image, OSTREE, OCI_IMAGE, OstreeJson)

_OSTREE_PUBLIC
OstreeOciImage * ostree_oci_image_new (void);

_OSTREE_PUBLIC
void ostree_oci_image_set_created (OstreeOciImage *image,
                                   const char *created);
_OSTREE_PUBLIC
void ostree_oci_image_set_architecture (OstreeOciImage *image,
                                        const char *arch);
_OSTREE_PUBLIC
void ostree_oci_image_set_os (OstreeOciImage *image,
                              const char *os);
_OSTREE_PUBLIC
void ostree_oci_image_set_layers (OstreeOciImage *image,
                                  const char **layers);


_OSTREE_PUBLIC
void ostree_oci_add_annotations_for_commit (GHashTable *annotations,
                                            const char *commit,
                                            GVariant *commit_data);
_OSTREE_PUBLIC
void ostree_oci_parse_commit_annotations (GHashTable *annotations,
                                          guint64 *out_timestamp,
                                          char **out_subject,
                                          char **out_body,
                                          char **out_commit,
                                          char **out_parent_commit,
                                          GVariantBuilder *metadata_builder);
