/*
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include "string.h"

#include "ostree-json-oci-private.h"
#include "libglnx.h"

void
ostree_oci_descriptor_destroy (OstreeOciDescriptor *self)
{
  g_free (self->mediatype);
  g_free (self->digest);
  g_strfreev (self->urls);
}

void
ostree_oci_descriptor_free (OstreeOciDescriptor *self)
{
  ostree_oci_descriptor_destroy (self);
  g_free (self);
}

static OstreeJsonProp ostree_oci_descriptor_props[] = {
  OSTREE_JSON_STRING_PROP (OstreeOciDescriptor, mediatype, "mediaType"),
  OSTREE_JSON_STRING_PROP (OstreeOciDescriptor, digest, "digest"),
  OSTREE_JSON_INT64_PROP (OstreeOciDescriptor, size, "size"),
  OSTREE_JSON_STRV_PROP (OstreeOciDescriptor, urls, "urls"),
  OSTREE_JSON_LAST_PROP
};

static void
ostree_oci_manifest_platform_destroy (OstreeOciManifestPlatform *self)
{
  g_free (self->architecture);
  g_free (self->os);
  g_free (self->os_version);
  g_strfreev (self->os_features);
  g_free (self->variant);
  g_strfreev (self->features);
}

void
ostree_oci_manifest_descriptor_destroy (OstreeOciManifestDescriptor *self)
{
  ostree_oci_manifest_platform_destroy (&self->platform);
  ostree_oci_descriptor_destroy (&self->parent);
}

void
ostree_oci_manifest_descriptor_free (OstreeOciManifestDescriptor *self)
{
  ostree_oci_manifest_descriptor_destroy (self);
  g_free (self);
}

static OstreeJsonProp ostree_oci_manifest_platform_props[] = {
  OSTREE_JSON_STRING_PROP (OstreeOciManifestPlatform, architecture, "architecture"),
  OSTREE_JSON_STRING_PROP (OstreeOciManifestPlatform, os, "os"),
  OSTREE_JSON_STRING_PROP (OstreeOciManifestPlatform, os_version, "os.version"),
  OSTREE_JSON_STRING_PROP (OstreeOciManifestPlatform, variant, "variant"),
  OSTREE_JSON_STRV_PROP (OstreeOciManifestPlatform, os_features, "os.features"),
  OSTREE_JSON_STRV_PROP (OstreeOciManifestPlatform, features, "features"),
  OSTREE_JSON_LAST_PROP
};
static OstreeJsonProp ostree_oci_manifest_descriptor_props[] = {
  OSTREE_JSON_PARENT_PROP (OstreeOciManifestDescriptor, parent, ostree_oci_descriptor_props),
  OSTREE_JSON_STRUCT_PROP (OstreeOciManifestDescriptor, platform, "platform", ostree_oci_manifest_platform_props),
  OSTREE_JSON_LAST_PROP
};

G_DEFINE_TYPE (OstreeOciRef, ostree_oci_ref, OSTREE_TYPE_JSON);

static void
ostree_oci_ref_finalize (GObject *object)
{
  OstreeOciRef *self = OSTREE_OCI_REF (object);

  ostree_oci_descriptor_destroy (&self->descriptor);

  G_OBJECT_CLASS (ostree_oci_ref_parent_class)->finalize (object);
}

static void
ostree_oci_ref_class_init (OstreeOciRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  OstreeJsonClass *json_class = OSTREE_JSON_CLASS (klass);
  static OstreeJsonProp props[] = {
    OSTREE_JSON_PARENT_PROP (OstreeOciRef, descriptor, ostree_oci_descriptor_props),
    OSTREE_JSON_LAST_PROP
  };

  object_class->finalize = ostree_oci_ref_finalize;
  json_class->props = props;
  json_class->mediatype = OSTREE_OCI_MEDIA_TYPE_DESCRIPTOR;
}

static void
ostree_oci_ref_init (OstreeOciRef *self)
{
}

OstreeOciRef *
ostree_oci_ref_new (const char *mediatype,
                    const char *digest,
                    gint64 size)
{
  OstreeOciRef *ref;

  ref = g_object_new (OSTREE_TYPE_OCI_REF, NULL);
  ref->descriptor.mediatype = g_strdup (mediatype);
  ref->descriptor.digest = g_strdup (digest);
  ref->descriptor.size = size;

  return ref;
}

const char *
ostree_oci_ref_get_mediatype (OstreeOciRef *self)
{
  return self->descriptor.mediatype;
}

const char *
ostree_oci_ref_get_digest (OstreeOciRef *self)
{
  return self->descriptor.digest;
}

gint64
ostree_oci_ref_get_size (OstreeOciRef *self)
{
  return self->descriptor.size;
}

const char **
ostree_oci_ref_get_urls (OstreeOciRef *self)
{
  return (const char **)self->descriptor.urls;
}

void
ostree_oci_ref_set_urls (OstreeOciRef *self,
                         const char **urls)
{
  g_strfreev (self->descriptor.urls);
  self->descriptor.urls = g_strdupv ((char **)urls);
}

G_DEFINE_TYPE (OstreeOciVersioned, ostree_oci_versioned, OSTREE_TYPE_JSON);

static void
ostree_oci_versioned_finalize (GObject *object)
{
  OstreeOciVersioned *self = OSTREE_OCI_VERSIONED (object);

  g_free (self->mediatype);

  G_OBJECT_CLASS (ostree_oci_versioned_parent_class)->finalize (object);
}

static void
ostree_oci_versioned_class_init (OstreeOciVersionedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  OstreeJsonClass *json_class = OSTREE_JSON_CLASS (klass);
  static OstreeJsonProp props[] = {
    OSTREE_JSON_INT64_PROP (OstreeOciVersioned, version, "schemaVersion"),
    OSTREE_JSON_STRING_PROP (OstreeOciVersioned, mediatype, "mediaType"),
    OSTREE_JSON_LAST_PROP
  };

  object_class->finalize = ostree_oci_versioned_finalize;
  json_class->props = props;
}

static void
ostree_oci_versioned_init (OstreeOciVersioned *self)
{
}

OstreeOciVersioned *
ostree_oci_versioned_from_json (GBytes *bytes, GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;
  const gchar *mediatype;
  JsonObject *object;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);
  object = json_node_get_object (root);

  mediatype = json_object_get_string_member (object, "mediaType");
  if (mediatype == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Versioned object lacks mediatype");
      return NULL;
    }

  if (strcmp (mediatype, OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFEST) == 0)
    return (OstreeOciVersioned *) ostree_json_from_node (root, OSTREE_TYPE_OCI_MANIFEST, error);

  if (strcmp (mediatype, OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFESTLIST) == 0)
    return (OstreeOciVersioned *) ostree_json_from_node (root, OSTREE_TYPE_OCI_MANIFEST_LIST, error);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
               "Unsupported media type %s", mediatype);
  return NULL;
}

const char *
ostree_oci_versioned_get_mediatype (OstreeOciVersioned *self)
{
  return self->mediatype;
}

gint64
ostree_oci_versioned_get_version (OstreeOciVersioned *self)
{
  return self->version;
}

G_DEFINE_TYPE (OstreeOciManifest, ostree_oci_manifest, OSTREE_TYPE_OCI_VERSIONED);

static void
ostree_oci_manifest_finalize (GObject *object)
{
  OstreeOciManifest *self = (OstreeOciManifest *) object;
  int i;

  for (i = 0; self->layers != NULL && self->layers[i] != NULL; i++)
    ostree_oci_descriptor_free (self->layers[i]);
  g_free (self->layers);
  ostree_oci_descriptor_destroy (&self->config);
  if (self->annotations)
    g_hash_table_destroy (self->annotations);

  G_OBJECT_CLASS (ostree_oci_manifest_parent_class)->finalize (object);
}

static void
ostree_oci_manifest_class_init (OstreeOciManifestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  OstreeJsonClass *json_class = OSTREE_JSON_CLASS (klass);
  static OstreeJsonProp props[] = {
    OSTREE_JSON_STRUCT_PROP(OstreeOciManifest, config, "config", ostree_oci_descriptor_props),
    OSTREE_JSON_STRUCTV_PROP(OstreeOciManifest, layers, "layers", ostree_oci_descriptor_props),
    OSTREE_JSON_STRMAP_PROP(OstreeOciManifest, annotations, "annotations"),
    OSTREE_JSON_LAST_PROP
  };

  object_class->finalize = ostree_oci_manifest_finalize;
  json_class->props = props;
  json_class->mediatype = OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFEST;
}

static void
ostree_oci_manifest_init (OstreeOciManifest *self)
{
}

OstreeOciManifest *
ostree_oci_manifest_new (void)
{
  OstreeOciManifest *manifest;

  manifest = g_object_new (OSTREE_TYPE_OCI_MANIFEST, NULL);
  manifest->parent.version = 2;
  manifest->parent.mediatype = g_strdup (OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFEST);

  manifest->annotations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  return manifest;
}

void
ostree_oci_manifest_set_config (OstreeOciManifest *self,
                                OstreeOciRef *ref)
{
  g_free (self->config.mediatype);
  self->config.mediatype = g_strdup (ref->descriptor.mediatype);
  g_free (self->config.digest);
  self->config.digest = g_strdup (ref->descriptor.digest);
  self->config.size = ref->descriptor.size;
}

static int
ptrv_count (gpointer *ptrs)
{
  int count;

  for (count = 0; ptrs != NULL && ptrs[count] != NULL; count++)
    ;

  return count;
}

void
ostree_oci_manifest_set_layers (OstreeOciManifest *self,
                                OstreeOciRef **refs)
{
  int i, count;

  for (i = 0; self->layers != NULL && self->layers[i] != NULL; i++)
    ostree_oci_descriptor_free (self->layers[i]);
  g_free (self->layers);

  count = ptrv_count ((gpointer *)refs);

  self->layers = g_new0 (OstreeOciDescriptor *, count + 1);
  for (i = 0; i < count; i++)
    {
      self->layers[i] = g_new0 (OstreeOciDescriptor, 1);
      self->layers[i]->mediatype = g_strdup (refs[i]->descriptor.mediatype);
      self->layers[i]->digest = g_strdup (refs[i]->descriptor.digest);
      self->layers[i]->size = refs[i]->descriptor.size;
    }
}

int
ostree_oci_manifest_get_n_layers (OstreeOciManifest *self)
{
  return ptrv_count ((gpointer *)self->layers);
}

const char *
ostree_oci_manifest_get_layer_digest (OstreeOciManifest *self,
                                      int i)
{
  return self->layers[i]->digest;
}

GHashTable *
ostree_oci_manifest_get_annotations (OstreeOciManifest *self)
{
  return self->annotations;
}

G_DEFINE_TYPE (OstreeOciManifestList, ostree_oci_manifest_list, OSTREE_TYPE_OCI_VERSIONED);

static void
ostree_oci_manifest_list_finalize (GObject *object)
{
  OstreeOciManifestList *self = (OstreeOciManifestList *) object;
  int i;

  for (i = 0; self->manifests != NULL && self->manifests[i] != NULL; i++)
    ostree_oci_manifest_descriptor_free (self->manifests[i]);
  g_free (self->manifests);

  if (self->annotations)
    g_hash_table_destroy (self->annotations);

  G_OBJECT_CLASS (ostree_oci_manifest_list_parent_class)->finalize (object);
}


static void
ostree_oci_manifest_list_class_init (OstreeOciManifestListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  OstreeJsonClass *json_class = OSTREE_JSON_CLASS (klass);
  static OstreeJsonProp props[] = {
    OSTREE_JSON_STRUCTV_PROP(OstreeOciManifestList, manifests, "manifests", ostree_oci_manifest_descriptor_props),
    OSTREE_JSON_STRMAP_PROP(OstreeOciManifestList, annotations, "annotations"),
    OSTREE_JSON_LAST_PROP
  };

  object_class->finalize = ostree_oci_manifest_list_finalize;
  json_class->props = props;
  json_class->mediatype = OSTREE_OCI_MEDIA_TYPE_IMAGE_MANIFESTLIST;
}

static void
ostree_oci_manifest_list_init (OstreeOciManifestList *self)
{
}

G_DEFINE_TYPE (OstreeOciImage, ostree_oci_image, OSTREE_TYPE_JSON);

static void
ostree_oci_image_rootfs_destroy (OstreeOciImageRootfs *self)
{
  g_free (self->type);
  g_strfreev (self->diff_ids);
}

static void
ostree_oci_image_config_destroy (OstreeOciImageConfig *self)
{
  g_free (self->user);
  g_free (self->working_dir);
  g_strfreev (self->env);
  g_strfreev (self->cmd);
  g_strfreev (self->entrypoint);
  g_strfreev (self->exposed_ports);
  g_strfreev (self->volumes);
  if (self->labels)
    g_hash_table_destroy (self->labels);
}

static void
ostree_oci_image_history_free (OstreeOciImageHistory *self)
{
  g_free (self->created);
  g_free (self->created_by);
  g_free (self->author);
  g_free (self->comment);
  g_free (self);
}

static void
ostree_oci_image_finalize (GObject *object)
{
  OstreeOciImage *self = (OstreeOciImage *) object;
  int i;

  g_free (self->created);
  g_free (self->author);
  g_free (self->architecture);
  g_free (self->os);
  ostree_oci_image_rootfs_destroy (&self->rootfs);
  ostree_oci_image_config_destroy (&self->config);

  for (i = 0; self->history != NULL && self->history[i] != NULL; i++)
    ostree_oci_image_history_free (self->history[i]);
  g_free (self->history);

  G_OBJECT_CLASS (ostree_oci_image_parent_class)->finalize (object);
}

static void
ostree_oci_image_class_init (OstreeOciImageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  OstreeJsonClass *json_class = OSTREE_JSON_CLASS (klass);
  static OstreeJsonProp config_props[] = {
    OSTREE_JSON_STRING_PROP (OstreeOciImageConfig, user, "User"),
    OSTREE_JSON_INT64_PROP (OstreeOciImageConfig, memory, "Memory"),
    OSTREE_JSON_INT64_PROP (OstreeOciImageConfig, memory_swap, "MemorySwap"),
    OSTREE_JSON_INT64_PROP (OstreeOciImageConfig, cpu_shares, "CpuShares"),
    OSTREE_JSON_BOOLMAP_PROP (OstreeOciImageConfig, exposed_ports, "ExposedPorts"),
    OSTREE_JSON_STRV_PROP (OstreeOciImageConfig, env, "Env"),
    OSTREE_JSON_STRV_PROP (OstreeOciImageConfig, entrypoint, "Entrypoint"),
    OSTREE_JSON_STRV_PROP (OstreeOciImageConfig, cmd, "Cmd"),
    OSTREE_JSON_BOOLMAP_PROP (OstreeOciImageConfig, volumes, "Volumes"),
    OSTREE_JSON_STRING_PROP (OstreeOciImageConfig, working_dir, "WorkingDir"),
    OSTREE_JSON_STRMAP_PROP(OstreeOciImageConfig, labels, "Labels"),
    OSTREE_JSON_LAST_PROP
  };
  static OstreeJsonProp rootfs_props[] = {
    OSTREE_JSON_STRING_PROP (OstreeOciImageRootfs, type, "type"),
    OSTREE_JSON_STRV_PROP (OstreeOciImageRootfs, diff_ids, "diff_ids"),
    OSTREE_JSON_LAST_PROP
  };
  static OstreeJsonProp history_props[] = {
    OSTREE_JSON_STRING_PROP (OstreeOciImageHistory, created, "created"),
    OSTREE_JSON_STRING_PROP (OstreeOciImageHistory, created_by, "created_by"),
    OSTREE_JSON_STRING_PROP (OstreeOciImageHistory, author, "author"),
    OSTREE_JSON_STRING_PROP (OstreeOciImageHistory, comment, "comment"),
    OSTREE_JSON_BOOL_PROP (OstreeOciImageHistory, empty_layer, "empty_layer"),
    OSTREE_JSON_LAST_PROP
  };
  static OstreeJsonProp props[] = {
    OSTREE_JSON_STRING_PROP (OstreeOciImage, created, "created"),
    OSTREE_JSON_STRING_PROP (OstreeOciImage, author, "author"),
    OSTREE_JSON_STRING_PROP (OstreeOciImage, architecture, "architecture"),
    OSTREE_JSON_STRING_PROP (OstreeOciImage, os, "os"),
    OSTREE_JSON_STRUCT_PROP (OstreeOciImage, config, "config", config_props),
    OSTREE_JSON_STRUCT_PROP (OstreeOciImage, rootfs, "rootfs", rootfs_props),
    OSTREE_JSON_STRUCTV_PROP (OstreeOciImage, history, "history", history_props),
    OSTREE_JSON_LAST_PROP
  };

  object_class->finalize = ostree_oci_image_finalize;
  json_class->props = props;
  json_class->mediatype = OSTREE_OCI_MEDIA_TYPE_IMAGE_CONFIG;
}

static void
ostree_oci_image_init (OstreeOciImage *self)
{
}

OstreeOciImage *
ostree_oci_image_new (void)
{
  OstreeOciImage *image;
  GTimeVal stamp;

  stamp.tv_sec = time (NULL);
  stamp.tv_usec = 0;

  image = g_object_new (OSTREE_TYPE_OCI_IMAGE, NULL);

  /* Some default values */
  image->created = g_time_val_to_iso8601 (&stamp);
  image->architecture = g_strdup ("arm64");
  image->os = g_strdup ("linux");

  image->rootfs.type = g_strdup ("layers");
  image->rootfs.diff_ids = g_new0 (char *, 1);

  return image;
}

