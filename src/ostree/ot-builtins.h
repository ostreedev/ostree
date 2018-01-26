/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include "config.h"

#include "ostree.h"

G_BEGIN_DECLS

#define BUILTINPROTO(name) gboolean ostree_builtin_ ## name (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)

BUILTINPROTO(admin);
BUILTINPROTO(cat);
BUILTINPROTO(config);
BUILTINPROTO(checkout);
BUILTINPROTO(checksum);
BUILTINPROTO(commit);
BUILTINPROTO(diff);
BUILTINPROTO(export);
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
BUILTINPROTO(find_remotes);
BUILTINPROTO(create_usb);
#endif
BUILTINPROTO(gpg_sign);
BUILTINPROTO(init);
BUILTINPROTO(log);
BUILTINPROTO(pull);
BUILTINPROTO(pull_local);
BUILTINPROTO(ls);
BUILTINPROTO(prune);
BUILTINPROTO(refs);
BUILTINPROTO(reset);
BUILTINPROTO(fsck);
BUILTINPROTO(show);
BUILTINPROTO(static_delta);
BUILTINPROTO(summary);
BUILTINPROTO(rev_parse);
BUILTINPROTO(remote);
BUILTINPROTO(write_refs);
BUILTINPROTO(trivial_httpd);

#undef BUILTINPROTO

G_END_DECLS
