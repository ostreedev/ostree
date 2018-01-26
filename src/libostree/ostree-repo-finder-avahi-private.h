/*
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <avahi-common/strlst.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

GHashTable *_ostree_txt_records_parse (AvahiStringList *txt);

G_END_DECLS
