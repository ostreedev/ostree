/*
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

#include "ostree-bootconfig-parser.h"
#include "otutil.h"

struct _OstreeBootconfigParser
{
  GObject       parent_instance;

  gboolean      parsed;
  const char   *separators;

  GHashTable   *options;
};

typedef GObjectClass OstreeBootconfigParserClass;

G_DEFINE_TYPE (OstreeBootconfigParser, ostree_bootconfig_parser, G_TYPE_OBJECT)

/**
 * ostree_bootconfig_parser_clone:
 * @self: Bootconfig to clone
 * 
 * Returns: (transfer full): Copy of @self
 */
OstreeBootconfigParser *
ostree_bootconfig_parser_clone (OstreeBootconfigParser *self)
{
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();

  GLNX_HASH_TABLE_FOREACH_KV (self->options, const char*, k, const char*, v)
    g_hash_table_replace (parser->options, g_strdup (k), g_strdup (v));

  return parser;
}

/**
 * ostree_bootconfig_parser_parse_at:
 * @self: Parser
 * @dfd: Directory fd
 * @path: File path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Initialize a bootconfig from the given file.
 */
gboolean
ostree_bootconfig_parser_parse_at (OstreeBootconfigParser  *self,
                                   int                      dfd,
                                   const char              *path,
                                   GCancellable            *cancellable,
                                   GError                 **error)
{
  g_return_val_if_fail (!self->parsed, FALSE);

  g_autofree char *contents = glnx_file_get_contents_utf8_at (dfd, path, NULL, cancellable, error);
  if (!contents)
    return FALSE;

  g_auto(GStrv) lines = g_strsplit (contents, "\n", -1);
  for (char **iter = lines; *iter; iter++)
    {
      const char *line = *iter;

      if (g_ascii_isalpha (*line))
        {
          char **items = NULL;
          items = g_strsplit_set (line, self->separators, 2);
          if (g_strv_length (items) == 2 && items[0][0] != '\0')
            {
              g_hash_table_insert (self->options, items[0], items[1]);
              g_free (items); /* Transfer ownership */
            }
          else
            {
              g_strfreev (items);
            }
        }
    }

  self->parsed = TRUE;

  return TRUE;
}

gboolean
ostree_bootconfig_parser_parse (OstreeBootconfigParser  *self,
                                GFile           *path,
                                GCancellable    *cancellable,
                                GError         **error)
{
  return ostree_bootconfig_parser_parse_at (self, AT_FDCWD, gs_file_get_path_cached (path),
                                            cancellable, error);
}

void
ostree_bootconfig_parser_set (OstreeBootconfigParser  *self,
                              const char      *key,
                              const char      *value)
{
  g_hash_table_replace (self->options, g_strdup (key), g_strdup (value));
}

const char *
ostree_bootconfig_parser_get (OstreeBootconfigParser  *self,
                              const char      *key)
{
  return g_hash_table_lookup (self->options, key);
}

static void
write_key (OstreeBootconfigParser    *self,
           GString                   *buf,
           const char                *key,
           const char                *value)
{
  g_string_append (buf, key);
  g_string_append_c (buf, self->separators[0]);
  g_string_append (buf, value);
  g_string_append_c (buf, '\n');
}

gboolean
ostree_bootconfig_parser_write_at (OstreeBootconfigParser   *self,
                                   int                       dfd,
                                   const char               *path,
                                   GCancellable             *cancellable,
                                   GError                  **error)
{
  /* Write the fields in a deterministic order, following what is used
   * in the bootconfig example of the BootLoaderspec document:
   * https://systemd.io/BOOT_LOADER_SPECIFICATION
   */
  const char *fields[] = { "title", "version", "options", "devicetree", "linux", "initrd" };
  g_autoptr(GHashTable) keys_written = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GString) buf = g_string_new ("");

  for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      const char *key = fields[i];
      const char *value = g_hash_table_lookup (self->options, key);
      if (value != NULL)
        {
          write_key (self, buf, key, value);
          g_hash_table_add (keys_written, (gpointer)key);
        }
    }

  /* Write unknown fields */
  GLNX_HASH_TABLE_FOREACH_KV (self->options, const char*, k, const char*, v)
    {
      if (g_hash_table_lookup (keys_written, k))
        continue;
      write_key (self, buf, k, v);
    }

  if (!glnx_file_replace_contents_at (dfd, path, (guint8*)buf->str, buf->len,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
ostree_bootconfig_parser_write (OstreeBootconfigParser   *self,
                                GFile            *output,
                                GCancellable     *cancellable,
                                GError          **error)
{
  return ostree_bootconfig_parser_write_at (self,
                                            AT_FDCWD, gs_file_get_path_cached (output),
                                            cancellable, error);
}

static void
ostree_bootconfig_parser_finalize (GObject *object)
{
  OstreeBootconfigParser *self = OSTREE_BOOTCONFIG_PARSER (object);

  g_hash_table_unref (self->options);

  G_OBJECT_CLASS (ostree_bootconfig_parser_parent_class)->finalize (object);
}

static void
ostree_bootconfig_parser_init (OstreeBootconfigParser *self)
{
  self->options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

void
ostree_bootconfig_parser_class_init (OstreeBootconfigParserClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ostree_bootconfig_parser_finalize;
}

OstreeBootconfigParser *
ostree_bootconfig_parser_new (void)
{
  OstreeBootconfigParser *self = NULL;

  self = g_object_new (OSTREE_TYPE_BOOTCONFIG_PARSER, NULL);
  self->separators = " \t";
  return self;
}
