/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __OT_DEPLOYMENT_H__
#define __OT_DEPLOYMENT_H__

#include <gio/gio.h>
#include "ot-config-parser.h"

G_BEGIN_DECLS

#define OT_TYPE_DEPLOYMENT (ot_deployment_get_type ())
#define OT_DEPLOYMENT(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OT_TYPE_DEPLOYMENT, OtDeployment))
#define OT_IS_DEPLOYMENT(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OT_TYPE_DEPLOYMENT))

typedef struct _OtDeployment OtDeployment;

GType ot_deployment_get_type (void) G_GNUC_CONST;

guint ot_deployment_hash (gconstpointer v);
gboolean ot_deployment_equal (gconstpointer a, gconstpointer b);

OtDeployment * ot_deployment_new (int    index,
                                  const char  *osname,
                                  const char  *csum,
                                  int    deployserial,
                                  const char  *bootcsum,
                                  int    bootserial);

int ot_deployment_get_index (OtDeployment *self);
const char *ot_deployment_get_osname (OtDeployment *self);
int ot_deployment_get_deployserial (OtDeployment *self);
const char *ot_deployment_get_csum (OtDeployment *self);
const char *ot_deployment_get_bootcsum (OtDeployment *self);
int ot_deployment_get_bootserial (OtDeployment *self);
OtConfigParser *ot_deployment_get_bootconfig (OtDeployment *self);
GKeyFile *ot_deployment_get_origin (OtDeployment *self);

void ot_deployment_set_index (OtDeployment *self, int index);
void ot_deployment_set_bootserial (OtDeployment *self, int index);
void ot_deployment_set_bootconfig (OtDeployment *self, OtConfigParser *bootconfig);
void ot_deployment_set_origin (OtDeployment *self, GKeyFile *origin);

OtDeployment *ot_deployment_clone (OtDeployment *self);


G_END_DECLS

#endif /* __OT_DEPLOYMENT_H__ */
