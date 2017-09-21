/*
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

#ifdef HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/defs.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-glib/glib-malloc.h>
#include <avahi-glib/glib-watch.h>
#include <libsoup/soup.h>
#include <netinet/in.h>
#include <string.h>
#endif  /* HAVE_AVAHI */

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-autocleanups.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-avahi.h"

#ifdef HAVE_AVAHI
#include "ostree-bloom-private.h"
#include "ostree-remote-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo.h"
#include "ostree-repo-finder-avahi-private.h"
#include "otutil.h"
#endif  /* HAVE_AVAHI */

/**
 * SECTION:ostree-repo-finder-avahi
 * @title: OstreeRepoFinderAvahi
 * @short_description: Finds remote repositories from ref names by looking at
 *    adverts of refs from peers on the local network
 * @stability: Unstable
 * @include: libostree/ostree-repo-finder-avahi.h
 *
 * #OstreeRepoFinderAvahi is an implementation of #OstreeRepoFinder which looks
 * for refs being hosted by peers on the local network.
 *
 * Any ref which matches by collection ID and ref name is returned as a result,
 * with no limitations on the peers which host them, as long as they are
 * accessible over the local network, and their adverts reach this machine via
 * DNS-SD/mDNS.
 *
 * For each repository which is found, a result will be returned for the
 * intersection of the refs being searched for, and the refs in `refs/mirrors`
 * in the remote repository.
 *
 * DNS-SD resolution is performed using Avahi, which will continue to scan for
 * matching peers throughout the lifetime of the process. It’s recommended that
 * ostree_repo_finder_avahi_start() be called early on in the process’ lifetime,
 * and the #GMainContext which is passed to ostree_repo_finder_avahi_new()
 * continues to be iterated until ostree_repo_finder_avahi_stop() is called.
 *
 * The values stored in DNS-SD TXT records are stored as big-endian whenever
 * endianness is relevant.
 *
 * Internally, #OstreeRepoFinderAvahi has an Avahi client, browser and resolver
 * which work in the background to track all available peers on the local
 * network. Whenever a resolve request is made using
 * ostree_repo_finder_resolve_async(), the request is blocked until the
 * background tracking is in a consistent state (typically this only happens at
 * startup), and is then answered using the current cache of background data.
 * The Avahi client tracks the #OstreeRepoFinderAvahi’s connection with the
 * Avahi D-Bus service. The browser looks for DNS-SD peers on the local network;
 * and the resolver is used to retrieve information about services advertised by
 * each peer, including the services’ TXT records.
 *
 * Since: 2017.8
 */

#ifdef HAVE_AVAHI
/* FIXME: Submit these upstream */
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

typedef struct
{
  gchar *uri;
  OstreeRemote *keyring_remote;  /* (owned) */
} UriAndKeyring;

