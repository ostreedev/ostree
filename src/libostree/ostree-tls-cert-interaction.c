/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-tls-cert-interaction.h"

struct _OstreeTlsCertInteraction
{
  GTlsInteraction parent_instance;

  char *cert_path;
  char *key_path;
  GTlsCertificate *cert;
};

struct _OstreeTlsCertInteractionClass
{
  GTlsInteractionClass parent_class;
};

#include <string.h>

G_DEFINE_TYPE (OstreeTlsCertInteraction, _ostree_tls_cert_interaction, G_TYPE_TLS_INTERACTION);

static GTlsInteractionResult
request_certificate (GTlsInteraction              *interaction,
                     GTlsConnection               *connection,
                     GTlsCertificateRequestFlags   flags,
                     GCancellable                 *cancellable,
                     GError                      **error)
{
  OstreeTlsCertInteraction *self = (OstreeTlsCertInteraction*)interaction;

  if (!self->cert)
    {
      self->cert = g_tls_certificate_new_from_files (self->cert_path, self->key_path, error);
      if (!self->cert)
        return G_TLS_INTERACTION_FAILED;
    }

  g_tls_connection_set_certificate (connection, self->cert);
  return G_TLS_INTERACTION_HANDLED;
}

static void
_ostree_tls_cert_interaction_init (OstreeTlsCertInteraction *interaction)
{
}

static void
_ostree_tls_cert_interaction_class_init (OstreeTlsCertInteractionClass *klass)
{
  GTlsInteractionClass *interaction_class = G_TLS_INTERACTION_CLASS (klass);
  interaction_class->request_certificate = request_certificate;
}

OstreeTlsCertInteraction *
_ostree_tls_cert_interaction_new (const char *cert_path,
                                  const char *key_path)
{
  OstreeTlsCertInteraction *self = g_object_new (OSTREE_TYPE_TLS_CERT_INTERACTION, NULL);
  self->cert_path = g_strdup (cert_path);
  self->key_path = g_strdup (key_path);
  return self;
}
