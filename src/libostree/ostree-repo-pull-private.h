/*
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "ostree-core.h"

G_BEGIN_DECLS

gboolean
_ostree_repo_verify_bindings (const char  *collection_id,
                              const char  *ref_name,
                              GVariant    *commit,
                              GError     **error);

G_END_DECLS
