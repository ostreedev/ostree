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

#include "ostree-json-oci.h"
#include "ostree-json-private.h"

G_BEGIN_DECLS

typedef struct {
  char *mediatype;
  char *digest;
  gint64 size;
  char **urls;
} OstreeOciDescriptor;

void ostree_oci_descriptor_destroy (OstreeOciDescriptor *self);
void ostree_oci_descriptor_free (OstreeOciDescriptor *self);

typedef struct
{
  char *architecture;
  char *os;
  char *os_version;
  char **os_features;
  char *variant;
  char **features;
} OstreeOciManifestPlatform;


typedef struct
{
  OstreeOciDescriptor parent;
  OstreeOciManifestPlatform platform;
} OstreeOciManifestDescriptor;

void ostree_oci_manifest_descriptor_destroy (OstreeOciManifestDescriptor *self);
void ostree_oci_manifest_descriptor_free (OstreeOciManifestDescriptor *self);

struct _OstreeOciRef {
  OstreeJson parent;

  OstreeOciDescriptor descriptor;
};

struct _OstreeOciRefClass {
  OstreeJsonClass parent_class;
};

struct _OstreeOciVersioned {
  OstreeJson parent;

  int version;
  char *mediatype;
};

struct _OstreeOciVersionedClass {
  OstreeJsonClass parent_class;
};

struct _OstreeOciManifest
{
  OstreeOciVersioned parent;

  OstreeOciDescriptor config;
  OstreeOciDescriptor **layers;
  GHashTable     *annotations;
};

struct _OstreeOciManifestClass
{
  OstreeOciVersionedClass parent_class;
};

struct _OstreeOciManifestList
{
  OstreeOciVersioned parent;

  OstreeOciManifestDescriptor **manifests;
  GHashTable     *annotations;
};

struct _OstreeOciManifestListClass
{
  OstreeOciVersionedClass parent_class;
};

typedef struct
{
  char *type;
  char **diff_ids;
} OstreeOciImageRootfs;

typedef struct
{
  char *user;
  char *working_dir;
  gint64 memory;
  gint64 memory_swap;
  gint64 cpu_shares;
  char **env;
  char **cmd;
  char **entrypoint;
  char **exposed_ports;
  char **volumes;
  GHashTable *labels;
} OstreeOciImageConfig;

typedef struct
{
  char *created;
  char *created_by;
  char *author;
  char *comment;
  gboolean empty_layer;
} OstreeOciImageHistory;

struct _OstreeOciImage
{
  OstreeJson parent;

  char *created;
  char *author;
  char *architecture;
  char *os;
  OstreeOciImageRootfs rootfs;
  OstreeOciImageConfig config;
  OstreeOciImageHistory **history;
};

struct _OstreeOciImageClass
{
  OstreeJsonClass parent_class;
};

G_END_DECLS
