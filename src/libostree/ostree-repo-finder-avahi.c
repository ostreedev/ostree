/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2016 Kinvolk GmbH
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Krzesimir Nowak <krzesimir@kinvolk.io>
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/defs.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-glib/glib-malloc.h>
#include <avahi-glib/glib-watch.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libsoup/soup.h>
#include <netinet/in.h>
#include <string.h>

#include "ostree-autocleanups.h"
#include "ostree-bloom-private.h"
#include "ostree-remote-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-avahi.h"
#include "ostree-repo-finder-avahi-private.h"

/* FIXME: Need to partition the results by keyring as well as URI. */

/* TODO: Section documentation
 * Mention endianness */

/* TODO: Submit these upstream */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AvahiClient, avahi_client_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AvahiServiceBrowser, avahi_service_browser_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AvahiServiceResolver, avahi_service_resolver_free)

/* FIXME: Register this with IANA? https://tools.ietf.org/html/rfc6335#section-5.2 */
const gchar * const OSTREE_AVAHI_SERVICE_TYPE = "_ostree_repo._tcp";

static const gchar *
ostree_avahi_client_state_to_string (AvahiClientState state)
{
  switch (state)
    {
    case AVAHI_CLIENT_S_REGISTERING:
      return "registering";
    case AVAHI_CLIENT_S_RUNNING:
      return "running";
    case AVAHI_CLIENT_S_COLLISION:
      return "collision";
    case AVAHI_CLIENT_CONNECTING:
      return "connecting";
    case AVAHI_CLIENT_FAILURE:
      return "failure";
    default:
      return "unknown";
    }
}

static const gchar *
ostree_avahi_resolver_event_to_string (AvahiResolverEvent event)
{
  switch (event)
    {
    case AVAHI_RESOLVER_FOUND:
      return "found";
    case AVAHI_RESOLVER_FAILURE:
      return "failure";
    default:
      return "unknown";
    }
}

static const gchar *
ostree_avahi_browser_event_to_string (AvahiBrowserEvent event)
{
  switch (event)
    {
    case AVAHI_BROWSER_NEW:
      return "new";
    case AVAHI_BROWSER_REMOVE:
      return "remove";
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      return "cache-exhausted";
    case AVAHI_BROWSER_ALL_FOR_NOW:
      return "all-for-now";
    case AVAHI_BROWSER_FAILURE:
      return "failure";
    default:
      return "unknown";
    }
}

/* TODO: Docs */
typedef struct
{
  gchar *name;
  gchar *domain;
  gchar *address;
  guint16 port;
  AvahiStringList *txt;
} OstreeAvahiService;

static void
ostree_avahi_service_free (OstreeAvahiService *service)
{
  g_free (service->name);
  g_free (service->domain);
  g_free (service->address);
  avahi_string_list_free (service->txt);
  g_free (service);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeAvahiService, ostree_avahi_service_free)

/* Convert an AvahiAddress to a string which is suitable for use in URIs (for
 * example). Take into account the scope ID, if the address is IPv6 and a
 * link-local address.
 * (See https://en.wikipedia.org/wiki/IPv6_address#Link-local_addresses_and_zone_indices and
 * https://github.com/lathiat/avahi/issues/110.) */
static gchar *
address_to_string (const AvahiAddress *address,
                   AvahiIfIndex        interface)
{
  char address_string[AVAHI_ADDRESS_STR_MAX];

  avahi_address_snprint (address_string, sizeof (address_string), address);

  switch (address->proto)
    {
    case AVAHI_PROTO_INET6:
      if (IN6_IS_ADDR_LINKLOCAL (address->data.data) ||
          IN6_IS_ADDR_LOOPBACK (address->data.data))
        return g_strdup_printf ("%s%%%d", address_string, interface);
      /* else fall through */
    case AVAHI_PROTO_INET:
    case AVAHI_PROTO_UNSPEC:
    default:
      return g_strdup (address_string);
    }
}

static OstreeAvahiService *
ostree_avahi_service_new (const gchar        *name,
                          const gchar        *domain,
                          const AvahiAddress *address,
                          AvahiIfIndex        interface,
                          guint16             port,
                          AvahiStringList    *txt)
{
  g_autoptr(OstreeAvahiService) service = NULL;

  g_return_val_if_fail (name != NULL, NULL);
  g_return_val_if_fail (domain != NULL, NULL);
  g_return_val_if_fail (address != NULL, NULL);
  g_return_val_if_fail (port > 0, NULL);

  service = g_new0 (OstreeAvahiService, 1);

  service->name = g_strdup (name);
  service->domain = g_strdup (domain);
  service->address = address_to_string (address, interface);
  service->port = port;
  service->txt = avahi_string_list_copy (txt);

  return g_steal_pointer (&service);
}