static void
uri_and_keyring_free (UriAndKeyring *data)
{
  g_free (data->uri);
  ostree_remote_unref (data->keyring_remote);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (UriAndKeyring, uri_and_keyring_free)

static UriAndKeyring *
uri_and_keyring_new (const gchar  *uri,
                     OstreeRemote *keyring_remote)
{
  g_autoptr(UriAndKeyring) data = NULL;

  data = g_new0 (UriAndKeyring, 1);
  data->uri = g_strdup (uri);
  data->keyring_remote = ostree_remote_ref (keyring_remote);

  return g_steal_pointer (&data);
}

static guint
uri_and_keyring_hash (gconstpointer key)
{
  const UriAndKeyring *_key = key;

  return g_str_hash (_key->uri) ^ g_str_hash (_key->keyring_remote->keyring);
}

static gboolean
uri_and_keyring_equal (gconstpointer a,
                       gconstpointer b)
{
  const UriAndKeyring *_a = a, *_b = b;

  return (g_str_equal (_a->uri, _b->uri) &&
          g_str_equal (_a->keyring_remote->keyring, _b->keyring_remote->keyring));
}

/* This must return a valid remote name (suitable for use in a refspec). */
static gchar *
uri_and_keyring_to_name (UriAndKeyring *data)
{
  g_autofree gchar *escaped_uri = g_uri_escape_string (data->uri, NULL, FALSE);
  g_autofree gchar *escaped_keyring = g_uri_escape_string (data->keyring_remote->keyring, NULL, FALSE);

  /* FIXME: Need a better separator than `_`, since it’s not escaped in the input. */
  g_autofree gchar *out = g_strdup_printf ("%s_%s", escaped_uri, escaped_keyring);

  for (gsize i = 0; out[i] != '\0'; i++)
    {
      if (out[i] == '%')
        out[i] = '_';
    }

  g_return_val_if_fail (ostree_validate_remote_name (out, NULL), NULL);

  return g_steal_pointer (&out);
}

/* Internal structure representing a service found advertised by a peer on the
 * local network. This includes details for connecting to the service, and the
 * metadata associated with the advert (@txt). */
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

/* Check whether @str is entirely lower case. */
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

/* Look up @key in the @attributes table derived from a TXT record, and validate
 * that its value is of type @value_type. If the key is not found, or its value
 * is of the wrong type or is not in normal form, %NULL is returned. @key must
 * be lowercase in order to match reliably. */
static GVariant *
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

/* Bloom hash function family for #OstreeCollectionRef, parameterised by @k. */
static guint64
ostree_collection_ref_bloom_hash (gconstpointer element,
                                  guint8        k)
{
  const OstreeCollectionRef *ref = element;

  return ostree_str_bloom_hash (ref->collection_id, k) ^ ostree_str_bloom_hash (ref->ref_name, k);
}

/* Return the (possibly empty) subset of @refs which are possibly in the given
 * encoded bloom filter, @bloom_encoded. The returned array is not
 * %NULL-terminated. If there is an error decoding the bloom filter (invalid
 * type, zero length, unknown hash function), %NULL will be returned. */
static GPtrArray *
bloom_refs_intersection (GVariant                          *bloom_encoded,
                         const OstreeCollectionRef * const *refs)
{
  g_autoptr(OstreeBloom) bloom = NULL;
  g_autoptr(GVariant) bloom_variant = NULL;
  guint8 k, hash_id;
  OstreeBloomHashFunc hash_func;
  const guint8 *bloom_bytes;
  gsize n_bloom_bytes;
  g_autoptr(GBytes) bytes = NULL;
  gsize i;
  g_autoptr(GPtrArray) possible_refs = NULL;  /* (element-type OstreeCollectionRef) */

  g_variant_get (bloom_encoded, "(yy@ay)", &k, &hash_id, &bloom_variant);

  if (k == 0)
    return NULL;

  switch (hash_id)
    {
    case 1:
      hash_func = ostree_collection_ref_bloom_hash;
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
      if (ostree_bloom_maybe_contains (bloom, refs[i]))
        g_ptr_array_add (possible_refs, (gpointer) refs[i]);
    }

  return g_steal_pointer (&possible_refs);
}

/* Given a @summary_map of ref name to commit details, and the @collection_id
 * for all the refs in the @summary_map (which may be %NULL if the summary does
 * not specify one), add the refs to @refs_and_checksums.
 *
 * The @summary_map is validated as it’s iterated over; on error, @error will be
 * set and @refs_and_checksums will be left in an undefined state. */
static gboolean
fill_refs_and_checksums_from_summary_map (GVariantIter  *summary_map,
                                          const gchar   *collection_id,
                                          GHashTable    *refs_and_checksums  /* (element-type OstreeCollectionRef utf8) */,
                                          GError       **error)
{
  g_autofree gchar *ref_name = NULL;
  g_autoptr(GVariant) checksum_variant = NULL;

  while (g_variant_iter_next (summary_map, "(s(t@aya{sv}))",
                              (gpointer *) &ref_name, NULL,
                              (gpointer *) &checksum_variant, NULL))
    {
      const OstreeCollectionRef ref = { (gchar *) collection_id, ref_name };

      if (!ostree_validate_rev (ref_name, error))
        return FALSE;
      if (!ostree_validate_structureof_csum_v (checksum_variant, error))
        return FALSE;

      if (g_hash_table_contains (refs_and_checksums, &ref))
        {
          g_autofree gchar *checksum_string = ostree_checksum_from_bytes_v (checksum_variant);

          g_hash_table_replace (refs_and_checksums,
                                ostree_collection_ref_dup (&ref),
                                g_steal_pointer (&checksum_string));
        }
    }

  return TRUE;
}

/* Given a @summary file, add the refs it lists to @refs_and_checksums. This
 * includes the main refs list in the summary, and the map of collection IDs
 * to further refs lists.
 *
 * The @summary is validated as it’s explored; on error, @error will be
 * set and @refs_and_checksums will be left in an undefined state. */
static gboolean
fill_refs_and_checksums_from_summary (GVariant    *summary,
                                      GHashTable  *refs_and_checksums  /* (element-type OstreeCollectionRef utf8) */,
                                      GError     **error)
{
  g_autoptr(GVariant) ref_map_v = NULL;
  g_autoptr(GVariant) additional_metadata_v = NULL;
  GVariantIter ref_map;
  g_auto(GVariantDict) additional_metadata = OT_VARIANT_BUILDER_INITIALIZER;
  const gchar *collection_id;
  g_autoptr(GVariantIter) collection_map = NULL;

  ref_map_v = g_variant_get_child_value (summary, 0);
  additional_metadata_v = g_variant_get_child_value (summary, 1);

  g_variant_iter_init (&ref_map, ref_map_v);
  g_variant_dict_init (&additional_metadata, additional_metadata_v);

  /* If the summary file specifies a collection ID (to apply to all the refs in its
   * ref map), use that to start matching against the queried refs. Otherwise,
   * it might specify all its refs in a collection-map; or the summary format is
   * old and unsuitable for P2P redistribution and we should bail. */
  if (g_variant_dict_lookup (&additional_metadata, OSTREE_SUMMARY_COLLECTION_ID, "&s", &collection_id))
    {
      if (!ostree_validate_collection_id (collection_id, error))
        return FALSE;
      if (!fill_refs_and_checksums_from_summary_map (&ref_map, collection_id, refs_and_checksums, error))
        return FALSE;
    }

  /* Repeat for the other collections listed in the summary. */
  if (g_variant_dict_lookup (&additional_metadata, OSTREE_SUMMARY_COLLECTION_MAP, "a{sa(s(taya{sv}))}", &collection_map))
    {
      while (g_variant_iter_loop (collection_map, "{sa(s(taya{sv}))}", &collection_id, &ref_map))
        {
          if (!ostree_validate_collection_id (collection_id, error))
            return FALSE;
          if (!fill_refs_and_checksums_from_summary_map (&ref_map, collection_id, refs_and_checksums, error))
            return FALSE;
        }
    }

  return TRUE;
}

/* Given a summary file (@summary_bytes), extract the refs it lists, and use that
 * to fill in the checksums in the @supported_ref_to_checksum map. This includes
 * the main refs list in the summary, and the map of collection IDs to further
 * refs lists.
 *
 * The @summary is validated as it’s explored; on error, @error will be
 * set and %FALSE will be returned. If the intersection of the summary file refs
 * and the keys in @supported_ref_to_checksum is empty, an error is set. */
static gboolean
get_refs_and_checksums_from_summary (GBytes      *summary_bytes,
                                     GHashTable  *supported_ref_to_checksum /* (element-type OstreeCollectionRef utf8) */,
                                     GError     **error)
{
  g_autoptr(GVariant) summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, summary_bytes, FALSE));
  GHashTableIter iter;
  const OstreeCollectionRef *ref;
  const gchar *checksum;

  if (!g_variant_is_normal_form (summary))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not normal form");
      return FALSE;
    }
  if (!g_variant_is_of_type (summary, OSTREE_SUMMARY_GVARIANT_FORMAT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Doesn't match variant type '%s'",
                   (char *)OSTREE_SUMMARY_GVARIANT_FORMAT);
      return FALSE;
    }

  if (!fill_refs_and_checksums_from_summary (summary, supported_ref_to_checksum, error))
    return FALSE;

  /* Check that at least one of the refs has a non-%NULL checksum set, otherwise
   * we can discard this peer. */
  g_hash_table_iter_init (&iter, supported_ref_to_checksum);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &ref,
                                 (gpointer *) &checksum))
    {
      if (checksum != NULL)
        return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "No matching refs were found in the summary file");
  return FALSE;
}

