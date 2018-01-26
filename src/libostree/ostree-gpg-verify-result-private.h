/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "ostree-gpg-verify-result.h"

#include "otutil.h"

/**
 * OstreeGpgVerifyResult:
 *
 * Private instance structure.
 */
struct OstreeGpgVerifyResult {
  GObject parent;

  gpgme_ctx_t context;
  gpgme_verify_result_t details;
};