/* Reference: RFC 6763, §6. */
static gboolean
parse_txt_record (const guint8  *txt,
                  gsize          txt_len,
                  const gchar  **key,
                  gsize         *key_len,
                  const guint8 **value,
                  gsize         *value_len)
{
  gsize i;

  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (key_len != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);
  g_return_val_if_fail (value_len != NULL, FALSE);

  /* RFC 6763, §6.1. */
  if (txt_len > 8900)
    return FALSE;

  *key = (const gchar *) txt;
  *key_len = 0;
  *value = NULL;
  *value_len = 0;

  for (i = 0; i < txt_len; i++)
    {
      if (txt[i] >= 0x20 && txt[i] <= 0x7e && txt[i] != '=')
        {
          /* Key character. */
          *key_len = *key_len + 1;
          continue;
        }
      else if (*key_len > 0 && txt[i] == '=')
        {
          /* Separator. */
          *value = txt + (i + 1);
          *value_len = txt_len - (i + 1);
          return TRUE;
        }
      else
        {
          return FALSE;
        }
    }

  /* The entire TXT record is the key; there is no ‘=’ or value. */
  *value = NULL;
  *value_len = 0;

  return (*key_len > 0);
}

/* TODO: Docs. Return value is only valid as long as @txt is. Reference: RFC 6763, §6. */
GHashTable *
_ostree_txt_records_parse (AvahiStringList *txt)
{
  AvahiStringList *l;
  g_autoptr(GHashTable) out = NULL;

  out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);

  for (l = txt; l != NULL; l = avahi_string_list_get_next (l))
    {
      const guint8 *txt;
      gsize txt_len;
      const gchar *key;
      const guint8 *value;
      gsize key_len, value_len;
      g_autofree gchar *key_allocated = NULL;
      g_autoptr(GBytes) value_allocated = NULL;

      txt = avahi_string_list_get_text (l);
      txt_len = avahi_string_list_get_size (l);

      if (!parse_txt_record (txt, txt_len, &key, &key_len, &value, &value_len))
        {
          g_debug ("Ignoring invalid TXT record of length %" G_GSIZE_FORMAT,
                   txt_len);
          continue;
        }

      key_allocated = g_ascii_strdown (key, key_len);

      if (g_hash_table_lookup_extended (out, key_allocated, NULL, NULL))
        {
          g_debug ("Ignoring duplicate TXT record ‘%s’", key_allocated);
          continue;
        }

      /* Distinguish between the case where the entire record is the key
       * (value == NULL) and the case where the record is the key + ‘=’ and the
       * value is empty (value != NULL && value_len == 0). */
      if (value != NULL)
        value_allocated = g_bytes_new_static (value, value_len);

      g_hash_table_insert (out, g_steal_pointer (&key_allocated), g_steal_pointer (&value_allocated));
    }

  return g_steal_pointer (&out);
}

/* TODO: Maybe make it a valid key check? */
static gboolean
str_is_lowercase (const gchar *str)
{
  gsize i;

  for (i = 0; str[i] != '\0'; i++)
    {
      if (!g_ascii_islower (str[i]))
        return FALSE;
    }

  return TRUE;
}

/* TODO: docs */
GVariant *
_ostree_txt_records_lookup_variant (GHashTable         *attributes,
                                    const gchar        *key,
                                    const GVariantType *value_type)
{
  GBytes *value;
  g_autoptr(GVariant) variant = NULL;

  g_return_val_if_fail (attributes != NULL, NULL);
  g_return_val_if_fail (str_is_lowercase (key), NULL);
  g_return_val_if_fail (value_type != NULL, NULL);

  value = g_hash_table_lookup (attributes, key);

  if (value == NULL)
    {
      g_debug ("TXT attribute ‘%s’ not found.", key);
      return NULL;
    }

  variant = g_variant_new_from_bytes (value_type, value, FALSE);

  if (!g_variant_is_normal_form (variant))
    {
      g_debug ("TXT attribute ‘%s’ value is not in normal form. Ignoring.", key);
      return NULL;
    }

  return g_steal_pointer (&variant);
}

/* TODO: docs */
static GPtrArray *
bloom_refs_intersection (GVariant            *bloom_encoded,
                         const gchar * const *refs)
{
  g_autoptr(OstreeBloom) bloom = NULL;
  g_autoptr(GVariant) bloom_variant = NULL;
  guint8 k, hash_id;
  OstreeBloomHashFunc hash_func;
  const guint8 *bloom_bytes;
  gsize n_bloom_bytes;
  g_autoptr(GBytes) bytes = NULL;
  gsize i;
  g_autoptr(GPtrArray) possible_refs = NULL;

  g_variant_get (bloom_encoded, "(yy@ay)", &k, &hash_id, &bloom_variant);

  if (k == 0)
    return NULL;

  switch (hash_id)
    {
    case 1:
      hash_func = ostree_str_bloom_hash;
      break;
    default:
      return NULL;
    }

  bloom_bytes = g_variant_get_fixed_array (bloom_variant, &n_bloom_bytes, sizeof (guint8));
  bytes = g_bytes_new_static (bloom_bytes, n_bloom_bytes);
  bloom = ostree_bloom_new_from_bytes (bytes, k, hash_func);

  possible_refs = g_ptr_array_new_with_free_func (NULL);

  for (i = 0; refs[i] != NULL; i++)
    {
      const gchar *ref = refs[i];

      if (ostree_bloom_maybe_contains (bloom, ref))
        g_ptr_array_add (possible_refs, (gpointer) ref);
    }

  return g_steal_pointer (&possible_refs);
}

