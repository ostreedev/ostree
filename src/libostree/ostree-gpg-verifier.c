/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Sjoerd Simons <sjoerd.simons@collabora.co.uk>
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
 * Author: Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 */

#include "config.h"

#include "ostree-gpg-verifier.h"
#include "otutil.h"

#define GPGVGOODPREFIX "[GNUPG:] GOODSIG "

typedef struct {
  GObjectClass parent_class;
} OstreeGpgVerifierClass;

struct OstreeGpgVerifier {
  GObject parent;

  GList *keyrings;
};

static void _ostree_gpg_verifier_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeGpgVerifier, _ostree_gpg_verifier, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, _ostree_gpg_verifier_initable_iface_init))

static void
ostree_gpg_verifier_finalize (GObject *object)
{
  OstreeGpgVerifier *self = OSTREE_GPG_VERIFIER (object);

  g_list_free_full (self->keyrings, g_object_unref);

  G_OBJECT_CLASS (_ostree_gpg_verifier_parent_class)->finalize (object);
}

static void
_ostree_gpg_verifier_class_init (OstreeGpgVerifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ostree_gpg_verifier_finalize;
}

static void
_ostree_gpg_verifier_init (OstreeGpgVerifier *self)
{
}

static gboolean
ostree_gpg_verifier_initable_init (GInitable        *initable,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  gboolean ret = FALSE;
  OstreeGpgVerifier *self = (OstreeGpgVerifier*)initable;
  const char *default_keyring_path = g_getenv ("OSTREE_GPG_HOME");
  gs_unref_object GFile *default_keyring_dir = NULL;
  gs_unref_object GFile *default_pubring = NULL;

  if (!default_keyring_path)
    default_keyring_path = DATADIR "/ostree/trusted.gpg.d/";

  default_keyring_dir = g_file_new_for_path (default_keyring_path);
  if (!_ostree_gpg_verifier_add_keyring_dir (self, default_keyring_dir,
                                             cancellable, error))
    {
      g_prefix_error (error, "Reading keyring directory '%s'",
                      gs_file_get_path_cached (default_keyring_dir));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
_ostree_gpg_verifier_initable_iface_init (GInitableIface *iface)
{
  iface->init = ostree_gpg_verifier_initable_init;
}

typedef struct {
  OstreeGpgVerifier *self;
  GCancellable *cancellable;
  gboolean gpgv_done;
  gboolean status_done;

  gint goodsigs;
  gint exitcode;
  GError *error;
  GMainLoop *loop;
} VerifyRun;

static void
_gpgv_parse_line (VerifyRun *v, const gchar *line)
{
  if (g_str_has_prefix (line, GPGVGOODPREFIX))
    v->goodsigs++;
}

static void
on_process_done (GObject *s, GAsyncResult *res, gpointer user_data)
{
  VerifyRun *v = user_data;
  gs_subprocess_wait_finish (GS_SUBPROCESS (s), res,
                             &v->exitcode, &v->error);

  v->gpgv_done = TRUE;

  g_main_loop_quit (v->loop);
}

static void
on_read_line (GObject *s, GAsyncResult *res, gpointer user_data)
{
  VerifyRun *v = user_data;
  gchar *line;

  /* Ignore errors when reading from the data input */
  line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (s),
                                               res, NULL, NULL);

  if (line == NULL)
    {
      v->status_done = TRUE;
      g_main_loop_quit (v->loop);
    }
  else
    {
      _gpgv_parse_line (v, line);
      g_free (line);
      g_data_input_stream_read_line_async (G_DATA_INPUT_STREAM (s),
                                           G_PRIORITY_DEFAULT, v->cancellable,
                                           on_read_line, v);
    }
}


gboolean
_ostree_gpg_verifier_check_signature (OstreeGpgVerifier   *self,
                                      GFile               *file,
                                      GFile               *signature,
                                      gboolean            *out_had_valid_sig,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  gboolean ret = FALSE;
  gboolean ret_had_valid_sig = FALSE;
  gs_unref_object GSSubprocessContext *context = NULL;
  gs_unref_object GSSubprocess *proc = NULL;
  gs_unref_object GDataInputStream *data = NULL;
  gs_free gchar *status_fd_str = NULL;
  GInputStream *output;
  gint fd;
  VerifyRun v = { 0, };
  GList *item;
  GMainContext *maincontext = NULL;
  GMainLoop *loop = NULL;
  
  g_return_val_if_fail (out_had_valid_sig != NULL, FALSE);

  maincontext = g_main_context_new ();
  loop = g_main_loop_new (maincontext, FALSE);

  g_main_context_push_thread_default (maincontext);

  context = gs_subprocess_context_newv (GPGVPATH, NULL);
  gs_subprocess_context_set_stdin_disposition (context,
                                               GS_SUBPROCESS_STREAM_DISPOSITION_NULL);
  gs_subprocess_context_set_stdout_disposition (context,
                                                GS_SUBPROCESS_STREAM_DISPOSITION_NULL);
  gs_subprocess_context_set_stderr_disposition (context,
                                                GS_SUBPROCESS_STREAM_DISPOSITION_NULL);
  
  if (!gs_subprocess_context_open_pipe_read (context, &output, &fd, error))
    goto out;

  status_fd_str = g_strdup_printf ("%d", fd);
  gs_subprocess_context_argv_append (context, "--status-fd");
  gs_subprocess_context_argv_append (context, status_fd_str);

  for (item = self->keyrings ; item != NULL; item = g_list_next (item))
    {
      GFile *keyring = item->data;
      gs_subprocess_context_argv_append (context, "--keyring");
      gs_subprocess_context_argv_append (context, gs_file_get_path_cached (keyring));
    }

  gs_subprocess_context_argv_append (context, gs_file_get_path_cached (signature));
  gs_subprocess_context_argv_append (context, gs_file_get_path_cached (file));

  proc = gs_subprocess_new (context, cancellable, error);
  if (proc == NULL)
    goto out;

  data = g_data_input_stream_new (output);

  v.self = self;
  v.cancellable = cancellable;
  v.loop = loop;

  gs_subprocess_wait (proc, cancellable, on_process_done, &v);
  g_data_input_stream_read_line_async (data, G_PRIORITY_DEFAULT, cancellable,
                                       on_read_line, &v);

  while (!v.gpgv_done || !v.status_done)
    g_main_loop_run (loop);

  if (v.goodsigs > 0)
    ret_had_valid_sig = TRUE;
  
  ret = TRUE;
  *out_had_valid_sig = ret_had_valid_sig;
 out:
  if (maincontext)
    {
      g_main_context_pop_thread_default (maincontext);
      g_main_context_unref (maincontext);
    }
  if (loop)
    g_main_loop_unref (loop);

  return ret;
}

gboolean
_ostree_gpg_verifier_add_keyring (OstreeGpgVerifier  *self,
                                  GFile              *path,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  g_return_val_if_fail (path != NULL, FALSE);

  self->keyrings = g_list_append (self->keyrings, g_object_ref (path));
  return TRUE;
}

gboolean
_ostree_gpg_verifier_add_keyring_dir (OstreeGpgVerifier   *self,
                                      GFile               *path,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *enumerator = NULL;
  
  enumerator = g_file_enumerate_children (path, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NONE,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &path,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR &&
          g_str_has_suffix (g_file_info_get_name (file_info), ".gpg"))
        self->keyrings = g_list_append (self->keyrings, g_object_ref (path));
    }

  ret = TRUE;
 out:
  return ret;
}

OstreeGpgVerifier*
_ostree_gpg_verifier_new (GCancellable   *cancellable,
                          GError        **error)
{
  return g_initable_new (OSTREE_TYPE_GPG_VERIFIER, cancellable, error, NULL);
}