void
ostree_oci_image_set_created (OstreeOciImage *image,
                              const char *created)
{
  g_free (image->created);
  image->created = g_strdup (created);
}

void
ostree_oci_image_set_architecture (OstreeOciImage *image,
                                   const char *arch)
{
  g_free (image->architecture);
  image->architecture = g_strdup (arch);
}

void
ostree_oci_image_set_os (OstreeOciImage *image,
                         const char *os)
{
  g_free (image->os);
  image->os = g_strdup (os);
}

void
ostree_oci_image_set_layers (OstreeOciImage *image,
                             const char **layers)
{
  g_strfreev (image->rootfs.diff_ids);
  image->rootfs.diff_ids = g_strdupv ((char **)layers);
}

static void
add_annotation (GHashTable *annotations, const char *key, const char *value)
{
    g_hash_table_replace (annotations,
                          g_strdup (key),
                          g_strdup (value));
}

void
ostree_oci_add_annotations_for_commit (GHashTable *annotations,
                                       const char *commit,
                                       GVariant *commit_data)
{
  if (commit)
    add_annotation (annotations,"io.github.ostreedev.Commit", commit);

  if (commit_data)
    {
      g_autofree char *parent = NULL;
      g_autofree char *subject = NULL;
      g_autofree char *body = NULL;
      g_autofree char *timestamp = NULL;
      g_autoptr(GVariant) metadata = NULL;
      int i;

      parent = ostree_commit_get_parent (commit_data);
      if (parent)
        add_annotation (annotations, "io.github.ostreedev.ParentCommit", parent);

      metadata = g_variant_get_child_value (commit_data, 0);
      for (i = 0; i < g_variant_n_children (metadata); i++)
        {
          g_autoptr(GVariant) elm = g_variant_get_child_value (metadata, i);
          g_autoptr(GVariant) value = g_variant_get_child_value (elm, 1);
          g_autofree char *key = NULL;
          g_autofree char *full_key = NULL;
          g_autofree char *value_base64 = NULL;

          g_variant_get_child (elm, 0, "s", &key);
          full_key = g_strdup_printf ("io.github.ostreedev.Metadata.%s", key);

          value_base64 = g_base64_encode (g_variant_get_data (value), g_variant_get_size (value));
          add_annotation (annotations, full_key, value_base64);
        }

      timestamp = g_strdup_printf ("%"G_GUINT64_FORMAT, ostree_commit_get_timestamp (commit_data));
      add_annotation (annotations, "io.github.ostreedev.Timestamp", timestamp);

      g_variant_get_child (commit_data, 3, "s", &subject);
      add_annotation (annotations, "io.github.ostreedev.Subject", subject);

      g_variant_get_child (commit_data, 4, "s", &body);
      add_annotation (annotations, "io.github.ostreedev.Body", body);
   }
}