static GHashTable *
ptr_array_to_hash_table_keys (GPtrArray *array)
{
  g_autoptr(GHashTable) table = NULL;
  gsize i;

  table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (i = 0; i < array->len && array->pdata[i] != NULL; i++)
    g_hash_table_insert (table, g_strdup (g_ptr_array_index (array, i)), NULL);

  return g_steal_pointer (&table);
}

static void
fill_refs_and_checksums_from_summary (GVariant   *summary,
                                      GHashTable *refs_and_checksums)
{
  g_autoptr(GVariantIter) iter = NULL;
  gchar *ref_name_tmp;
  GVariant *checksum_variant_tmp;

  g_variant_get (summary, OSTREE_SUMMARY_GVARIANT_STRING, &iter, NULL);
  while (g_variant_iter_next (iter, "(s(t@aya{sv}))",
                              &ref_name_tmp, NULL, &checksum_variant_tmp, NULL))
    {
      g_autofree gchar *ref_name = ref_name_tmp;
      g_autoptr(GVariant) checksum_variant = checksum_variant_tmp;

      if (g_hash_table_contains (refs_and_checksums, ref_name))
        {
          g_autofree gchar *checksum_string = ostree_checksum_from_bytes_v (checksum_variant);

          if (checksum_string)
            {
              g_hash_table_replace (refs_and_checksums,
                                    g_steal_pointer (&ref_name),
                                    g_steal_pointer (&checksum_string));
            }
          else
            {
              g_debug ("invalid checksum for %s, ignoring the ref", ref_name);
              g_hash_table_remove (refs_and_checksums,
                                   ref_name);
            }
        }
    }
}

static void
filter_out_refs_with_no_checksum (GHashTable *refs_and_checksums)
{
  GHashTableIter iter;
  const gchar *ref_name;
  const gchar *checksum;

  g_hash_table_iter_init (&iter, refs_and_checksums);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *)&ref_name,
                                 (gpointer *)&checksum))
    {
      if (checksum == NULL)
        {
          g_debug ("ref %s did not exist in summary, ignoring it", ref_name);
          g_hash_table_iter_remove (&iter);
        }
    }
}

static GHashTable * /* (element-type utf8 utf8) */
get_refs_and_checksums_from_summary (GBytes    *summary_bytes,
                                     GPtrArray *possible_refs /* (element-type utf8) */)
{
  g_autoptr(GVariant) summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, summary_bytes, FALSE));
  g_autoptr(GHashTable) refs_and_checksums = ptr_array_to_hash_table_keys (possible_refs);

  fill_refs_and_checksums_from_summary (summary, refs_and_checksums);
  filter_out_refs_with_no_checksum (refs_and_checksums);

  if (g_hash_table_size (refs_and_checksums) == 0)
    return NULL;

  return g_steal_pointer (&refs_and_checksums);
}

static gboolean
fetch_summary_from_remote (OstreeRepo    *repo,
                           OstreeRemote  *remote,
                           GBytes       **out_summary_bytes,
                           GCancellable  *cancellable)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;
  gboolean remote_already_existed = _ostree_repo_add_remote (repo, remote);
  gboolean success = ostree_repo_remote_fetch_summary_with_options (repo,
                                                                    remote->name,
                                                                    NULL /* options */,
                                                                    &summary_bytes,
                                                                    NULL /* signature */,
                                                                    cancellable,
                                                                    &local_error);

  if (!remote_already_existed)
    _ostree_repo_remove_remote (repo, remote);

  if (!success)
    {
      g_debug ("failed to fetch summary: %s", local_error->message);
      return FALSE;
    }

  g_assert (out_summary_bytes != NULL);
  *out_summary_bytes = g_steal_pointer (&summary_bytes);
  return TRUE;
}

struct _OstreeRepoFinderAvahi
{
  GObject parent_instance;

  /* All elements of this structure must only be accessed from @avahi_context
   * after construction. */

  /* Note: There is a ref-count loop here: each #GTask has a reference to the
   * #OstreeRepoFinderAvahi, and we have to keep a reference to the #GTask. */
  GPtrArray *resolve_tasks;  /* (element-type (owned) GTask) */

  AvahiGLibPoll *poll;
  AvahiClient *client;
  AvahiServiceBrowser *browser;

  AvahiClientState client_state;
  gboolean browser_failed;
  gboolean browser_all_for_now;

  GCancellable *avahi_cancellable;
  GMainContext *avahi_context;

  /* Map of service name (typically human readable) to a #GPtrArray of the
   * #AvahiServiceResolver instances we have running against that name. We
   * could end up with more than one resolver if the same name is advertised to
   * us over multiple interfaces or protocols (for example, IPv4 and IPv6).
   * Resolve all of them just in case one doesn’t work. */
  GHashTable *resolvers;  /* (element-type (owned) utf8 (owned) GPtrArray (element-type (owned) AvahiServiceResolver)) */