/* Download the summary file from @remote, and return the bytes of the file in
 * @out_summary_bytes. This will return %TRUE and set @out_summary_bytes to %NULL
 * if the summary file does not exist. */
static gboolean
fetch_summary_from_remote (OstreeRepo    *repo,
                           OstreeRemote  *remote,
                           GBytes       **out_summary_bytes,
                           GCancellable  *cancellable,
                           GError       **error)
{
  g_autoptr(GBytes) summary_bytes = NULL;
  gboolean remote_already_existed = _ostree_repo_add_remote (repo, remote);
  gboolean success = ostree_repo_remote_fetch_summary_with_options (repo,
                                                                    remote->name,
                                                                    NULL /* options */,
                                                                    &summary_bytes,
                                                                    NULL /* signature */,
                                                                    cancellable,
                                                                    error);

  if (!remote_already_existed)
    _ostree_repo_remove_remote (repo, remote);

  if (!success)
    return FALSE;

  g_assert (out_summary_bytes != NULL);
  *out_summary_bytes = g_steal_pointer (&summary_bytes);
  return TRUE;
}
#endif  /* HAVE_AVAHI */

struct _OstreeRepoFinderAvahi
{
  GObject parent_instance;

#ifdef HAVE_AVAHI
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
#endif  /* HAVE_AVAHI */
};

