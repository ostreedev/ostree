/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "otutil.h"
#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_TLS_CERT_INTERACTION         (_ostree_tls_cert_interaction_get_type ())
#define OSTREE_TLS_CERT_INTERACTION(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_TLS_CERT_INTERACTION, OstreeTlsCertInteraction))
#define OSTREE_TLS_CERT_INTERACTION_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_TLS_CERT_INTERACTION, OstreeTlsCertInteractionClass))
#define OSTREE_IS_TLS_CERT_INTERACTION(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_TLS_CERT_INTERACTION))
#define OSTREE_IS_TLS_CERT_INTERACTION_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_TLS_CERT_INTERACTION))
#define OSTREE_TLS_CERT_INTERACTION_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_TLS_CERT_INTERACTION, OstreeTlsCertInteractionClass))

typedef struct _OstreeTlsCertInteraction        OstreeTlsCertInteraction;
typedef struct _OstreeTlsCertInteractionClass   OstreeTlsCertInteractionClass;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeTlsCertInteraction, g_object_unref)

GType                       _ostree_tls_cert_interaction_get_type    (void) G_GNUC_CONST;

OstreeTlsCertInteraction *  _ostree_tls_cert_interaction_new         (const char *cert_path,
                                                                      const char *key_path);

G_END_DECLS