  /* Array of #OstreeAvahiService instances representing all the services which
   * we currently think are valid. */
  GPtrArray *found_services;  /* (element-type (owned OstreeAvahiService) */
};

static GHashTable * /* (element-type utf8 utf8) */
get_checksums (OstreeRepoFinderAvahi *finder,
               OstreeRepo            *repo,
               OstreeRemote          *remote,
               GPtrArray             *possible_refs /* (element-type utf8) */)
{
  g_autoptr(GBytes) summary_bytes = NULL;

  if (!fetch_summary_from_remote (repo,
                                  remote,
                                  &summary_bytes,
                                  finder->avahi_cancellable))
    return NULL;

  return get_refs_and_checksums_from_summary (summary_bytes,
                                              possible_refs);
}

/* TODO: docs

v=1
rb=refs bloom filter
st=summary timestamp
ri=repository index
*/
static OstreeRepoFinderResult *
ostree_avahi_service_build_repo_finder_result (OstreeAvahiService    *self,
                                               OstreeRepoFinderAvahi *finder,
                                               OstreeRepo            *repo,
                                               gint                   priority,
                                               const gchar * const   *refs)
{
  g_autoptr(GHashTable) attributes = NULL;
  g_autoptr(GVariant) version = NULL;
  g_autoptr(GVariant) bloom = NULL;
  g_autoptr(GVariant) summary_timestamp = NULL;
  g_autoptr(GVariant) repo_index = NULL;
  g_autofree gchar *repo_path = NULL;
  g_autoptr(GPtrArray) possible_refs = NULL; /* (element-type utf8) */
  g_autoptr(GHashTable) possible_ref_to_checksum = NULL;  /* (element-type utf8 utf8) */
  SoupURI *_uri = NULL;
  g_autofree gchar *uri = NULL;

  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (refs != NULL, NULL);

  attributes = _ostree_txt_records_parse (self->txt);

  /* Check the record version. */
  version = _ostree_txt_records_lookup_variant (attributes, "v", G_VARIANT_TYPE_BYTE);

  if (g_variant_get_byte (version) != 1)
    {
      g_debug ("Unknown v=%02x attribute provided in TXT record. Ignoring.",
               g_variant_get_byte (version));
      return NULL;
    }

  /* Refs bloom filter? */
  bloom = _ostree_txt_records_lookup_variant (attributes, "rb", G_VARIANT_TYPE ("(yyay)"));

  if (bloom == NULL)
    {
      g_debug ("Missing rb (refs bloom) attribute in TXT record. Ignoring.");
      return NULL;
    }

  possible_refs = bloom_refs_intersection (bloom, refs);
  if (possible_refs == NULL)
    {
      g_debug ("Wrong k parameter or hash id in rb (refs bloom) attribute in TXT record. Ignoring.");
      return NULL;
    }
  if (possible_refs->len == 0)
    {
      g_debug ("TXT record definitely has no matching refs. Ignoring.");
      return NULL;
    }

  /* Summary timestamp. */
  summary_timestamp = _ostree_txt_records_lookup_variant (attributes, "st", G_VARIANT_TYPE_UINT64);
  if (summary_timestamp == NULL)
    {
      g_debug ("Missing st (summary timestamp) attribute in TXT record. Ignoring.");
      return NULL;
    }

  /* Repository index. */
  repo_index = _ostree_txt_records_lookup_variant (attributes, "ri", G_VARIANT_TYPE_UINT16);
  if (repo_index == NULL)
    {
      g_debug ("Missing ri (repository index) attribute in TXT record. Ignoring.");
      return NULL;
    }
  repo_path = g_strdup_printf ("/%u", GUINT16_FROM_BE (g_variant_get_uint16 (repo_index)));

  /* Build the URI for the repository. */
  _uri = soup_uri_new (NULL);
  soup_uri_set_scheme (_uri, "http");
  soup_uri_set_host (_uri, self->address);
  soup_uri_set_port (_uri, self->port);
  soup_uri_set_path (_uri, repo_path);
  uri = soup_uri_to_string (_uri, FALSE);
  soup_uri_free (_uri);

  /* Build the URI into a remote. */
  g_autoptr(OstreeRemote) remote = NULL;

  /* Build an #OstreeRemote. Use the hash of the URI, since remote->name
   * is used in file paths, so needs to not contain special characters. */
  g_autofree gchar *name = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
  remote = ostree_remote_new (name);

  g_key_file_set_string (remote->options, remote->group, "url", uri);
  g_key_file_set_boolean (remote->options, remote->group, "gpg-verify", TRUE);
  g_key_file_set_boolean (remote->options, remote->group, "gpg-verify-summary", TRUE);

  possible_ref_to_checksum = get_checksums (finder, repo, remote, possible_refs);
  if (possible_ref_to_checksum == NULL)
    {
      g_debug ("Failed to get checksums for possible refs. Ignoring.");
      return NULL;
    }

  return ostree_repo_finder_result_new (remote, OSTREE_REPO_FINDER (finder), priority,
                                        possible_ref_to_checksum,
                                        (summary_timestamp != NULL) ? GUINT64_FROM_BE (g_variant_get_uint64 (summary_timestamp)) : 0);
}