void
ostree_oci_parse_commit_annotations (GHashTable *annotations,
                                     guint64 *out_timestamp,
                                     char **out_subject,
                                     char **out_body,
                                     char **out_commit,
                                     char **out_parent_commit,
                                     GVariantBuilder *metadata_builder)
{
  const char *oci_timestamp, *oci_subject, *oci_body, *oci_parent_commit, *oci_commit;
  GHashTableIter iter;
  gpointer _key, _value;

  oci_commit = g_hash_table_lookup (annotations, "io.github.ostreedev.Commit");
  if (oci_commit != NULL && out_commit != NULL && *out_commit == NULL)
    *out_commit = g_strdup (oci_commit);

  oci_parent_commit = g_hash_table_lookup (annotations, "io.github.ostreedev.ParentCommit");
  if (oci_parent_commit != NULL && out_parent_commit != NULL && *out_parent_commit == NULL)
    *out_parent_commit = g_strdup (oci_parent_commit);

  oci_timestamp = g_hash_table_lookup (annotations, "io.github.ostreedev.Timestamp");
  if (oci_timestamp != NULL && out_timestamp != NULL && *out_timestamp == 0)
    *out_timestamp = g_ascii_strtoull (oci_timestamp, NULL, 10);

  oci_subject = g_hash_table_lookup (annotations, "io.github.ostreedev.Subject");
  if (oci_subject != NULL && out_subject != NULL && *out_subject == NULL)
    *out_subject = g_strdup (oci_subject);

  oci_body = g_hash_table_lookup (annotations, "io.github.ostreedev.Body");
  if (oci_body != NULL && out_body != NULL && *out_body == NULL)
    *out_body = g_strdup (oci_body);

  if (metadata_builder)
    {
      g_hash_table_iter_init (&iter, annotations);
      while (g_hash_table_iter_next (&iter, &_key, &_value))
        {
          const char *key = _key;
          const char *value = _value;
          guchar *bin;
          gsize bin_len;
          g_autoptr(GVariant) data = NULL;

          if (!g_str_has_prefix (key, "io.github.ostreedev.Metadata."))
            continue;
          key += strlen ("io.github.ostreedev.Metadata.");

          bin = g_base64_decode (value, &bin_len);
          data = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE("v"), bin, bin_len, FALSE,
                                                              g_free, bin));
          g_variant_builder_add (metadata_builder, "{s@v}", key, data);
        }
    }
}
