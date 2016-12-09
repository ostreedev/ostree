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

#include <ostree-json.h>
#include <json-glib/json-glib.h>

/* These were added in 1.1.2, add fallbacks if they are not there */

#if !JSON_CHECK_VERSION(1,1,2)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonArray, json_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonBuilder, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonGenerator, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonNode, json_node_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonObject, json_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonPath, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonReader, g_object_unref)
#endif

G_BEGIN_DECLS

typedef enum {
  OSTREE_JSON_PROP_TYPE_PARENT,
  OSTREE_JSON_PROP_TYPE_INT64,
  OSTREE_JSON_PROP_TYPE_BOOL,
  OSTREE_JSON_PROP_TYPE_STRING,
  OSTREE_JSON_PROP_TYPE_STRUCT,
  OSTREE_JSON_PROP_TYPE_STRUCTV,
  OSTREE_JSON_PROP_TYPE_STRV,
  OSTREE_JSON_PROP_TYPE_STRMAP,
  OSTREE_JSON_PROP_TYPE_BOOLMAP,
} OstreeJsonPropType;

struct _OstreeJsonProp {
  const char *name;
  gsize offset;
  OstreeJsonPropType type;
  gpointer type_data;
  gpointer type_data2;
} ;

#define OSTREE_JSON_STRING_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_STRING }
#define OSTREE_JSON_INT64_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_INT64 }
#define OSTREE_JSON_BOOL_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_BOOL }
#define OSTREE_JSON_STRV_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_STRV }
#define OSTREE_JSON_STRMAP_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_STRMAP }
#define OSTREE_JSON_BOOLMAP_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_BOOLMAP }
#define OSTREE_JSON_STRUCT_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_STRUCT, (gpointer)_props}
#define OSTREE_JSON_PARENT_PROP(_struct, _field, _props) \
  { "parent", G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_PARENT, (gpointer)_props}
#define OSTREE_JSON_STRUCTV_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), OSTREE_JSON_PROP_TYPE_STRUCTV, (gpointer)_props, (gpointer) sizeof (**((_struct *) 0)->_field) }
#define OSTREE_JSON_LAST_PROP { NULL }

OstreeJson *ostree_json_from_node (JsonNode       *node,
                                   GType           type,
                                   GError        **error);

JsonNode   *ostree_json_to_node   (OstreeJson  *self);

G_END_DECLS