static void ostree_repo_finder_avahi_iface_init (OstreeRepoFinderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderAvahi, ostree_repo_finder_avahi, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_avahi_iface_init))

typedef struct
{
  gchar **refs;  /* (owned) */
  OstreeRepo *parent_repo;  /* (owned) */
} ResolveData;

static void
resolve_data_free (ResolveData *data)
{
  g_object_unref (data->parent_repo);
  g_strfreev (data->refs);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ResolveData, resolve_data_free)

static ResolveData *
resolve_data_new (const gchar * const *refs,
                  OstreeRepo          *parent_repo)
{
  g_autoptr(ResolveData) data = NULL;

  data = g_new0 (ResolveData, 1);
  data->refs = g_strdupv ((gchar **) refs);
  data->parent_repo = g_object_ref (parent_repo);

  return g_steal_pointer (&data);
}

static void
fail_all_pending_tasks (OstreeRepoFinderAvahi *self,
                        GQuark                 domain,
                        gint                   code,
                        const gchar           *format,
                        ...) G_GNUC_PRINTF(4, 5);

/* TODO: Docs */
/* Executed in @self->avahi_context. */
static void
fail_all_pending_tasks (OstreeRepoFinderAvahi *self,
                        GQuark                 domain,
                        gint                   code,
                        const gchar           *format,
                        ...)
{
  gsize i;
  va_list args;
  g_autoptr(GError) error = NULL;

  g_assert (g_main_context_is_owner (self->avahi_context));

  va_start (args, format);
  error = g_error_new_valist (domain, code, format, args);
  va_end (args);

  for (i = 0; i < self->resolve_tasks->len; i++)
    {
      GTask *task = G_TASK (g_ptr_array_index (self->resolve_tasks, i));
      g_task_return_error (task, g_error_copy (error));
    }

  g_ptr_array_set_size (self->resolve_tasks, 0);
}

/* TODO: docs */
/* Executed in @self->avahi_context. */
static void
complete_all_pending_tasks (OstreeRepoFinderAvahi *self)
{
  gsize i;
  const gint priority = 60;  /* arbitrarily chosen */
  g_autoptr(GPtrArray) results_for_tasks = g_ptr_array_new_full (self->resolve_tasks->len, (GDestroyNotify)g_ptr_array_unref);
  gboolean cancelled = FALSE;

  g_assert (g_main_context_is_owner (self->avahi_context));
  g_debug ("%s: Completing %u tasks", G_STRFUNC, self->resolve_tasks->len);

  for (i = 0; i < self->resolve_tasks->len; i++)
    {
      g_autoptr(GPtrArray) results = NULL;
      GTask *task;
      ResolveData *data;
      const gchar * const *refs;
      gsize j;

      task = G_TASK (g_ptr_array_index (self->resolve_tasks, i));
      data = g_task_get_task_data (task);
      refs = (const gchar * const *) data->refs;
      results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);

      for (j = 0; j < self->found_services->len; j++)
        {
          OstreeAvahiService *service = g_ptr_array_index (self->found_services, j);
          g_autoptr(OstreeRepoFinderResult) result = NULL;

          result = ostree_avahi_service_build_repo_finder_result (service, self, data->parent_repo, priority, refs);
          if (g_cancellable_is_cancelled (self->avahi_cancellable))
            {
              cancelled = TRUE;
              break;
            }
          if (result != NULL)
            g_ptr_array_add (results, g_steal_pointer (&result));
        }
      if (cancelled)
        break;

      g_ptr_array_add (results_for_tasks, g_steal_pointer (&results));
    }

  if (!cancelled)
    {
      for (i = 0; i < self->resolve_tasks->len; i++)
        {
          GTask *task = G_TASK (g_ptr_array_index (self->resolve_tasks, i));
          GPtrArray *results = g_ptr_array_index (results_for_tasks, i);

          g_task_return_pointer (task,
                                 g_ptr_array_ref (results),
                                 (GDestroyNotify) g_ptr_array_unref);
        }

      g_ptr_array_set_size (self->resolve_tasks, 0);
    }
  else
    {
      fail_all_pending_tasks (self, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Avahi service resolution cancelled.");
    }
}

