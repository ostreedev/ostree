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

#pragma once

#include "ostree-bootconfig-parser.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_DEPLOYMENT (ostree_deployment_get_type ())
#define OSTREE_DEPLOYMENT(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OSTREE_TYPE_DEPLOYMENT, OstreeDeployment))
#define OSTREE_IS_DEPLOYMENT(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OSTREE_TYPE_DEPLOYMENT))

typedef struct _OstreeDeployment OstreeDeployment;

GType ostree_deployment_get_type (void) G_GNUC_CONST;

guint ostree_deployment_hash (gconstpointer v);
gboolean ostree_deployment_equal (gconstpointer ap, gconstpointer bp);

OstreeDeployment * ostree_deployment_new (int    index,
                                  const char  *osname,
                                  const char  *csum,
                                  int    deployserial,
                                  const char  *bootcsum,
                                  int    bootserial);

int ostree_deployment_get_index (OstreeDeployment *self);
const char *ostree_deployment_get_osname (OstreeDeployment *self);
int ostree_deployment_get_deployserial (OstreeDeployment *self);
const char *ostree_deployment_get_csum (OstreeDeployment *self);
const char *ostree_deployment_get_bootcsum (OstreeDeployment *self);
int ostree_deployment_get_bootserial (OstreeDeployment *self);
OstreeBootconfigParser *ostree_deployment_get_bootconfig (OstreeDeployment *self);
GKeyFile *ostree_deployment_get_origin (OstreeDeployment *self);

void ostree_deployment_set_index (OstreeDeployment *self, int index);
void ostree_deployment_set_bootserial (OstreeDeployment *self, int index);
void ostree_deployment_set_bootconfig (OstreeDeployment *self, OstreeBootconfigParser *bootconfig);
void ostree_deployment_set_origin (OstreeDeployment *self, GKeyFile *origin);

OstreeDeployment *ostree_deployment_clone (OstreeDeployment *self);


G_END_DECLS
