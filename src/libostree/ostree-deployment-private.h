/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "ostree-deployment.h"

G_BEGIN_DECLS


/**
 * OstreeDeployment:
 * @parent_instance:
 * @index: Global offset
 * @osname:
 * @csum: OSTree checksum of tree
 * @deployserial: How many times this particular csum appears in deployment list
 * @bootcsum: Checksum of kernel+initramfs
 * @bootserial: An integer assigned to this tree per its ${bootcsum}
 * @bootconfig: Bootloader configuration
 * @origin: How to construct an upgraded version of this tree
 * @unlocked: The unlocked state
 */
struct _OstreeDeployment
{
  GObject       parent_instance;

  int index;
  char *osname;
  char *csum;
  int deployserial;
  char *bootcsum;
  int bootserial;
  OstreeBootconfigParser *bootconfig;
  GKeyFile *origin;
  OstreeDeploymentUnlockedState unlocked;
};

void _ostree_deployment_set_bootcsum (OstreeDeployment *self, const char *bootcsum);

G_END_DECLS
