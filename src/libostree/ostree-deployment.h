/*
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

/**
 * OSTREE_ORIGIN_TRANSIENT_GROUP:
 *
 * The name of a `GKeyFile` group for data that should not
 * be carried across upgrades.  For more information,
 * see ostree_deployment_origin_remove_transient_state().
 *
 * Since: 2018.3
 */
#define OSTREE_ORIGIN_TRANSIENT_GROUP "libostree-transient"

typedef struct _OstreeDeployment OstreeDeployment;

_OSTREE_PUBLIC
GType ostree_deployment_get_type (void) G_GNUC_CONST;

_OSTREE_PUBLIC
guint ostree_deployment_hash (gconstpointer v);
_OSTREE_PUBLIC
gboolean ostree_deployment_equal (gconstpointer ap, gconstpointer bp);

_OSTREE_PUBLIC
OstreeDeployment * ostree_deployment_new (int    index,
                                  const char  *osname,
                                  const char  *csum,
                                  int    deployserial,
                                  const char  *bootcsum,
                                  int    bootserial);

_OSTREE_PUBLIC
int ostree_deployment_get_index (OstreeDeployment *self);
_OSTREE_PUBLIC
const char *ostree_deployment_get_osname (OstreeDeployment *self);
_OSTREE_PUBLIC
int ostree_deployment_get_deployserial (OstreeDeployment *self);
_OSTREE_PUBLIC
const char *ostree_deployment_get_csum (OstreeDeployment *self);
_OSTREE_PUBLIC
const char *ostree_deployment_get_bootcsum (OstreeDeployment *self);
_OSTREE_PUBLIC
int ostree_deployment_get_bootserial (OstreeDeployment *self);
_OSTREE_PUBLIC
OstreeBootconfigParser *ostree_deployment_get_bootconfig (OstreeDeployment *self);
_OSTREE_PUBLIC
GKeyFile *ostree_deployment_get_origin (OstreeDeployment *self);


_OSTREE_PUBLIC
gboolean ostree_deployment_is_pinned (OstreeDeployment *self);

_OSTREE_PUBLIC
void ostree_deployment_set_index (OstreeDeployment *self, int index);
_OSTREE_PUBLIC
void ostree_deployment_set_bootserial (OstreeDeployment *self, int index);
_OSTREE_PUBLIC
void ostree_deployment_set_bootconfig (OstreeDeployment *self, OstreeBootconfigParser *bootconfig);
_OSTREE_PUBLIC
void ostree_deployment_set_origin (OstreeDeployment *self, GKeyFile *origin);

_OSTREE_PUBLIC
void ostree_deployment_origin_remove_transient_state (GKeyFile *origin);

_OSTREE_PUBLIC
OstreeDeployment *ostree_deployment_clone (OstreeDeployment *self);

_OSTREE_PUBLIC
char *ostree_deployment_get_origin_relpath (OstreeDeployment *self);

typedef enum {
  OSTREE_DEPLOYMENT_UNLOCKED_NONE,
  OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT,
  OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX
} OstreeDeploymentUnlockedState;

_OSTREE_PUBLIC
const char *ostree_deployment_unlocked_state_to_string (OstreeDeploymentUnlockedState state);

_OSTREE_PUBLIC
OstreeDeploymentUnlockedState ostree_deployment_get_unlocked (OstreeDeployment *self);

G_END_DECLS
