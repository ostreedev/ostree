/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

#define BUILTINPROTO(name) gboolean ot_remote_builtin_ ## name (int argc, char **argv, \
                                                                OstreeCommandInvocation *invocation, \
                                                                GCancellable *cancellable, GError **error)

BUILTINPROTO(add);
BUILTINPROTO(delete);
BUILTINPROTO(gpg_import);
BUILTINPROTO(list);
#ifdef HAVE_LIBCURL_OR_LIBSOUP
BUILTINPROTO(add_cookie);
BUILTINPROTO(list_cookies);
BUILTINPROTO(delete_cookie);
#endif
BUILTINPROTO(show_url);
BUILTINPROTO(refs);
BUILTINPROTO(summary);

#undef BUILTINPROTO

G_END_DECLS