/* Executed in @self->avahi_context. */
static void
maybe_complete_all_pending_tasks (OstreeRepoFinderAvahi *self)
{
  g_assert (g_main_context_is_owner (self->avahi_context));
  g_debug ("%s: client_state: %s, browser_failed: %u, cancelled: %u, "
           "browser_all_for_now: %u, n_resolvers: %u",
           G_STRFUNC, ostree_avahi_client_state_to_string (self->client_state),
           self->browser_failed,
           g_cancellable_is_cancelled (self->avahi_cancellable),
           self->browser_all_for_now, g_hash_table_size (self->resolvers));

  if (self->client_state == AVAHI_CLIENT_FAILURE)
    fail_all_pending_tasks (self, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Avahi client error: %s",
                            avahi_strerror (avahi_client_errno (self->client)));
  else if (self->browser_failed)
    fail_all_pending_tasks (self, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Avahi browser error: %s",
                            avahi_strerror (avahi_client_errno (self->client)));
  else if (g_cancellable_is_cancelled (self->avahi_cancellable))
    fail_all_pending_tasks (self, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Avahi service resolution cancelled.");
  else if (self->browser_all_for_now &&
           g_hash_table_size (self->resolvers) == 0)
    complete_all_pending_tasks (self);
}

/* Executed in @self->avahi_context. */
static void
client_cb (AvahiClient      *client,
           AvahiClientState  state,
           void             *finder_ptr)
{
  /* Completing the pending tasks might drop the final reference to @self. */
  g_autoptr(OstreeRepoFinderAvahi) self = g_object_ref (finder_ptr);

  /* self->client will be NULL if client_cb() is called from
   * ostree_repo_finder_avahi_start(). */
  g_assert (self->client == NULL || g_main_context_is_owner (self->avahi_context));

  g_debug ("%s: Entered state ‘%s’.",
           G_STRFUNC, ostree_avahi_client_state_to_string (state));

  /* We only care about entering and leaving %AVAHI_CLIENT_FAILURE. */
  self->client_state = state;
  if (self->client != NULL)
    maybe_complete_all_pending_tasks (self);
}

/* Executed in @self->avahi_context. */
static void
resolve_cb (AvahiServiceResolver   *resolver,
            AvahiIfIndex            interface,
            AvahiProtocol           protocol,
            AvahiResolverEvent      event,
            const char             *name,
            const char             *type,
            const char             *domain,
            const char             *host_name,
            const AvahiAddress     *address,
            uint16_t                port,
            AvahiStringList        *txt,
            AvahiLookupResultFlags  flags,
            void                   *finder_ptr)
{
  /* Completing the pending tasks might drop the final reference to @self. */
  g_autoptr(OstreeRepoFinderAvahi) self = g_object_ref (finder_ptr);
  g_autoptr(OstreeAvahiService) service = NULL;
  GPtrArray *resolvers;

  g_assert (g_main_context_is_owner (self->avahi_context));

  g_debug ("%s: Resolve event ‘%s’ for name ‘%s’.",
           G_STRFUNC, ostree_avahi_resolver_event_to_string (event), name);

  /* Track the resolvers active for this @name. There may be several,
   * as @name might appear to us over several interfaces or protocols. Most
   * commonly this happens when both hosts are connected via IPv4 and IPv6. */
  resolvers = g_hash_table_lookup (self->resolvers, name);

  if (resolvers == NULL || resolvers->len == 0)
    {
      /* maybe it was removed in the meantime */
      g_hash_table_remove (self->resolvers, name);
      return;
    }
  else if (resolvers->len == 1)
    {
      g_hash_table_remove (self->resolvers, name);
    }
  else
    {
      g_ptr_array_remove_fast (resolvers, resolver);
    }

  /* Was resolution successful? */
  switch (event)
    {
    case AVAHI_RESOLVER_FOUND:
      service = ostree_avahi_service_new (name, domain, address, interface,
                                          port, txt);
      g_ptr_array_add (self->found_services, g_steal_pointer (&service));
      break;
    case AVAHI_RESOLVER_FAILURE:
    default:
      g_warning ("Failed to resolve service ‘%s’: %s", name,
                 avahi_strerror (avahi_client_errno (self->client)));
      break;
    }

  maybe_complete_all_pending_tasks (self);
}

/* TODO: overall documentation about the split between client, browser, resolver;
 * and between the background processing and resolve_async() requests. */
/* Executed in @self->avahi_context. */
static void
browse_new (OstreeRepoFinderAvahi *self,
            AvahiIfIndex           interface,
            AvahiProtocol          protocol,
            const gchar           *name,
            const gchar           *type,
            const gchar           *domain)
{
  g_autoptr(AvahiServiceResolver) resolver = NULL;
  GPtrArray *resolvers;  /* (element-type AvahiServiceResolver) */

  g_assert (g_main_context_is_owner (self->avahi_context));

  resolver = avahi_service_resolver_new (self->client,
                                         interface,
                                         protocol,
                                         name,
                                         type,
                                         domain,
                                         AVAHI_PROTO_UNSPEC,
                                         0,
                                         resolve_cb,
                                         self);
  if (resolver == NULL)
    {
      g_warning ("Failed to resolve service ‘%s’: %s", name,
                 avahi_strerror (avahi_client_errno (self->client)));
      return;
    }

  g_debug ("Found name service %s on the network; type: %s, domain: %s, "
           "protocol: %u, interface: %u", name, type, domain, protocol,
           interface);

  /* Start a resolver for this (interface, protocol, name, type, domain)
   * combination. */
  resolvers = g_hash_table_lookup (self->resolvers, name);
  if (resolvers == NULL)
    {
      resolvers = g_ptr_array_new_with_free_func ((GDestroyNotify) avahi_service_resolver_free);
      g_hash_table_insert (self->resolvers, g_strdup (name), resolvers);
    }

  g_ptr_array_add (resolvers, g_steal_pointer (&resolver));
}

/* Executed in @self->avahi_context. Caller must call maybe_complete_all_pending_tasks(). */
static void
browse_remove (OstreeRepoFinderAvahi *self,
               const char            *name)
{
  gsize i;
  gboolean removed = FALSE;

  g_assert (g_main_context_is_owner (self->avahi_context));

  g_hash_table_remove (self->resolvers, name);

  for (i = 0; i < self->found_services->len; i += (removed ? 0 : 1))
    {
      OstreeAvahiService *service = g_ptr_array_index (self->found_services, i);

      removed = FALSE;

      if (g_strcmp0 (service->name, name) == 0)
        {
          g_ptr_array_remove_index_fast (self->found_services, i);
          removed = TRUE;
          continue;
        }
    }
}

/* Executed in @self->avahi_context. */
static void
browse_cb (AvahiServiceBrowser    *browser,
           AvahiIfIndex            interface,
           AvahiProtocol           protocol,
           AvahiBrowserEvent       event,
           const char             *name,
           const char             *type,
           const char             *domain,
           AvahiLookupResultFlags  flags,
           void                   *finder_ptr)
{
  /* Completing the pending tasks might drop the final reference to @self. */
  g_autoptr(OstreeRepoFinderAvahi) self = g_object_ref (finder_ptr);

  g_assert (g_main_context_is_owner (self->avahi_context));

  g_debug ("%s: Browse event ‘%s’ for name ‘%s’.",
           G_STRFUNC, ostree_avahi_browser_event_to_string (event), name);

  self->browser_failed = FALSE;

  switch (event)
    {
    case AVAHI_BROWSER_NEW:
      browse_new (self, interface, protocol, name, type, domain);
      break;

    case AVAHI_BROWSER_REMOVE:
      browse_remove (self, name);
      break;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      /* don’t care about this. */
      break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
      self->browser_all_for_now = TRUE;
      break;

    case AVAHI_BROWSER_FAILURE:
      self->browser_failed = TRUE;
      break;

    default:
      g_assert_not_reached ();
    }

  /* Check all the tasks for any event, since the @browser_failed state
   * may have changed. */
  maybe_complete_all_pending_tasks (self);
}

static gboolean add_resolve_task_cb (gpointer user_data);

/* TODO: Docs, make sure it’s testable */
static void
ostree_repo_finder_avahi_resolve_async (OstreeRepoFinder    *finder,
                                        const gchar * const *refs,
                                        OstreeRepo          *parent_repo,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  OstreeRepoFinderAvahi *self = OSTREE_REPO_FINDER_AVAHI (finder);
  g_autoptr(GTask) task = NULL;

  g_debug ("%s: Starting resolving", G_STRFUNC);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_avahi_resolve_async);
  g_task_set_task_data (task, resolve_data_new (refs, parent_repo), (GDestroyNotify) resolve_data_free);

  /* Move @task to the @avahi_context where it can be processed. */
  g_main_context_invoke (self->avahi_context, add_resolve_task_cb, g_steal_pointer (&task));
}

