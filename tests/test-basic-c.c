/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Red Hat, Inc.
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
 */

#include "config.h"

#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

#include "libglnx.h"
#include "libostreetest.h"

static void
test_repo_is_not_system (gconstpointer data)
{
  OstreeRepo *repo = (void*)data;
  g_assert (!ostree_repo_is_system (repo));
}

int main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;

  g_test_init (&argc, &argv, NULL);

  repo = ot_test_setup_repo (NULL, &error); 
  if (!repo)
    goto out;
  
  g_test_add_data_func ("/repo-not-system", repo, test_repo_is_not_system);

  return g_test_run();
 out:
  if (error)
    g_error ("%s", error->message);
  return 1;
}