static void ostree_repo_finder_avahi_iface_init (OstreeRepoFinderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderAvahi, ostree_repo_finder_avahi, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_avahi_iface_init))

#ifdef HAVE_AVAHI

/* Download the summary file from @remote and fill in the checksums in the given
 * @supported_ref_to_checksum hash table, given the existing refs in it as keys.
 * See get_refs_and_checksums_from_summary() for more details. */
static gboolean
get_checksums (OstreeRepoFinderAvahi  *finder,
               OstreeRepo             *repo,
               OstreeRemote           *remote,
               GHashTable             *supported_ref_to_checksum /* (element-type OstreeCollectionRef utf8) */,
               GError                **error)
{
  g_autoptr(GBytes) summary_bytes = NULL;

  if (!fetch_summary_from_remote (repo,
                                  remote,
                                  &summary_bytes,
                                  finder->avahi_cancellable,
                                  error))
    return FALSE;

  if (summary_bytes == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No summary file found on server");
      return FALSE;
    }

  return get_refs_and_checksums_from_summary (summary_bytes, supported_ref_to_checksum, error);
}

/* Build some #OstreeRepoFinderResults out of the given #OstreeAvahiService by
 * parsing its DNS-SD TXT records and finding the intersection between the refs
 * it advertises and @refs. One or more results will be added to @results, with
 * multiple results being added if the intersection of refs covers refs which
 * need different GPG keyrings. One result is added per (uri, keyring) pair.
 *
 * If any of the TXT records are malformed or missing, or if the intersection of
 * refs is empty, return early without modifying @results.
 *
 * This recognises the following TXT records:
 *  - `v` (`y`): Version of the TXT record format. Only version `1` is currently
 *    supported.
 *  - `rb` (`(yyay)`): Bloom filter indicating which refs are served by the peer.
 *  - `st` (`t`): Timestamp (seconds since the Unix epoch, big endian) the
 *    summary file was last modified.
 *  - `ri` (`q`): Repository index, indicating which of several repositories
 *    hosted on the peer this is. Big endian.
 */