/* Executed in @self->avahi_context. */
static gboolean
add_resolve_task_cb (gpointer user_data)
{
  g_autoptr(GTask) task = G_TASK (user_data);
  OstreeRepoFinderAvahi *self = g_task_get_source_object (task);

  g_assert (g_main_context_is_owner (self->avahi_context));
  g_debug ("%s", G_STRFUNC);

  /* Track the task and check to see if the browser and resolvers are in a
   * quiescent state suitable for returning a result immediately. */
  g_ptr_array_add (self->resolve_tasks, g_object_ref (task));
  maybe_complete_all_pending_tasks (self);

  return G_SOURCE_REMOVE;
}

static GPtrArray *
ostree_repo_finder_avahi_resolve_finish (OstreeRepoFinder  *finder,
                                         GAsyncResult      *result,
                                         GError           **error)
{
  g_return_val_if_fail (g_task_is_valid (result, finder), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_repo_finder_avahi_dispose (GObject *obj)
{
  OstreeRepoFinderAvahi *self = OSTREE_REPO_FINDER_AVAHI (obj);

  ostree_repo_finder_avahi_stop (self);

  g_assert (self->resolve_tasks == NULL || self->resolve_tasks->len == 0);

  g_clear_pointer (&self->resolve_tasks, g_ptr_array_unref);
  g_clear_pointer (&self->browser, avahi_service_browser_free);
  g_clear_pointer (&self->client, avahi_client_free);
  g_clear_pointer (&self->poll, avahi_glib_poll_free);
  g_clear_pointer (&self->avahi_context, g_main_context_unref);
  g_clear_pointer (&self->found_services, g_ptr_array_unref);
  g_clear_pointer (&self->resolvers, g_hash_table_unref);
  g_clear_object (&self->avahi_cancellable);

  /* Chain up. */
  G_OBJECT_CLASS (ostree_repo_finder_avahi_parent_class)->dispose (obj);
}

static void
ostree_repo_finder_avahi_class_init (OstreeRepoFinderAvahiClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ostree_repo_finder_avahi_dispose;
}

static void
ostree_repo_finder_avahi_iface_init (OstreeRepoFinderInterface *iface)
{
  iface->resolve_async = ostree_repo_finder_avahi_resolve_async;
  iface->resolve_finish = ostree_repo_finder_avahi_resolve_finish;
}

static void
ostree_repo_finder_avahi_init (OstreeRepoFinderAvahi *self)
{
  self->resolve_tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  self->avahi_cancellable = g_cancellable_new ();
  self->client_state = AVAHI_CLIENT_S_REGISTERING;
  self->resolvers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  self->found_services = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_avahi_service_free);
}

/**
 * ostree_repo-finder_avahi_new:
 * @context: (transfer none) (nullable): a #GMainContext for processing Avahi
 *    events in, or %NULL to use the current thread-default
 *
 * TODO
 *
 * The calling code is responsible for ensuring that @context is iterated while
 * the #OstreeRepoFinderAvahi is running (after ostree_repo_finder_avahi_start()
 * is called). This may be done from any thread.
 *
 * If @context is %NULL, the current thread-default #GMainContext is used.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderAvahi
 * Since: 2017.7
 */
OstreeRepoFinderAvahi *
ostree_repo_finder_avahi_new (GMainContext *context)
{
  g_autoptr(OstreeRepoFinderAvahi) finder = NULL;

  finder = g_object_new (OSTREE_TYPE_REPO_FINDER_AVAHI, NULL);

  /* TODO: Make this a property */
  if (context != NULL)
    finder->avahi_context = g_main_context_ref (context);
  else
    finder->avahi_context = g_main_context_ref_thread_default ();

  /* Avahi setup. Note: Technically the allocator is per-process state which we
   * shouldn’t set here, but it’s probably fine. It’s unlikely that code which
   * is using libostree is going to use an allocator which is not GLib, and
   * *also* use Avahi API itself. */
  avahi_set_allocator (avahi_glib_allocator ());
  finder->poll = avahi_glib_poll_new (finder->avahi_context, G_PRIORITY_DEFAULT);

  return g_steal_pointer (&finder);
}

/* TODO: Docs */
void
ostree_repo_finder_avahi_start (OstreeRepoFinderAvahi  *self,
                                GError                **error)
{
  g_autoptr(AvahiClient) client = NULL;
  g_autoptr(AvahiServiceBrowser) browser = NULL;
  int failure = 0;

  g_return_if_fail (OSTREE_IS_REPO_FINDER_AVAHI (self));
  g_return_if_fail (error == NULL || *error == NULL);

  if (g_cancellable_set_error_if_cancelled (self->avahi_cancellable, error))
    return;

  g_assert (self->client == NULL);

  client = avahi_client_new (avahi_glib_poll_get (self->poll),
                             AVAHI_CLIENT_NO_FAIL,
                             client_cb, self, &failure);

  if (client == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create finder client: %s",
                   avahi_strerror (failure));
      return;
    }

  /* Query for the OSTree DNS-SD service on the local network. */
  browser = avahi_service_browser_new (client,
                                       AVAHI_IF_UNSPEC,
                                       AVAHI_PROTO_UNSPEC,
                                       OSTREE_AVAHI_SERVICE_TYPE,
                                       NULL,
                                       0,
                                       browse_cb,
                                       self);

  if (browser == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create service browser: %s",
                   avahi_strerror (avahi_client_errno (client)));
      return;
    }

  /* Success. */
  self->client = g_steal_pointer (&client);
  self->browser = g_steal_pointer (&browser);
}

static gboolean stop_cb (gpointer user_data);

/* TODO: Docs. Can’t be started again after it’s been stopped. Any in-progress queries will be cancelled. */
void
ostree_repo_finder_avahi_stop (OstreeRepoFinderAvahi *self)
{
  g_return_if_fail (OSTREE_IS_REPO_FINDER_AVAHI (self));

  if (self->browser == NULL)
    return;

  g_main_context_invoke (self->avahi_context, stop_cb, g_object_ref (self));
}

static gboolean
stop_cb (gpointer user_data)
{
  g_autoptr(OstreeRepoFinderAvahi) self = OSTREE_REPO_FINDER_AVAHI (user_data);

  g_cancellable_cancel (self->avahi_cancellable);
  maybe_complete_all_pending_tasks (self);

  g_clear_pointer (&self->browser, avahi_service_browser_free);
  g_clear_pointer (&self->client, avahi_client_free);
  g_hash_table_remove_all (self->resolvers);

  return G_SOURCE_REMOVE;
}
