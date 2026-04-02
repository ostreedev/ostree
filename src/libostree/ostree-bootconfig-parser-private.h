/* SPDX-License-Identifier: LGPL-2.0+ */

#pragma once

#include "ostree-bootconfig-parser.h"

G_BEGIN_DECLS

const char *_ostree_bootconfig_parser_filename (OstreeBootconfigParser *self);

GVariant *_ostree_bootconfig_parser_get_extra_keys_variant (OstreeBootconfigParser *self);

G_END_DECLS