static void
ostree_avahi_service_build_repo_finder_result (OstreeAvahiService                *service,
                                               OstreeRepoFinderAvahi             *finder,
                                               OstreeRepo                        *parent_repo,
                                               gint                               priority,
                                               const OstreeCollectionRef * const *refs,
                                               GPtrArray                         *results,
                                               GCancellable                      *cancellable)
{
  g_autoptr(GHashTable) attributes = NULL;
  g_autoptr(GVariant) version = NULL;
  g_autoptr(GVariant) bloom = NULL;
  g_autoptr(GVariant) summary_timestamp = NULL;
  g_autoptr(GVariant) repo_index = NULL;
  g_autofree gchar *repo_path = NULL;
  g_autoptr(GPtrArray) possible_refs = NULL; /* (element-type OstreeCollectionRef) */
  SoupURI *_uri = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr(GError) error = NULL;
  gsize i;
  g_autoptr(GHashTable) repo_to_refs = NULL;  /* (element-type UriAndKeyring GHashTable) */
  GHashTable *supported_ref_to_checksum;  /* (element-type OstreeCollectionRef utf8) */
  GHashTableIter iter;
  UriAndKeyring *repo;

  g_return_if_fail (service != NULL);
  g_return_if_fail (refs != NULL);

  attributes = _ostree_txt_records_parse (service->txt);

  /* Check the record version. */
  version = _ostree_txt_records_lookup_variant (attributes, "v", G_VARIANT_TYPE_BYTE);

  if (g_variant_get_byte (version) != 1)
    {
      g_debug ("Unknown v=%02x attribute provided in TXT record. Ignoring.",
               g_variant_get_byte (version));
      return;
    }

  /* Refs bloom filter? */
  bloom = _ostree_txt_records_lookup_variant (attributes, "rb", G_VARIANT_TYPE ("(yyay)"));

  if (bloom == NULL)
    {
      g_debug ("Missing rb (refs bloom) attribute in TXT record. Ignoring.");
      return;
    }

  possible_refs = bloom_refs_intersection (bloom, refs);
  if (possible_refs == NULL)
    {
      g_debug ("Wrong k parameter or hash id in rb (refs bloom) attribute in TXT record. Ignoring.");
      return;
    }
  if (possible_refs->len == 0)
    {
      g_debug ("TXT record definitely has no matching refs. Ignoring.");
      return;
    }

  /* Summary timestamp. */
  summary_timestamp = _ostree_txt_records_lookup_variant (attributes, "st", G_VARIANT_TYPE_UINT64);
  if (summary_timestamp == NULL)
    {
      g_debug ("Missing st (summary timestamp) attribute in TXT record. Ignoring.");
      return;
    }

  /* Repository index. */
  repo_index = _ostree_txt_records_lookup_variant (attributes, "ri", G_VARIANT_TYPE_UINT16);
  if (repo_index == NULL)
    {
      g_debug ("Missing ri (repository index) attribute in TXT record. Ignoring.");
      return;
    }
  repo_path = g_strdup_printf ("/%u", GUINT16_FROM_BE (g_variant_get_uint16 (repo_index)));

  /* Create a new result for each keyring needed by @possible_refs. Typically,
   * there will be a separate keyring per collection, but some might be shared. */
  repo_to_refs = g_hash_table_new_full (uri_and_keyring_hash, uri_and_keyring_equal,
                                        (GDestroyNotify) uri_and_keyring_free, (GDestroyNotify) g_hash_table_unref);

  _uri = soup_uri_new (NULL);
  soup_uri_set_scheme (_uri, "http");
  soup_uri_set_host (_uri, service->address);
  soup_uri_set_port (_uri, service->port);
  soup_uri_set_path (_uri, repo_path);
  uri = soup_uri_to_string (_uri, FALSE);
  soup_uri_free (_uri);

  for (i = 0; i < possible_refs->len; i++)
    {
      const OstreeCollectionRef *ref = g_ptr_array_index (possible_refs, i);
      g_autoptr(UriAndKeyring) resolved_repo = NULL;
      g_autoptr(OstreeRemote) keyring_remote = NULL;

      /* Look up the GPG keyring for this ref. */
      keyring_remote = ostree_repo_resolve_keyring_for_collection (parent_repo,
                                                                   ref->collection_id,
                                                                   cancellable, &error);

      if (keyring_remote == NULL)
        {
          g_debug ("Ignoring ref (%s, %s) on host ‘%s’ due to missing keyring: %s",
                   ref->collection_id, refs[i]->ref_name, service->address,
                   error->message);
          g_clear_error (&error);
          continue;
        }

      /* Add this repo to the results, keyed by the canonicalised repository URI
       * to deduplicate the results. */
      g_debug ("Resolved ref (%s, %s) to repo URI ‘%s’ with keyring ‘%s’ from remote ‘%s’.",
               ref->collection_id, ref->ref_name, uri, keyring_remote->keyring,
               keyring_remote->name);

      resolved_repo = uri_and_keyring_new (uri, keyring_remote);

      supported_ref_to_checksum = g_hash_table_lookup (repo_to_refs, resolved_repo);

      if (supported_ref_to_checksum == NULL)
        {
          supported_ref_to_checksum = g_hash_table_new_full (ostree_collection_ref_hash,
                                                             ostree_collection_ref_equal,
                                                             NULL, g_free);
          g_hash_table_insert (repo_to_refs, g_steal_pointer (&resolved_repo), supported_ref_to_checksum  /* transfer */);
        }

      /* Add a placeholder to @supported_ref_to_checksum for this ref. It will
       * be filled out by the get_checksums() call below. */
      g_hash_table_insert (supported_ref_to_checksum, (gpointer) ref, NULL);
    }

  /* Aggregate the results. */
  g_hash_table_iter_init (&iter, repo_to_refs);

  while (g_hash_table_iter_next (&iter, (gpointer *) &repo, (gpointer *) &supported_ref_to_checksum))
    {
      g_autoptr(OstreeRemote) remote = NULL;

      /* Build an #OstreeRemote. Use the escaped URI, since remote->name
       * is used in file paths, so needs to not contain special characters. */
      g_autofree gchar *name = uri_and_keyring_to_name (repo);
      remote = ostree_remote_new_dynamic (name, repo->keyring_remote->name);

      g_clear_pointer (&remote->keyring, g_free);
      remote->keyring = g_strdup (repo->keyring_remote->keyring);

      /* gpg-verify-summary is false since we use the unsigned summary file support. */
      g_key_file_set_string (remote->options, remote->group, "url", repo->uri);
      g_key_file_set_boolean (remote->options, remote->group, "gpg-verify", TRUE);
      g_key_file_set_boolean (remote->options, remote->group, "gpg-verify-summary", FALSE);

      get_checksums (finder, parent_repo, remote, supported_ref_to_checksum, &error);
      if (error != NULL)
        {
          g_debug ("Failed to get checksums for possible refs; ignoring: %s", error->message);
          g_clear_error (&error);
          continue;
        }

      g_ptr_array_add (results, ostree_repo_finder_result_new (remote, OSTREE_REPO_FINDER (finder),
                                                               priority, supported_ref_to_checksum,
                                                               GUINT64_FROM_BE (g_variant_get_uint64 (summary_timestamp))));
    }
}

