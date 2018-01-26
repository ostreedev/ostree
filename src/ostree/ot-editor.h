/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#pragma once

#include <gio/gio.h>

#include "ostree.h"

char *  ot_editor_prompt    (OstreeRepo *repo, const char *input,
                             GCancellable *cancellable, GError **error);
