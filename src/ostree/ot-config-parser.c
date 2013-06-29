/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ot-config-parser.h"
#include "libgsystem.h"

struct _OtConfigParser
{
  GObject       parent_instance;

  gboolean      parsed;
  char         *separators;

  GHashTable   *options;
  GPtrArray    *lines;
};

typedef GObjectClass OtConfigParserClass;

G_DEFINE_TYPE (OtConfigParser, ot_config_parser, G_TYPE_OBJECT)

gboolean
ot_config_parser_parse (OtConfigParser  *self,
                        GFile           *path,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  gs_free char *contents = NULL;
  char **lines = NULL;
  char **iter = NULL;

  g_return_val_if_fail (!self->parsed, FALSE);

  contents = gs_file_load_contents_utf8 (path, cancellable, error);
  if (!contents)
    goto out;

  lines = g_strsplit (contents, "\n", -1);
  for (iter = lines; *iter; iter++)
    {
      const char *line = *iter;
      char *keyname = "";
      
      if (g_ascii_isalpha (*line))
        {
          char **items = NULL;
          items = g_strsplit_set (line, self->separators, 2);
          if (g_strv_length (items) == 2 && items[0][0] != '\0')
            {
              keyname = items[0];
              g_hash_table_insert (self->options, items[0], items[1]);
              g_free (items); /* Transfer ownership */
            }
          else
            {
              g_strfreev (items);
            }
        }
      g_ptr_array_add (self->lines, g_variant_new ("(ss)", keyname, line));
    }

  self->parsed = TRUE;
  
  ret = TRUE;
 out:
  g_strfreev (lines);
  return ret;
}

void
ot_config_parser_set (OtConfigParser  *self,
                      const char      *key,
                      const char      *value)
{
  g_hash_table_replace (self->options, g_strdup (key), g_strdup (value));
}

const char *
ot_config_parser_get (OtConfigParser  *self,
                      const char      *key)
{
  return g_hash_table_lookup (self->options, key);
}

static gboolean
write_key (OtConfigParser         *self,
           GDataOutputStream      *out,
           const char             *key,
           const char             *value,
           GCancellable           *cancellable,
           GError                **error)
{
  gboolean ret = FALSE;

  if (!g_data_output_stream_put_string (out, key, cancellable, error))
    goto out;
  if (!g_data_output_stream_put_byte (out, self->separators[0], cancellable, error))
    goto out;
  if (!g_data_output_stream_put_string (out, value, cancellable, error))
    goto out;
  if (!g_data_output_stream_put_byte (out, '\n', cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
           
gboolean
ot_config_parser_write (OtConfigParser   *self,
                        GFile            *output,
                        GCancellable     *cancellable,
                        GError          **error)
{
  gboolean ret = FALSE;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;
  gs_unref_object GOutputStream *out = NULL;
  gs_unref_object GDataOutputStream *dataout = NULL;
  guint i;
  gs_unref_hashtable GHashTable *written_overrides = NULL;

  written_overrides = g_hash_table_new (g_str_hash, g_str_equal);

  out = (GOutputStream*)g_file_replace (output, NULL, FALSE, 0, cancellable, error);
  if (!out)
    goto out;

  dataout = g_data_output_stream_new (out);

  for (i = 0; i < self->lines->len; i++)
    {
      GVariant *linedata = self->lines->pdata[i];
      const char *key;
      const char *value;
      const char *line;

      g_variant_get (linedata, "(&s&s)", &key, &line);

      value = g_hash_table_lookup (self->options, key);
      if (value == NULL)
        {
          if (!g_data_output_stream_put_string (dataout, line, cancellable, error))
            goto out;
          if (!g_data_output_stream_put_byte (dataout, '\n', cancellable, error))
            goto out;
        }
      else
        {
          if (!write_key (self, dataout, key, value, cancellable, error))
            goto out;
          g_hash_table_insert (written_overrides, (gpointer)key, (gpointer)key);
        }
    }

  g_hash_table_iter_init (&hashiter, self->options);
  while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
    {
      if (g_hash_table_lookup (written_overrides, hashkey))
        continue;
      if (!write_key (self, dataout, hashkey, hashvalue, cancellable, error))
        goto out;
    }

  if (!g_output_stream_close ((GOutputStream*)dataout, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static void
ot_config_parser_finalize (GObject *object)
{
  OtConfigParser *self = OT_CONFIG_PARSER (object);

  g_hash_table_unref (self->options);
  g_ptr_array_unref (self->lines);
  g_free (self->separators);

  G_OBJECT_CLASS (ot_config_parser_parent_class)->finalize (object);
}

static void
ot_config_parser_init (OtConfigParser *self)
{
  self->options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->lines = g_ptr_array_new ();
}

void
ot_config_parser_class_init (OtConfigParserClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ot_config_parser_finalize;
}

OtConfigParser *
ot_config_parser_new (const char *separators)
{
  OtConfigParser *self = NULL;

  g_return_val_if_fail (separators != NULL && separators[0], NULL);

  self = g_object_new (OT_TYPE_CONFIG_PARSER, NULL);
  self->separators = g_strdup (separators);
  return self;
}
