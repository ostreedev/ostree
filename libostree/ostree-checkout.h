/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#ifndef _OSTREE_CHECKOUT
#define _OSTREE_CHECKOUT

#include <ostree-repo.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_CHECKOUT ostree_checkout_get_type()
#define OSTREE_CHECKOUT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_CHECKOUT, OstreeCheckout))
#define OSTREE_CHECKOUT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), OSTREE_TYPE_CHECKOUT, OstreeCheckoutClass))
#define OSTREE_IS_CHECKOUT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_CHECKOUT))
#define OSTREE_IS_CHECKOUT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), OSTREE_TYPE_CHECKOUT))
#define OSTREE_CHECKOUT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), OSTREE_TYPE_CHECKOUT, OstreeCheckoutClass))

typedef struct {
  GObject parent;
} OstreeCheckout;

typedef struct {
  GObjectClass parent_class;
} OstreeCheckoutClass;

GType ostree_checkout_get_type (void);

OstreeCheckout* ostree_checkout_new (OstreeRepo  *repo,
                                     const char  *path);

gboolean ostree_checkout_run_triggers (OstreeCheckout *checkout,
                                       GError        **error);

G_END_DECLS

#endif /* _OSTREE_CHECKOUT */