typedef struct
{
  OstreeCollectionRef **refs;  /* (owned) (array zero-terminated=1) */
  OstreeRepo *parent_repo;  /* (owned) */
} ResolveData;

static void
resolve_data_free (ResolveData *data)
{
  g_object_unref (data->parent_repo);
  ostree_collection_ref_freev (data->refs);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ResolveData, resolve_data_free)

static ResolveData *
resolve_data_new (const OstreeCollectionRef * const *refs,
                  OstreeRepo                        *parent_repo)
{
  g_autoptr(ResolveData) data = NULL;

  data = g_new0 (ResolveData, 1);
  data->refs = ostree_collection_ref_dupv (refs);
  data->parent_repo = g_object_ref (parent_repo);

  return g_steal_pointer (&data);
}

static void
fail_all_pending_tasks (OstreeRepoFinderAvahi *self,
                        GQuark                 domain,
                        gint                   code,
                        const gchar           *format,
                        ...) G_GNUC_PRINTF(4, 5);

/* Executed in @self->avahi_context.
 *
 * Return the given error from all the pending resolve tasks in
 * self->resolve_tasks. */
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

static gint
results_compare_cb (gconstpointer a,
                    gconstpointer b)
{
  const OstreeRepoFinderResult *result_a = *((const OstreeRepoFinderResult **) a);
  const OstreeRepoFinderResult *result_b = *((const OstreeRepoFinderResult **) b);

  return ostree_repo_finder_result_compare (result_a, result_b);
}

/* Executed in @self->avahi_context.
 *
 * For each of the pending resolve tasks in self->resolve_tasks, calculate and
 * return the result set for its query given the currently known services from
 * Avahi which are stored in self->found_services. */
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
      const OstreeCollectionRef * const *refs;
      gsize j;

      task = G_TASK (g_ptr_array_index (self->resolve_tasks, i));
      data = g_task_get_task_data (task);
      refs = (const OstreeCollectionRef * const *) data->refs;
      results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);

      for (j = 0; j < self->found_services->len; j++)
        {
          OstreeAvahiService *service = g_ptr_array_index (self->found_services, j);

          ostree_avahi_service_build_repo_finder_result (service, self, data->parent_repo,
                                                         priority, refs, results,
                                                         self->avahi_cancellable);
          if (g_cancellable_is_cancelled (self->avahi_cancellable))
            {
              cancelled = TRUE;
              break;
            }
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

          g_ptr_array_sort (results, results_compare_cb);

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
#endif  /* HAVE_AVAHI */

static void
ostree_repo_finder_avahi_resolve_async (OstreeRepoFinder                  *finder,
                                        const OstreeCollectionRef * const *refs,
                                        OstreeRepo                        *parent_repo,
                                        GCancellable                      *cancellable,
                                        GAsyncReadyCallback                callback,
                                        gpointer                           user_data)
{
  OstreeRepoFinderAvahi *self = OSTREE_REPO_FINDER_AVAHI (finder);
  g_autoptr(GTask) task = NULL;

  g_debug ("%s: Starting resolving", G_STRFUNC);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_avahi_resolve_async);

#ifdef HAVE_AVAHI
  g_task_set_task_data (task, resolve_data_new (refs, parent_repo), (GDestroyNotify) resolve_data_free);

  /* Move @task to the @avahi_context where it can be processed. */
  g_main_context_invoke (self->avahi_context, add_resolve_task_cb, g_steal_pointer (&task));
#else  /* if !HAVE_AVAHI */
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Avahi support was not compiled in to libostree");
#endif  /* !HAVE_AVAHI */
}

#ifdef HAVE_AVAHI
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
#endif  /* HAVE_AVAHI */

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
#ifdef HAVE_AVAHI
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
#endif  /* HAVE_AVAHI */

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
#ifdef HAVE_AVAHI
  self->resolve_tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  self->avahi_cancellable = g_cancellable_new ();
  self->client_state = AVAHI_CLIENT_S_REGISTERING;
  self->resolvers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  self->found_services = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_avahi_service_free);
#endif  /* HAVE_AVAHI */
}

/**
 * ostree_repo-finder_avahi_new:
 * @context: (transfer none) (nullable): a #GMainContext for processing Avahi
 *    events in, or %NULL to use the current thread-default
 *
 * Create a new #OstreeRepoFinderAvahi instance. It is intended that one such
 * instance be created per process, and it be used to answer all resolution
 * requests from #OstreeRepos.
 *
 * The calling code is responsible for ensuring that @context is iterated while
 * the #OstreeRepoFinderAvahi is running (after ostree_repo_finder_avahi_start()
 * is called). This may be done from any thread.
 *
 * If @context is %NULL, the current thread-default #GMainContext is used.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderAvahi
 * Since: 2017.8
 */
OstreeRepoFinderAvahi *
ostree_repo_finder_avahi_new (GMainContext *context)
{
  g_autoptr(OstreeRepoFinderAvahi) finder = NULL;

  finder = g_object_new (OSTREE_TYPE_REPO_FINDER_AVAHI, NULL);

#ifdef HAVE_AVAHI
  /* FIXME: Make this a property */
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
#endif  /* HAVE_AVAHI */

  return g_steal_pointer (&finder);
}

/**
 * ostree_repo_finder_avahi_start:
 * @self: an #OstreeRepoFinderAvahi
 * @error: return location for a #GError
 *
 * Start monitoring the local network for peers who are advertising OSTree
 * repositories, using Avahi. In order for this to work, the #GMainContext
 * passed to @self at construction time must be iterated (so it will typically
 * be the global #GMainContext, or be a separate #GMainContext in a worker
 * thread).
 *
 * This will return an error (%G_IO_ERROR_FAILED) if initialisation fails, or if
 * Avahi support is not available (%G_IO_ERROR_NOT_SUPPORTED). In either case,
 * the #OstreeRepoFinderAvahi instance is useless afterwards and should be
 * destroyed.
 *
 * Call ostree_repo_finder_avahi_stop() to stop the repo finder.
 *
 * It is an error to call this function multiple times on the same
 * #OstreeRepoFinderAvahi instance, or to call it after
 * ostree_repo_finder_avahi_stop().
 *
 * Since: 2017.8
 */
void
ostree_repo_finder_avahi_start (OstreeRepoFinderAvahi  *self,
                                GError                **error)
{
  g_return_if_fail (OSTREE_IS_REPO_FINDER_AVAHI (self));
  g_return_if_fail (error == NULL || *error == NULL);

#ifdef HAVE_AVAHI
  g_autoptr(AvahiClient) client = NULL;
  g_autoptr(AvahiServiceBrowser) browser = NULL;
  int failure = 0;

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
#else  /* if !HAVE_AVAHI */
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "Avahi support was not compiled in to libostree");
#endif  /* !HAVE_AVAHI */
}

#ifdef HAVE_AVAHI
static gboolean stop_cb (gpointer user_data);
#endif  /* HAVE_AVAHI */

/**
 * ostree_repo_finder_avahi_stop:
 * @self: an #OstreeRepoFinderAvahi
 *
 * Stop monitoring the local network for peers who are advertising OSTree
 * repositories. If any resolve tasks (from ostree_repo_finder_resolve_async())
 * are in progress, they will be cancelled and will return %G_IO_ERROR_CANCELLED.
 *
 * Call ostree_repo_finder_avahi_start() to start the repo finder.
 *
 * It is an error to call this function multiple times on the same
 * #OstreeRepoFinderAvahi instance, or to call it before
 * ostree_repo_finder_avahi_start().
 *
 * Since: 2017.8
 */
void
ostree_repo_finder_avahi_stop (OstreeRepoFinderAvahi *self)
{
  g_return_if_fail (OSTREE_IS_REPO_FINDER_AVAHI (self));

#ifdef HAVE_AVAHI
  if (self->browser == NULL)
    return;

  g_main_context_invoke (self->avahi_context, stop_cb, g_object_ref (self));
#endif  /* HAVE_AVAHI */
}

#ifdef HAVE_AVAHI
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
#endif  /* HAVE_AVAHI */
