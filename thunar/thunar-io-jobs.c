/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2009-2011 Jannis Pohlmann <jannis@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include "thunar/thunar-application.h"
#include "thunar/thunar-enum-types.h"
#include "thunar/thunar-file.h"
#include "thunar/thunar-gio-extensions.h"
#include "thunar/thunar-gobject-extensions.h"
#include "thunar/thunar-io-jobs-util.h"
#include "thunar/thunar-io-jobs.h"
#include "thunar/thunar-io-scan-directory.h"
#include "thunar/thunar-job.h"
#include "thunar/thunar-preferences.h"
#include "thunar/thunar-private.h"
#include "thunar/thunar-simple-job.h"
#include "thunar/thunar-thumbnail-cache.h"
#include "thunar/thunar-transfer-job.h"

#include <gio/gio.h>
#include <glib/gstdio.h>



static GList *
_tij_collect_nofollow (ThunarJob *job,
                       GList     *base_file_list,
                       gboolean   unlinking,
                       GError   **error)
{
  GError *err = NULL;
  GList  *child_file_list = NULL;
  GList  *file_list = NULL;
  GList  *lp;

  /* recursively collect the files */
  for (lp = base_file_list;
       err == NULL && lp != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job));
       lp = lp->next)
    {
      /* try to scan the directory */
      child_file_list = thunar_io_scan_directory (job, lp->data,
                                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                  TRUE, unlinking, FALSE, NULL, &err);

      /* prepend the new files to the existing list */
      file_list = thunar_g_list_prepend_deep (file_list, lp->data);
      file_list = g_list_concat (child_file_list, file_list);
    }

  /* check if we failed */
  if (err != NULL || thunar_job_is_cancelled (THUNAR_JOB (job)))
    {
      if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
        g_error_free (err);
      else
        g_propagate_error (error, err);

      /* release the collected files */
      thunar_g_list_free_full (file_list);

      return NULL;
    }

  return file_list;
}



static gboolean
_tij_delete_file (GFile        *file,
                  GCancellable *cancellable,
                  GError      **error)
{
  gchar *path;

  if (!g_file_is_native (file))
    return g_file_delete (file, cancellable, error);

  /* adapted from g_local_file_delete of gio/glocalfile.c */
  path = g_file_get_path (file);

  if (g_remove (path) == 0)
    {
      g_free (path);
      return TRUE;
    }

  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
               _("Error removing file: %s"), g_strerror (errno));

  g_free (path);
  return FALSE;
}



static gboolean
_thunar_io_jobs_create (ThunarJob *job,
                        GArray    *param_values,
                        GError   **error)
{
  GFileOutputStream     *stream;
  ThunarJobResponse      response = THUNAR_JOB_RESPONSE_CANCEL;
  GFileInfo             *info;
  GError                *err = NULL;
  GList                 *file_list;
  GList                 *lp;
  gchar                 *base_name;
  gchar                 *display_name;
  guint                  n_processed = 0;
  GFile                 *template_file;
  GFileInputStream      *template_stream = NULL;
  ThunarJobOperation    *operation = NULL;
  ThunarOperationLogMode log_mode;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 2, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* get the file list */
  file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));
  template_file = g_value_get_object (&g_array_index (param_values, GValue, 1));

  /* we know the total amount of files to be processed */
  thunar_job_set_total_files (THUNAR_JOB (job), file_list);

  /* check if we need to open the template */
  if (template_file != NULL)
    {
      /* open read stream to feed in the new files */
      template_stream = g_file_read (template_file, thunar_job_get_cancellable (THUNAR_JOB (job)), &err);
      if (G_UNLIKELY (template_stream == NULL))
        {
          g_propagate_error (error, err);
          return FALSE;
        }
    }

  log_mode = thunar_job_get_log_mode (job);

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    operation = thunar_job_operation_new (THUNAR_JOB_OPERATION_KIND_CREATE_FILE);

  /* iterate over all files in the list */
  for (lp = file_list;
       err == NULL && lp != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job));
       lp = lp->next, n_processed++)
    {
      g_assert (G_IS_FILE (lp->data));

      /* update progress information */
      thunar_job_processing_file (THUNAR_JOB (job), lp, n_processed);

again:
      /* try to create the file */
      stream = g_file_create (lp->data,
                              G_FILE_CREATE_NONE,
                              thunar_job_get_cancellable (THUNAR_JOB (job)),
                              &err);

      /* abort if the job was cancelled */
      if (thunar_job_is_cancelled (THUNAR_JOB (job)))
        break;

      /* check if creating failed */
      if (stream == NULL)
        {
          if (err->code == G_IO_ERROR_EXISTS)
            {
              g_clear_error (&err);

              /* the file already exists, query its display name */
              info = g_file_query_info (lp->data,
                                        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                        G_FILE_QUERY_INFO_NONE,
                                        thunar_job_get_cancellable (THUNAR_JOB (job)),
                                        NULL);

              /* abort if the job was cancelled */
              if (thunar_job_is_cancelled (THUNAR_JOB (job)))
                break;

              /* determine the display name, using the basename as a fallback */
              if (info != NULL)
                {
                  display_name = g_strdup (g_file_info_get_display_name (info));
                  g_object_unref (info);
                }
              else
                {
                  base_name = g_file_get_basename (lp->data);
                  display_name = g_filename_display_name (base_name);
                  g_free (base_name);
                }

              /* ask the user whether he wants to overwrite the existing file */
              response = thunar_job_ask_overwrite (THUNAR_JOB (job),
                                                   _("The file \"%s\" already exists"),
                                                   display_name);

              /* clean up */
              g_free (display_name);

              /* check if we should overwrite */
              if (response == THUNAR_JOB_RESPONSE_REPLACE)
                {
                  /* try to remove the file. fail if not possible */
                  if (_tij_delete_file (lp->data, thunar_job_get_cancellable (THUNAR_JOB (job)), &err))
                    goto again;
                }
            }
          else
            {
              /* determine display name of the file */
              base_name = g_file_get_basename (lp->data);
              display_name = g_filename_display_basename (base_name);
              g_free (base_name);

              /* ask the user whether to skip/retry this path (cancels the job if not) */
              response = thunar_job_ask_skip (THUNAR_JOB (job),
                                              _("Failed to create empty file \"%s\": %s"),
                                              display_name, err->message);
              g_free (display_name);

              g_clear_error (&err);

              /* go back to the beginning if the user wants to retry */
              if (response == THUNAR_JOB_RESPONSE_RETRY)
                goto again;
            }
        }
      else
        {
          /* remember the file for possible undo */
          if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
            thunar_job_operation_add (operation, template_file, lp->data);

          if (template_stream != NULL)
            {
              /* write the template into the new file */
              g_output_stream_splice (G_OUTPUT_STREAM (stream),
                                      G_INPUT_STREAM (template_stream),
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                      thunar_job_get_cancellable (THUNAR_JOB (job)),
                                      NULL);
            }

          g_object_unref (stream);
        }
    }

  if (template_stream != NULL)
    g_object_unref (template_stream);

  /* check if we have failed */
  if (err != NULL)
    {
      g_propagate_error (error, err);
      if (operation != NULL)
        g_object_unref (operation);
      return FALSE;
    }

  /* check if the job was cancelled */
  if (thunar_job_is_cancelled (THUNAR_JOB (job)))
    {
      if (operation != NULL)
        g_object_unref (operation);
      return FALSE;
    }

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    {
      thunar_job_operation_history_commit (operation);
      g_object_unref (operation);
    }

  /* emit the "new-files" signal with the given file list */
  thunar_job_new_files (THUNAR_JOB (job), file_list);

  return TRUE;
}



ThunarJob *
thunar_io_jobs_create_files (GList *file_list,
                             GFile *template_file)
{
  return thunar_simple_job_new (_thunar_io_jobs_create, 2,
                                THUNAR_TYPE_G_FILE_LIST, file_list,
                                G_TYPE_FILE, template_file);
}



static gboolean
_thunar_io_jobs_mkdir (ThunarJob *job,
                       GArray    *param_values,
                       GError   **error)
{
  ThunarJobResponse      response;
  GFileInfo             *info;
  GError                *err = NULL;
  GList                 *file_list;
  GList                 *lp;
  gchar                 *base_name;
  gchar                 *display_name;
  guint                  n_processed = 0;
  ThunarJobOperation    *operation = NULL;
  ThunarOperationLogMode log_mode;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 1, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));

  /* we know the total list of files to process */
  thunar_job_set_total_files (THUNAR_JOB (job), file_list);

  log_mode = thunar_job_get_log_mode (job);

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    operation = thunar_job_operation_new (THUNAR_JOB_OPERATION_KIND_CREATE_FOLDER);

  for (lp = file_list;
       err == NULL && lp != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job));
       lp = lp->next, n_processed++)
    {
      g_assert (G_IS_FILE (lp->data));

      /* update progress information */
      thunar_job_processing_file (THUNAR_JOB (job), lp, n_processed);

again:
      /* try to create the directory */
      if (!g_file_make_directory (lp->data, thunar_job_get_cancellable (THUNAR_JOB (job)), &err))
        {
          if (err->code == G_IO_ERROR_EXISTS)
            {
              g_error_free (err);
              err = NULL;

              /* abort if the job was cancelled */
              if (thunar_job_is_cancelled (THUNAR_JOB (job)))
                break;

              /* the file already exists, query its display name */
              info = g_file_query_info (lp->data,
                                        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                        G_FILE_QUERY_INFO_NONE,
                                        thunar_job_get_cancellable (THUNAR_JOB (job)),
                                        NULL);

              /* abort if the job was cancelled */
              if (thunar_job_is_cancelled (THUNAR_JOB (job)))
                break;

              /* determine the display name, using the basename as a fallback */
              if (info != NULL)
                {
                  display_name = g_strdup (g_file_info_get_display_name (info));
                  g_object_unref (info);
                }
              else
                {
                  base_name = g_file_get_basename (lp->data);
                  display_name = g_filename_display_name (base_name);
                  g_free (base_name);
                }

              /* ask the user whether he wants to overwrite the existing file */
              response = thunar_job_ask_overwrite (THUNAR_JOB (job),
                                                   _("The file \"%s\" already exists"),
                                                   display_name);

              /* clean up */
              g_free (display_name);

              /* check if we should overwrite it */
              if (response == THUNAR_JOB_RESPONSE_REPLACE)
                {
                  /* try to remove the file, fail if not possible */
                  if (_tij_delete_file (lp->data, thunar_job_get_cancellable (THUNAR_JOB (job)), &err))
                    goto again;
                }
            }
          else
            {
              /* determine the display name of the file */
              base_name = g_file_get_basename (lp->data);
              display_name = g_filename_display_basename (base_name);
              g_free (base_name);

              /* ask the user whether to skip/retry this path (cancels the job if not) */
              response = thunar_job_ask_skip (THUNAR_JOB (job),
                                              _("Failed to create directory \"%s\": %s"),
                                              display_name, err->message);
              g_free (display_name);

              g_error_free (err);
              err = NULL;

              /* go back to the beginning if the user wants to retry */
              if (response == THUNAR_JOB_RESPONSE_RETRY)
                goto again;
            }
        } /* end try creation */

      /* remember the file for possible undo */
      if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
        thunar_job_operation_add (operation, NULL, lp->data);

    } /* end for all files */

  /* check if we have failed */
  if (err != NULL)
    {
      g_propagate_error (error, err);
      if (operation != NULL)
        g_object_unref (operation);
      return FALSE;
    }

  /* check if the job was cancelled */
  if (thunar_job_is_cancelled (THUNAR_JOB (job)))
    {
      if (operation != NULL)
        g_object_unref (operation);
      return FALSE;
    }

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    {
      thunar_job_operation_history_commit (operation);
      g_object_unref (operation);
    }

  /* emit the "new-files" signal with the given file list */
  thunar_job_new_files (THUNAR_JOB (job), file_list);

  return TRUE;
}



ThunarJob *
thunar_io_jobs_make_directories (GList *file_list)
{
  return thunar_simple_job_new (_thunar_io_jobs_mkdir, 1,
                                THUNAR_TYPE_G_FILE_LIST, file_list);
}



static gboolean
_thunar_io_jobs_unlink (ThunarJob *job,
                        GArray    *param_values,
                        GError   **error)
{
  ThunarThumbnailCache *thumbnail_cache;
  ThunarApplication    *application;
  ThunarJobResponse     response;
  GFileInfo            *info;
  GError               *err = NULL;
  GList                *file_list;
  GList                *stage_file_list = NULL;
  GList                *lp;
  GFile                *parent;
  gchar                *base_name;
  gchar                *display_name;
  gboolean              retry;
  guint                 n_processed = 0;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 1, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* get the file list */
  file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));

  /* tell the user that we're preparing to unlink the files */
  thunar_job_info_message (THUNAR_JOB (job), _("Preparing..."));

  /* handle and exclude any files that cannot be written to */
  for (lp = file_list; lp != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job)); lp = lp->next)
    {
      /* if file is in trash we don't need to query the parent directory for
         write permissions */
      if (thunar_g_file_is_trashed (lp->data))
        {
          stage_file_list = thunar_g_list_prepend_deep (stage_file_list, lp->data);
          continue;
        }

      parent = g_file_get_parent (lp->data);
      if (parent == NULL)
        continue;

      do
        {
          retry = FALSE;
          info = g_file_query_info (parent,
                                    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    thunar_job_get_cancellable (THUNAR_JOB (job)),
                                    &err);

          if (err != NULL)
            {
              g_propagate_error (error, err);
              thunar_g_list_free_full (stage_file_list);
              g_object_unref (parent);
              return FALSE;
            }

          if (thunar_job_is_cancelled (THUNAR_JOB (job)))
            {
              thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error);
              thunar_g_list_free_full (stage_file_list);
              g_object_unref (parent);
              return FALSE;
            }

          if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)
              && !g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
            {
              response = thunar_job_ask_skip (THUNAR_JOB (job),
                                              _("You do not have permission to delete the file \"%s\""),
                                              g_file_info_get_display_name (info));
              g_object_unref (info);
              retry = response == THUNAR_JOB_RESPONSE_RETRY;
            }
          else
            {
              stage_file_list = thunar_g_list_prepend_deep (stage_file_list, lp->data);
              g_object_unref (info);
            }
        }
      while (retry);

      g_object_unref (parent);
    }

  if (stage_file_list == NULL)
    return TRUE;

  /* recursively collect files for removal, not following any symlinks */
  file_list = _tij_collect_nofollow (job, stage_file_list, TRUE, &err);
  thunar_g_list_free_full (stage_file_list);

  /* free the file list and fail if there was an error or the job was cancelled */
  if (err != NULL || thunar_job_is_cancelled (THUNAR_JOB (job)))
    {
      if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
        g_error_free (err);
      else
        g_propagate_error (error, err);

      thunar_g_list_free_full (file_list);
      return FALSE;
    }

  /* we know the total list of files to process */
  thunar_job_set_total_files (THUNAR_JOB (job), file_list);

  /* take a reference on the thumbnail cache */
  application = thunar_application_get ();
  thumbnail_cache = thunar_application_get_thumbnail_cache (application);
  g_object_unref (application);

  /* remove all the files */
  for (lp = file_list;
       lp != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job));
       lp = lp->next, n_processed++)
    {
      g_assert (G_IS_FILE (lp->data));

      /* skip root folders which cannot be deleted anyway */
      if (thunar_g_file_is_root (lp->data))
        continue;

      /* update progress information */
      thunar_job_processing_file (THUNAR_JOB (job), lp, n_processed);

again:
      /* try to delete the file */
      if (_tij_delete_file (lp->data, thunar_job_get_cancellable (THUNAR_JOB (job)), &err))
        {
          /* notify the thumbnail cache that the corresponding thumbnail can also
           * be deleted now */
          thunar_thumbnail_cache_delete_file (thumbnail_cache, lp->data);
        }
      else
        {
          /* query the file info for the display name */
          info = g_file_query_info (lp->data,
                                    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                    G_FILE_QUERY_INFO_NONE,
                                    thunar_job_get_cancellable (THUNAR_JOB (job)),
                                    NULL);

          /* abort if the job was cancelled */
          if (thunar_job_is_cancelled (THUNAR_JOB (job)))
            {
              g_clear_error (&err);
              break;
            }

          /* determine the display name, using the basename as a fallback */
          if (info != NULL)
            {
              display_name = g_strdup (g_file_info_get_display_name (info));
              g_object_unref (info);
            }
          else
            {
              base_name = g_file_get_basename (lp->data);
              display_name = g_filename_display_name (base_name);
              g_free (base_name);
            }

          /* ask the user whether he wants to skip this file */
          response = thunar_job_ask_skip (THUNAR_JOB (job),
                                          _("Could not delete file \"%s\": %s"),
                                          display_name, err->message);
          g_free (display_name);

          /* clear the error */
          g_clear_error (&err);

          /* check whether to retry */
          if (response == THUNAR_JOB_RESPONSE_RETRY)
            goto again;
        }
    }

  /* release the thumbnail cache */
  g_object_unref (thumbnail_cache);

  /* release the file list */
  thunar_g_list_free_full (file_list);

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;
  else
    return TRUE;
}



ThunarJob *
thunar_io_jobs_unlink_files (GList *file_list)
{
  return thunar_simple_job_new (_thunar_io_jobs_unlink, 1,
                                THUNAR_TYPE_G_FILE_LIST, file_list);
}



ThunarJob *
thunar_io_jobs_move_files (GList *source_file_list,
                           GList *target_file_list)
{
  ThunarJob *job;

  _thunar_return_val_if_fail (source_file_list != NULL, NULL);
  _thunar_return_val_if_fail (target_file_list != NULL, NULL);
  _thunar_return_val_if_fail (g_list_length (source_file_list) == g_list_length (target_file_list), NULL);

  job = thunar_transfer_job_new (source_file_list, target_file_list,
                                 THUNAR_TRANSFER_JOB_MOVE);
  thunar_job_set_total_files (job, source_file_list);
  thunar_job_set_pausable (job, TRUE);

  return job;
}



ThunarJob *
thunar_io_jobs_copy_files (GList *source_file_list,
                           GList *target_file_list)
{
  ThunarJob *job;

  _thunar_return_val_if_fail (source_file_list != NULL, NULL);
  _thunar_return_val_if_fail (target_file_list != NULL, NULL);
  _thunar_return_val_if_fail (g_list_length (source_file_list) == g_list_length (target_file_list), NULL);

  job = thunar_transfer_job_new (source_file_list, target_file_list,
                                 THUNAR_TRANSFER_JOB_COPY);
  thunar_job_set_total_files (job, source_file_list);
  thunar_job_set_pausable (job, TRUE);

  return job;
}



static GFile *
_thunar_io_jobs_link_file (ThunarJob *job,
                           GFile     *source_file,
                           GFile     *target_file,
                           GError   **error)
{
  ThunarJobResponse response;
  GError           *err = NULL;
  gchar            *symlink_target;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), NULL);
  _thunar_return_val_if_fail (G_IS_FILE (source_file), NULL);
  _thunar_return_val_if_fail (G_IS_FILE (target_file), NULL);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, NULL);

  /* abort on cancellation */
  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return NULL;

  /* various attempts to create the symbolic link */
  while (err == NULL)
    {
      GFile *symlink;
      if (!g_file_equal (source_file, target_file))
        symlink = g_object_ref (target_file);
      else
        symlink = thunar_io_jobs_util_next_duplicate_file (job, source_file, THUNAR_NEXT_FILE_NAME_MODE_LINK, &err);
      if (err == NULL)
        {
          /* try to create the symlink from the g_file */
          symlink_target = thunar_g_file_get_link_path_for_symlink (source_file, symlink);
          if (g_file_make_symbolic_link (symlink, symlink_target,
                                         thunar_job_get_cancellable (THUNAR_JOB (job)),
                                         &err))
            {
              g_free (symlink_target);

              /* return the new symlink */
              return symlink;
            }

          /* on error release the target, to be reused in the next iteration */
          g_free (symlink_target);
        }

      g_object_unref (symlink);

      /* check if we can recover from this error */
      if (err->domain == G_IO_ERROR && err->code == G_IO_ERROR_EXISTS)
        {
          /* ask the user whether to replace the target file */
          response = thunar_job_ask_overwrite (job, "%s", err->message);

          /* reset the error */
          g_clear_error (&err);

          /* propagate the cancelled error if the job was aborted */
          if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), &err))
            break;

          /* try to delete the file */
          if (response == THUNAR_JOB_RESPONSE_REPLACE)
            {
              /* try to remove the target file. if not possible, err will be set and
               * the while loop will be aborted */
              _tij_delete_file (target_file, thunar_job_get_cancellable (THUNAR_JOB (job)), &err);
            }

          /* tell the caller that we skipped this file if the user doesn't want to
           * overwrite it */
          if (response == THUNAR_JOB_RESPONSE_SKIP)
            return g_object_ref (source_file);
        }
    }

  _thunar_assert (err != NULL);

  g_propagate_error (error, err);
  return NULL;
}



static gboolean
_thunar_io_jobs_link (ThunarJob *job,
                      GArray    *param_values,
                      GError   **error)
{
  ThunarThumbnailCache  *thumbnail_cache;
  ThunarApplication     *application;
  GError                *err = NULL;
  GFile                 *real_target_file;
  GList                 *new_files_list = NULL;
  GList                 *source_file_list;
  GList                 *sp;
  GList                 *target_file_list;
  GList                 *tp;
  guint                  n_processed = 0;
  ThunarJobOperation    *operation = NULL;
  ThunarOperationLogMode log_mode;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 2, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  source_file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));
  target_file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 1));

  /* we know the total list of paths to process */
  thunar_job_set_total_files (THUNAR_JOB (job), source_file_list);

  /* take a reference on the thumbnail cache */
  application = thunar_application_get ();
  thumbnail_cache = thunar_application_get_thumbnail_cache (application);
  g_object_unref (application);

  log_mode = thunar_job_get_log_mode (job);

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    operation = thunar_job_operation_new (THUNAR_JOB_OPERATION_KIND_LINK);

  /* process all files */
  for (sp = source_file_list, tp = target_file_list;
       err == NULL && sp != NULL && tp != NULL;
       sp = sp->next, tp = tp->next, n_processed++)
    {
      _thunar_assert (G_IS_FILE (sp->data));
      _thunar_assert (G_IS_FILE (tp->data));

      /* update progress information */
      thunar_job_processing_file (THUNAR_JOB (job), sp, n_processed);

      /* try to create the symbolic link */
      real_target_file = _thunar_io_jobs_link_file (job, sp->data, tp->data, &err);
      if (real_target_file != NULL)
        {
          /* queue the file for the folder update unless it was skipped */
          if (sp->data != real_target_file)
            {
              new_files_list = thunar_g_list_prepend_deep (new_files_list,
                                                           real_target_file);

              /* notify the thumbnail cache that we need to copy the original
               * thumbnail for the symlink to have one too */
              thunar_thumbnail_cache_copy_file (thumbnail_cache, sp->data,
                                                real_target_file);

              /* remember the file for possible undo */
              if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
                thunar_job_operation_add (operation, sp->data, real_target_file);
            }

          /* release the real target file */
          g_object_unref (real_target_file);
        }
    }

  /* release the thumbnail cache */
  g_object_unref (thumbnail_cache);

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    {
      thunar_job_operation_history_commit (operation);
      g_object_unref (operation);
    }

  if (err != NULL)
    {
      thunar_g_list_free_full (new_files_list);
      g_propagate_error (error, err);
      return FALSE;
    }
  else
    {
      thunar_job_new_files (THUNAR_JOB (job), new_files_list);
      thunar_g_list_free_full (new_files_list);
      return TRUE;
    }
}



ThunarJob *
thunar_io_jobs_link_files (GList *source_file_list,
                           GList *target_file_list)
{
  _thunar_return_val_if_fail (source_file_list != NULL, NULL);
  _thunar_return_val_if_fail (target_file_list != NULL, NULL);
  _thunar_return_val_if_fail (g_list_length (source_file_list) == g_list_length (target_file_list), NULL);

  return thunar_simple_job_new (_thunar_io_jobs_link, 2,
                                THUNAR_TYPE_G_FILE_LIST, source_file_list,
                                THUNAR_TYPE_G_FILE_LIST, target_file_list);
}



static gboolean
_thunar_io_jobs_trash (ThunarJob *job,
                       GArray    *param_values,
                       GError   **error)
{
  ThunarThumbnailCache  *thumbnail_cache;
  ThunarApplication     *application;
  ThunarJobOperation    *operation = NULL;
  ThunarJobResponse      response;
  ThunarOperationLogMode log_mode;
  GError                *err = NULL;
  GList                 *file_list;
  GList                 *lp;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 1, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;

  /* take a reference on the thumbnail cache */
  application = thunar_application_get ();
  thumbnail_cache = thunar_application_get_thumbnail_cache (application);
  g_object_unref (application);

  log_mode = thunar_job_get_log_mode (job);

  if (log_mode != THUNAR_OPERATION_LOG_NO_OPERATIONS)
    operation = thunar_job_operation_new (THUNAR_JOB_OPERATION_KIND_TRASH);

  for (lp = file_list; err == NULL && lp != NULL; lp = lp->next)
    {
      _thunar_assert (G_IS_FILE (lp->data));

      /* trash the file or folder */
      g_file_trash (lp->data, thunar_job_get_cancellable (THUNAR_JOB (job)), &err);

      if (err != NULL)
        {
          response = thunar_job_ask_delete (job, "%s", err->message);

          g_clear_error (&err);

          if (response == THUNAR_JOB_RESPONSE_CANCEL)
            break;

          if (response == THUNAR_JOB_RESPONSE_YES)
            _tij_delete_file (lp->data, thunar_job_get_cancellable (THUNAR_JOB (job)), &err);
        }

      if (err == NULL && log_mode != THUNAR_OPERATION_LOG_NO_OPERATIONS)
        thunar_job_operation_add (operation, lp->data, NULL);

      /* update the thumbnail cache */
      thunar_thumbnail_cache_cleanup_file (thumbnail_cache, lp->data);
    }

  /* release the thumbnail cache */
  g_object_unref (thumbnail_cache);

  if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
    {
      thunar_job_operation_history_commit (operation);
      g_object_unref (operation);
    }
  else if (log_mode == THUNAR_OPERATION_LOG_ONLY_TIMESTAMPS)
    {
      /* only required for 'redo' operation, in order to update the timestamps of the original trash operation */
      thunar_job_operation_history_update_trash_timestamps (operation);
      g_object_unref (operation);
    }

  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }
  else
    {
      return TRUE;
    }
}



ThunarJob *
thunar_io_jobs_trash_files (GList *file_list)
{
  _thunar_return_val_if_fail (file_list != NULL, NULL);

  return thunar_simple_job_new (_thunar_io_jobs_trash, 1,
                                THUNAR_TYPE_G_FILE_LIST, file_list);
}



ThunarJob *
thunar_io_jobs_restore_files (GList *source_file_list,
                              GList *target_file_list)
{
  ThunarJob *job;

  _thunar_return_val_if_fail (source_file_list != NULL, NULL);
  _thunar_return_val_if_fail (target_file_list != NULL, NULL);
  _thunar_return_val_if_fail (g_list_length (source_file_list) == g_list_length (target_file_list), NULL);

  job = thunar_transfer_job_new (source_file_list, target_file_list,
                                 THUNAR_TRANSFER_JOB_MOVE);

  return job;
}



static gboolean
_thunar_io_jobs_chown (ThunarJob *job,
                       GArray    *param_values,
                       GError   **error)
{
  ThunarJobResponse response;
  const gchar      *message;
  GFileInfo        *info;
  gboolean          recursive;
  GError           *err = NULL;
  GList            *file_list;
  GList            *lp;
  gint              uid;
  gint              gid;
  guint             n_processed = 0;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 4, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));
  uid = g_value_get_int (&g_array_index (param_values, GValue, 1));
  gid = g_value_get_int (&g_array_index (param_values, GValue, 2));
  recursive = g_value_get_boolean (&g_array_index (param_values, GValue, 3));

  _thunar_assert ((uid >= 0 || gid >= 0) && !(uid >= 0 && gid >= 0));

  /* collect the files for the chown operation */
  if (recursive)
    file_list = _tij_collect_nofollow (job, file_list, FALSE, &err);
  else
    file_list = thunar_g_list_copy_deep (file_list);

  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }

  /* we know the total list of files to process */
  thunar_job_set_total_files (THUNAR_JOB (job), file_list);

  /* change the ownership of all files */
  for (lp = file_list; lp != NULL && err == NULL; lp = lp->next, n_processed++)
    {
      /* update progress information */
      thunar_job_processing_file (THUNAR_JOB (job), lp, n_processed);

      /* try to query information about the file */
      info = g_file_query_info (lp->data,
                                G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                thunar_job_get_cancellable (THUNAR_JOB (job)),
                                &err);

      if (err != NULL)
        break;

retry_chown:
      if (uid >= 0)
        {
          /* try to change the owner UID */
          g_file_set_attribute_uint32 (lp->data,
                                       G_FILE_ATTRIBUTE_UNIX_UID, uid,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       thunar_job_get_cancellable (THUNAR_JOB (job)),
                                       &err);
        }
      else if (gid >= 0)
        {
          /* try to change the owner GID */
          g_file_set_attribute_uint32 (lp->data,
                                       G_FILE_ATTRIBUTE_UNIX_GID, gid,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       thunar_job_get_cancellable (THUNAR_JOB (job)),
                                       &err);
        }

      /* check if there was a recoverable error */
      if (err != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job)))
        {
          /* generate a useful error message */
          message = G_LIKELY (uid >= 0) ? _("Failed to change the owner of \"%s\": %s")
                                        : _("Failed to change the group of \"%s\": %s");

          /* ask the user whether to skip/retry this file */
          response = thunar_job_ask_skip (THUNAR_JOB (job), message,
                                          g_file_info_get_display_name (info),
                                          err->message);

          /* clear the error */
          g_clear_error (&err);

          /* check whether to retry */
          if (response == THUNAR_JOB_RESPONSE_RETRY)
            goto retry_chown;
        }

      /* release file information */
      g_object_unref (info);
    }

  /* release the file list */
  thunar_g_list_free_full (file_list);

  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }
  else
    {
      return TRUE;
    }
}



ThunarJob *
thunar_io_jobs_change_group (GList   *files,
                             guint32  gid,
                             gboolean recursive)
{
  _thunar_return_val_if_fail (files != NULL, NULL);

  /* files are released when the list if destroyed */
  g_list_foreach (files, (GFunc) (void (*) (void)) g_object_ref, NULL);

  return thunar_simple_job_new (_thunar_io_jobs_chown, 4,
                                THUNAR_TYPE_G_FILE_LIST, files,
                                G_TYPE_INT, -1,
                                G_TYPE_INT, (gint) gid,
                                G_TYPE_BOOLEAN, recursive);
}



static gboolean
_thunar_io_jobs_chmod (ThunarJob *job,
                       GArray    *param_values,
                       GError   **error)
{
  ThunarJobResponse response;
  GFileInfo        *info;
  gboolean          recursive;
  GError           *err = NULL;
  GList            *file_list;
  GList            *lp;
  guint             n_processed = 0;
  ThunarFileMode    dir_mask;
  ThunarFileMode    dir_mode;
  ThunarFileMode    file_mask;
  ThunarFileMode    file_mode;
  ThunarFileMode    mask;
  ThunarFileMode    mode;
  ThunarFileMode    old_mode;
  ThunarFileMode    new_mode;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 6, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  file_list = g_value_get_boxed (&g_array_index (param_values, GValue, 0));
  dir_mask = g_value_get_flags (&g_array_index (param_values, GValue, 1));
  dir_mode = g_value_get_flags (&g_array_index (param_values, GValue, 2));
  file_mask = g_value_get_flags (&g_array_index (param_values, GValue, 3));
  file_mode = g_value_get_flags (&g_array_index (param_values, GValue, 4));
  recursive = g_value_get_boolean (&g_array_index (param_values, GValue, 5));

  /* collect the files for the chown operation */
  if (recursive)
    file_list = _tij_collect_nofollow (job, file_list, FALSE, &err);
  else
    file_list = thunar_g_list_copy_deep (file_list);

  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }

  /* we know the total list of files to process */
  thunar_job_set_total_files (THUNAR_JOB (job), file_list);

  /* change the ownership of all files */
  for (lp = file_list; lp != NULL && err == NULL; lp = lp->next, n_processed++)
    {
      /* update progress information */
      thunar_job_processing_file (THUNAR_JOB (job), lp, n_processed);

      /* try to query information about the file */
      info = g_file_query_info (lp->data,
                                G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_UNIX_MODE,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                thunar_job_get_cancellable (THUNAR_JOB (job)),
                                &err);

      if (err != NULL)
        break;

retry_chown:
      /* different actions depending on the type of the file */
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
          mask = dir_mask;
          mode = dir_mode;
        }
      else
        {
          mask = file_mask;
          mode = file_mode;
        }

      /* determine the current mode */
      old_mode = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);

      /* generate the new mode, taking the old mode (which contains file type
       * information) into account */
      new_mode = ((old_mode & ~mask) | mode) & 07777;

      if (old_mode != new_mode)
        {
          /* try to change the file mode */
          g_file_set_attribute_uint32 (lp->data,
                                       G_FILE_ATTRIBUTE_UNIX_MODE, new_mode,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       thunar_job_get_cancellable (THUNAR_JOB (job)),
                                       &err);
        }

      /* check if there was a recoverable error */
      if (err != NULL && !thunar_job_is_cancelled (THUNAR_JOB (job)))
        {
          /* ask the user whether to skip/retry this file */
          response = thunar_job_ask_skip (job,
                                          _("Failed to change the permissions of \"%s\": %s"),
                                          g_file_info_get_display_name (info),
                                          err->message);

          /* clear the error */
          g_clear_error (&err);

          /* check whether to retry */
          if (response == THUNAR_JOB_RESPONSE_RETRY)
            goto retry_chown;
        }

      /* release file information */
      g_object_unref (info);
    }

  /* release the file list */
  thunar_g_list_free_full (file_list);

  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }
  else
    {
      return TRUE;
    }
  return TRUE;
}



ThunarJob *
thunar_io_jobs_change_mode (GList         *files,
                            ThunarFileMode dir_mask,
                            ThunarFileMode dir_mode,
                            ThunarFileMode file_mask,
                            ThunarFileMode file_mode,
                            gboolean       recursive)
{
  _thunar_return_val_if_fail (files != NULL, NULL);

  /* files are released when the list if destroyed */
  g_list_foreach (files, (GFunc) (void (*) (void)) g_object_ref, NULL);

  return thunar_simple_job_new (_thunar_io_jobs_chmod, 6,
                                THUNAR_TYPE_G_FILE_LIST, files,
                                THUNAR_TYPE_FILE_MODE, dir_mask,
                                THUNAR_TYPE_FILE_MODE, dir_mode,
                                THUNAR_TYPE_FILE_MODE, file_mask,
                                THUNAR_TYPE_FILE_MODE, file_mode,
                                G_TYPE_BOOLEAN, recursive);
}



static gboolean
_thunar_io_jobs_ls (ThunarJob *job,
                    GArray    *param_values,
                    GError   **error)
{
  GError *err = NULL;
  GFile  *directory;
  GList  *file_list = NULL;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 1, FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;

  /* determine the directory to list */
  directory = g_value_get_object (&g_array_index (param_values, GValue, 0));

  /* make sure the object is valid */
  _thunar_assert (G_IS_FILE (directory));

  /* collect directory contents (non-recursively) */
  file_list = thunar_io_scan_directory (job, directory,
                                        G_FILE_QUERY_INFO_NONE,
                                        FALSE, FALSE, TRUE, NULL, &err);

  /* abort on errors or cancellation */
  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }
  else if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), &err))
    {
      g_propagate_error (error, err);
      return FALSE;
    }

  /* check if we have any files to report */
  if (G_LIKELY (file_list != NULL))
    {
      /* emit the "files-ready" signal */
      if (!thunar_job_files_ready (THUNAR_JOB (job), file_list))
        {
          /* none of the handlers took over the file list, so it's up to us
           * to destroy it */
          thunar_g_list_free_full (file_list);
        }
    }

  /* there should be no errors here */
  _thunar_assert (err == NULL);

  /* propagate cancellation error */
  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), &err))
    {
      g_propagate_error (error, err);
      return FALSE;
    }

  return TRUE;
}



ThunarJob *
thunar_io_jobs_list_directory (GFile *directory)
{
  _thunar_return_val_if_fail (G_IS_FILE (directory), NULL);

  return thunar_simple_job_new (_thunar_io_jobs_ls, 1, G_TYPE_FILE, directory);
}



static gboolean
_thunar_io_jobs_rename_notify (gpointer user_data)
{
  ThunarFile *file = user_data;

  /* tell the associated folder that the file was renamed */
  thunarx_file_info_renamed (THUNARX_FILE_INFO (file));

  /* emit the file changed signal */
  thunar_file_changed (file);

  return FALSE;
}



static gboolean
_thunar_io_jobs_rename (ThunarJob *job,
                        GArray    *param_values,
                        GError   **error)
{
  const gchar           *display_name;
  ThunarFile            *file;
  GError                *err = NULL;
  GFile                 *old_file;
  gchar                 *old_file_uri;
  ThunarOperationLogMode log_mode;
  ThunarJobOperation    *operation;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 3, FALSE);
  _thunar_return_val_if_fail (G_VALUE_HOLDS (&g_array_index (param_values, GValue, 0), THUNAR_TYPE_FILE), FALSE);
  _thunar_return_val_if_fail (G_VALUE_HOLDS_STRING (&g_array_index (param_values, GValue, 1)), FALSE);
  _thunar_return_val_if_fail (G_VALUE_HOLDS_ENUM (&g_array_index (param_values, GValue, 2)), FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;

  /* determine the file, display name and log mode */
  file = g_value_get_object (&g_array_index (param_values, GValue, 0));
  display_name = g_value_get_string (&g_array_index (param_values, GValue, 1));
  log_mode = g_value_get_enum (&g_array_index (param_values, GValue, 2));

  old_file_uri = g_file_get_uri (thunar_file_get_file (file));

  /* try to rename the file */
  if (thunar_file_rename (file, display_name, thunar_job_get_cancellable (THUNAR_JOB (job)), TRUE, &err))
    {
      thunar_job_send_to_mainloop (THUNAR_JOB (job),
                                   _thunar_io_jobs_rename_notify,
                                   g_object_ref (file), g_object_unref);

      if (log_mode == THUNAR_OPERATION_LOG_OPERATIONS)
        {
          operation = thunar_job_operation_new (THUNAR_JOB_OPERATION_KIND_RENAME);
          old_file = g_file_new_for_uri (old_file_uri);
          thunar_job_operation_add (operation, old_file, thunar_file_get_file (file));
          thunar_job_operation_history_commit (operation);
          g_object_unref (old_file);
          g_object_unref (operation);
        }
    }

  g_free (old_file_uri);

  /* abort on errors or cancellation */
  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }

  return TRUE;
}



ThunarJob *
thunar_io_jobs_rename_file (ThunarFile            *file,
                            const gchar           *display_name,
                            ThunarOperationLogMode log_mode)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (g_utf8_validate (display_name, -1, NULL), NULL);

  return thunar_simple_job_new (_thunar_io_jobs_rename, 3,
                                THUNAR_TYPE_FILE, file,
                                G_TYPE_STRING, display_name,
                                THUNAR_TYPE_OPERATION_LOG_MODE, log_mode);
}



static gboolean
_thunar_io_jobs_count (ThunarJob *job,
                       GArray    *param_values,
                       GError   **error)
{
  GError          *err = NULL;
  ThunarFile      *file;
  GFileEnumerator *enumerator;
  guint            count;

  _thunar_return_val_if_fail (THUNAR_IS_JOB (job), FALSE);
  _thunar_return_val_if_fail (param_values != NULL, FALSE);
  _thunar_return_val_if_fail (param_values->len == 1, FALSE);
  _thunar_return_val_if_fail (G_VALUE_HOLDS (&g_array_index (param_values, GValue, 0), THUNAR_TYPE_FILE), FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  file = THUNAR_FILE (g_value_get_object (&g_array_index (param_values, GValue, 0)));

  if (file == NULL)
    return FALSE;

  enumerator = g_file_enumerate_children (thunar_file_get_file (file), NULL,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, &err);
  if (err != NULL)
    {
      g_propagate_error (error, err);
      return FALSE;
    }

  count = 0;
  for (GFileInfo *child_info = g_file_enumerator_next_file (enumerator, NULL, &err);
       child_info != NULL;
       child_info = g_file_enumerator_next_file (enumerator, NULL, &err))
    {
      if (err != NULL)
        {
          g_propagate_error (error, err);
          return FALSE;
        }

      count++;
      g_object_unref (child_info);
    }

  thunar_file_set_file_count (file, count);

  g_object_unref (enumerator);

  return TRUE;
}



ThunarJob *
thunar_io_jobs_count_files (ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  return thunar_simple_job_new (_thunar_io_jobs_count, 1,
                                THUNAR_TYPE_FILE, file);
}



static void
_thunar_search_folder (ThunarStandardViewModel           *model,
                       ThunarJob                         *job,
                       gchar                             *uri,
                       gchar                            **search_query_c_terms,
                       enum ThunarStandardViewModelSearch search_type,
                       gboolean                           show_hidden)
{
  GCancellable    *cancellable;
  GFileEnumerator *enumerator;
  GFile           *directory;
  GList           *files_found = NULL; /* contains the matching files in this folder only */
  const gchar *namespace;
  const gchar *display_name;
  gchar       *display_name_c; /* converted to ignore case */

  cancellable = thunar_job_get_cancellable (THUNAR_JOB (job));
  directory = g_file_new_for_uri (uri);
  namespace = G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_TARGET_URI "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_NAME ", recent::*";

  /* The directory enumerator MUST NOT follow symlinks itself, meaning that any symlinks that
   * g_file_enumerator_next_file() emits are the actual symlink entries. This prevents one
   * possible source of infinitely deep recursion.
   *
   * There is otherwise no special handling of entries in the folder which are symlinks,
   * which allows them to appear in the search results. */
  enumerator = g_file_enumerate_children (directory, namespace, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
  if (enumerator == NULL)
    {
      g_object_unref (directory);
      return;
    }

  /* go through every file in the folder and check if it matches */
  while (thunar_job_is_cancelled (THUNAR_JOB (job)) == FALSE)
    {
      GFile     *file;
      GFileInfo *info;
      GFileType  type;

      /* get GFile and GFileInfo */
      info = g_file_enumerator_next_file (enumerator, cancellable, NULL);
      if (G_UNLIKELY (info == NULL))
        break;

      if (g_file_has_uri_scheme (directory, "recent"))
        {
          file = g_file_new_for_uri (g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI));
          g_object_unref (info);
          info = g_file_query_info (file, namespace, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
          if (G_UNLIKELY (info == NULL))
            {
              g_object_unref (file);
              break;
            }
        }
      else
        file = g_file_get_child (directory, g_file_info_get_name (info));

      /* respect last-show-hidden */
      if (show_hidden == FALSE)
        {
          /* same logic as thunar_file_is_hidden() */
          if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN)
              || g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP))
            {
              g_object_unref (file);
              g_object_unref (info);
              continue;
            }
        }

      type = g_file_info_get_file_type (info);

      /* handle directories */
      if (type == G_FILE_TYPE_DIRECTORY && search_type == THUNAR_STANDARD_VIEW_MODEL_SEARCH_RECURSIVE)
        {
          gchar *file_uri = g_file_get_uri (file);
          _thunar_search_folder (model, job, file_uri, search_query_c_terms, search_type, show_hidden);
          g_free (file_uri);
        }

      /* prepare entry display name */
      display_name = g_file_info_get_display_name (info);
      display_name_c = thunar_g_utf8_normalize_for_search (display_name, TRUE, TRUE);

      /* search for all substrings */
      if (thunar_util_search_terms_match (search_query_c_terms, display_name_c))
        files_found = g_list_prepend (files_found, thunar_file_get (file, NULL));

      /* free memory */
      g_free (display_name_c);
      g_object_unref (file);
      g_object_unref (info);
    }

  g_object_unref (enumerator);
  g_object_unref (directory);

  if (thunar_job_is_cancelled (THUNAR_JOB (job)))
    {
      thunar_g_list_free_full (files_found);
      return;
    }

  thunar_standard_view_model_add_search_files (model, files_found);
}



static gboolean
_thunar_job_search_directory (ThunarJob *job,
                              GArray    *param_values,
                              GError   **error)
{
  ThunarStandardViewModel           *model;
  ThunarFile                        *directory;
  const char                        *search_query_c;
  gchar                            **search_query_c_terms;
  gboolean                           is_source_device_local;
  ThunarRecursiveSearchMode          mode;
  gboolean                           show_hidden;
  enum ThunarStandardViewModelSearch search_type;
  gchar                             *directory_uri;

  search_type = THUNAR_STANDARD_VIEW_MODEL_SEARCH_NON_RECURSIVE;

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;

  model = g_value_get_object (&g_array_index (param_values, GValue, 0));
  search_query_c = g_value_get_string (&g_array_index (param_values, GValue, 1));
  directory = g_value_get_object (&g_array_index (param_values, GValue, 2));
  mode = g_value_get_enum (&g_array_index (param_values, GValue, 3));
  show_hidden = g_value_get_boolean (&g_array_index (param_values, GValue, 4));

  search_query_c_terms = thunar_util_split_search_query (search_query_c, error);
  if (search_query_c_terms == NULL)
    return FALSE;

  is_source_device_local = thunar_g_file_is_on_local_device (thunar_file_get_file (directory));
  if (mode == THUNAR_RECURSIVE_SEARCH_ALWAYS || (mode == THUNAR_RECURSIVE_SEARCH_LOCAL && is_source_device_local))
    search_type = THUNAR_STANDARD_VIEW_MODEL_SEARCH_RECURSIVE;

  directory_uri = thunar_file_dup_uri (directory);

  _thunar_search_folder (model, job, directory_uri, search_query_c_terms, search_type, show_hidden);

  g_free (directory_uri);
  g_strfreev (search_query_c_terms);

  return TRUE;
}



ThunarJob *
thunar_io_jobs_search_directory (ThunarStandardViewModel *model,
                                 const gchar             *search_query,
                                 ThunarFile              *directory)
{
  ThunarPreferences        *preferences;
  ThunarRecursiveSearchMode mode;
  gboolean                  show_hidden;

  preferences = thunar_preferences_get ();

  /* grab a reference of preferences determine the current recursive search mode */
  g_object_get (G_OBJECT (preferences), "misc-recursive-search", &mode, NULL);
  g_object_get (G_OBJECT (preferences), "last-show-hidden", &show_hidden, NULL);

  g_object_unref (preferences);
  return thunar_simple_job_new (_thunar_job_search_directory, 5,
                                THUNAR_TYPE_STANDARD_VIEW_MODEL, model,
                                G_TYPE_STRING, search_query,
                                THUNAR_TYPE_FILE, directory,
                                G_TYPE_ENUM, mode,
                                G_TYPE_BOOLEAN, show_hidden);
}



static gboolean
_thunar_io_jobs_clear_metadata_for_files (ThunarJob *job,
                                          GArray    *param_values,
                                          GError   **error)
{
  GList *gfiles, *infos;
  GList *setting_names;

  gfiles = g_value_get_pointer (&g_array_index (param_values, GValue, 0));
  infos = g_value_get_pointer (&g_array_index (param_values, GValue, 1));
  setting_names = g_value_get_pointer (&g_array_index (param_values, GValue, 2));

  for (GList *gfile = gfiles, *info = infos; gfile != NULL && info != NULL; gfile = gfile->next, info = info->next)
    for (GList *sn = setting_names; sn != NULL; sn = sn->next)
      thunar_g_file_clear_metadata_setting (gfile->data, info->data, sn->data);

  g_list_free_full (gfiles, g_object_unref);
  g_list_free_full (infos, g_object_unref);
  g_list_free_full (setting_names, g_free);

  return TRUE;
}



/**
 * thunar_io_jobs_clear_metadata_for_files:
 * @files: a #GList of #ThunarFiles
 * @first_metadata_setting_name: the setting name to set
 * @...: followed by the corresponding setting value and more
 *       setting_name/value pairs, finally ending with a %NULL
 *
 * Accepts a variable length metadata_setting_names to clear
 * for @files.
 *
 * Note: the reason for making this func variable length is that
 * by clearing multiple metadata at once we don't have to call
 * this func for each metadata setting. Individual calls would trigger
 * multiple threads which would then require a mutex logic to handle
 * callback.
 **/
ThunarJob *
thunar_io_jobs_clear_metadata_for_files (GList *files,
                                         ...)
{
  va_list list;
  GList  *setting_names = NULL;
  gchar  *str;
  GList  *gfiles = NULL, *infos = NULL;

  va_start (list, files);
  str = va_arg (list, gchar *);
  while (str != NULL)
    {
      setting_names = g_list_prepend (setting_names, g_strdup (str));
      str = va_arg (list, gchar *);
    }
  va_end (list);

  for (GList *lp = files; lp != NULL; lp = lp->next)
    {
      gfiles = g_list_prepend (gfiles, g_object_ref (thunar_file_get_file (THUNAR_FILE (lp->data))));
      infos = g_list_prepend (infos, g_object_ref (thunar_file_get_info (THUNAR_FILE (lp->data))));
    }
  return thunar_simple_job_new (_thunar_io_jobs_clear_metadata_for_files, 3,
                                G_TYPE_POINTER, gfiles,
                                G_TYPE_POINTER, infos,
                                G_TYPE_POINTER, setting_names);
}



static gboolean
_thunar_io_jobs_set_metadata_for_files (ThunarJob *job,
                                        GArray    *param_values,
                                        GError   **error)
{
  GList      *gfiles, *infos;
  ThunarGType type;
  GList      *setting_value_pairs;

  gfiles = g_value_get_pointer (&g_array_index (param_values, GValue, 0));
  infos = g_value_get_pointer (&g_array_index (param_values, GValue, 1));
  type = g_value_get_int (&g_array_index (param_values, GValue, 2));
  setting_value_pairs = g_value_get_pointer (&g_array_index (param_values, GValue, 3));

  for (GList *gfile = gfiles, *info = infos; gfile != NULL && info != NULL; gfile = gfile->next, info = info->next)
    for (GList *sn = setting_value_pairs; sn != NULL; sn = sn->next)
      thunar_g_file_set_metadata_setting (gfile->data, info->data, type,
                                          ((gchar **) sn->data)[0],
                                          ((gchar **) sn->data)[1], FALSE);

  g_list_free_full (gfiles, g_object_unref);
  g_list_free_full (infos, g_object_unref);
  g_list_free_full (setting_value_pairs, (GDestroyNotify) g_strfreev);

  return TRUE;
}



/**
 * thunar_io_jobs_set_metadata_for_files:
 * @files: a #GList of #ThunarFiles
 * @type: #ThunarGType of the metadata
 * @first_metadata_setting_name: the setting name to set
 * @...: followed by the corresponding setting value and more
 *       setting_name/value pairs, finally ending with a %NULL
 *
 * Accepts a variable length metadata setting_name
 * & setting_value pairs to set for @files.
 *
 * Note: the reason for making this func variable length is that
 * by clearing multiple metadata at once we don't have to call
 * this func for each metadata setting. Individual calls would trigger
 * multiple threads which would then require a mutex logic to handle
 * callback.
 **/
ThunarJob *
thunar_io_jobs_set_metadata_for_files (GList      *files,
                                       ThunarGType type,
                                       ...)
{
  va_list list;
  GList  *setting_value_pairs = NULL;
  gchar  *str, *val;
  gchar **sn_val_pair;
  GList  *gfiles = NULL, *infos = NULL;

  /* this func accepts a variable length setting_name, setting_value pair*/
  va_start (list, type);
  str = va_arg (list, gchar *);
  while (str != NULL)
    {
      val = va_arg (list, gchar *);
      if (val == NULL)
        break;
      // Needs to be null terminated for use in g_strfreev
      sn_val_pair = (gchar **) g_malloc0 (sizeof (gchar *) * 3);
      sn_val_pair[0] = g_strdup (str);
      sn_val_pair[1] = g_strdup (val);
      sn_val_pair[2] = NULL;
      setting_value_pairs = g_list_prepend (setting_value_pairs, sn_val_pair);
      str = va_arg (list, gchar *);
    }
  va_end (list);

  for (GList *lp = files; lp != NULL; lp = lp->next)
    {
      gfiles = g_list_prepend (gfiles, g_object_ref (thunar_file_get_file (THUNAR_FILE (lp->data))));
      infos = g_list_prepend (infos, g_object_ref (thunar_file_get_info (THUNAR_FILE (lp->data))));
    }

  return thunar_simple_job_new (_thunar_io_jobs_set_metadata_for_files, 4,
                                G_TYPE_POINTER, gfiles,
                                G_TYPE_POINTER, infos,
                                G_TYPE_INT, type,
                                G_TYPE_POINTER, setting_value_pairs);
}



static gboolean
_thunar_job_load_content_types (ThunarJob *job,
                                GArray    *param_values,
                                GError   **error)
{
  GHashTable    *g_files;
  gpointer       g_file;
  GHashTableIter iter;

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;

  g_files = g_value_get_boxed (&g_array_index (param_values, GValue, 0));

  g_hash_table_iter_init (&iter, g_files);
  while (g_hash_table_iter_next (&iter, &g_file, NULL))
    {
      gchar *content_type;

      content_type = thunar_g_file_get_content_type (G_FILE (g_file));
      g_object_set_data_full (G_OBJECT (g_file), "content-type", content_type, g_free);
    }

  return TRUE;
}



static void
thunar_io_jobs_load_content_types_finished (ThunarJob  *job,
                                            GHashTable *g_files)
{
  GHashTableIter iter;
  gpointer       g_file;

  g_hash_table_iter_init (&iter, g_files);
  while (g_hash_table_iter_next (&iter, &g_file, NULL))
    {
      ThunarFile *thunar_file;
      gchar      *content_type;

      thunar_file = thunar_file_get (G_FILE (g_file), NULL);
      if (thunar_file != NULL)
        {
          content_type = g_object_get_data (G_OBJECT (g_file), "content-type");
          if (content_type != NULL)
            thunar_file_set_content_type (thunar_file, content_type);
          g_object_unref (thunar_file);
        }
    }

  g_hash_table_destroy (g_files);
}



/**
 * thunar_io_jobs_load_content_types:
 * @files: a #GList of #ThunarFiles
 *
 * Loads the content types of the passed #ThunarFiles in a separate thread.
 * After loaded, 'thunar_file_set_content_type' is called for each #ThunarFile.
 *
 * Returns: (transfer none): the #ThunarJob which manages the separate thread
 **/
ThunarJob *
thunar_io_jobs_load_content_types (GHashTable *files)
{
  GHashTable    *g_files = g_hash_table_new_full (g_direct_hash, NULL, g_object_unref, NULL);
  GHashTableIter iter;
  gpointer       key;

  g_hash_table_iter_init (&iter, files);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_hash_table_add (g_files, g_object_ref (thunar_file_get_file (THUNAR_FILE (key))));

  ThunarJob *job = thunar_simple_job_new (_thunar_job_load_content_types, 1,
                                          THUNAR_TYPE_G_FILE_HASH_TABLE, g_files);

  g_signal_connect (job, "finished", G_CALLBACK (thunar_io_jobs_load_content_types_finished), g_files);

  return job;
}



static gboolean
_thunar_job_load_statusbar_text (ThunarJob *job,
                                 GArray    *param_values,
                                 GError   **error)
{
  ThunarStandardView *standard_view;
  ThunarFile         *thunar_folder;
  GHashTable         *g_files;
  gboolean            show_hidden;
  gboolean            show_file_size_binary_format;
  ThunarDateStyle     date_style;
  const gchar        *date_custom_style;
  gchar              *text_for_files;
  gchar              *temp_string;
  GList              *text_list = NULL;
  guint               status_bar_active_info;
  guint64             size;

  if (thunar_job_set_error_if_cancelled (THUNAR_JOB (job), error))
    return FALSE;

  standard_view = g_value_get_object (&g_array_index (param_values, GValue, 0));
  thunar_folder = g_value_get_object (&g_array_index (param_values, GValue, 1));
  g_files = g_value_get_boxed (&g_array_index (param_values, GValue, 2));
  show_hidden = g_value_get_boolean (&g_array_index (param_values, GValue, 3));
  show_file_size_binary_format = g_value_get_boolean (&g_array_index (param_values, GValue, 4));
  date_style = g_value_get_enum (&g_array_index (param_values, GValue, 5));
  date_custom_style = g_value_get_string (&g_array_index (param_values, GValue, 6));
  status_bar_active_info = g_value_get_uint (&g_array_index (param_values, GValue, 7));

  text_for_files = thunar_util_get_statusbar_text_for_files (g_files,
                                                             show_hidden,
                                                             show_file_size_binary_format,
                                                             date_style,
                                                             date_custom_style,
                                                             status_bar_active_info);

  /* the text is about a selection of files or about a folder */
  if (thunar_folder == 0)
    {
      temp_string = g_strdup_printf (_ ("Selection: %s"), text_for_files);
      text_list = g_list_append (text_list, temp_string);
      g_free (text_for_files);
    }
  else
    {
      text_list = g_list_append (text_list, text_for_files);

      /* check if we can determine the amount of free space for the volume */
      if (thunar_g_file_get_free_space (thunar_file_get_file (thunar_folder), &size, NULL))
        {
          /* humanize the free space */
          gchar *size_string = g_format_size_full (size, show_file_size_binary_format ? G_FORMAT_SIZE_IEC_UNITS : G_FORMAT_SIZE_DEFAULT);
          temp_string = g_strdup_printf (_ ("Free space: %s"), size_string);
          text_list = g_list_append (text_list, temp_string);
          g_free (size_string);
        }
    }

  temp_string = thunar_util_strjoin_list (text_list, "  |  ");
  g_list_free_full (text_list, g_free);

  thunar_standard_view_set_statusbar_text (standard_view, temp_string);
  g_free (temp_string);

  return TRUE;
}



ThunarJob *
thunar_io_jobs_load_statusbar_text_for_folder (ThunarStandardView *standard_view,
                                               ThunarFolder       *folder)
{
  ThunarPreferences *preferences;
  gboolean           show_hidden;
  gboolean           show_file_size_binary_format;
  ThunarDateStyle    date_style;
  gchar             *date_custom_style;
  guint              status_bar_active_info;
  GHashTable        *thunar_files;
  GHashTable        *g_files = g_hash_table_new (g_direct_hash, NULL);
  GHashTableIter     iter;
  gpointer           key;
  ThunarFile        *file;

  preferences = thunar_preferences_get ();
  g_object_get (G_OBJECT (preferences), "last-show-hidden", &show_hidden,
                "misc-date-style", &date_style,
                "misc-date-custom-style", &date_custom_style,
                "misc-file-size-binary", &show_file_size_binary_format,
                "misc-status-bar-active-info", &status_bar_active_info, NULL);

  thunar_files = thunar_folder_get_files (folder);

  g_hash_table_iter_init (&iter, thunar_files);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_hash_table_add (g_files, thunar_file_get_file (THUNAR_FILE (key)));

  file = thunar_folder_get_corresponding_file (folder);

  if (file == NULL)
    {
      g_free (date_custom_style);
      return NULL;
    }

  ThunarJob *job = thunar_simple_job_new (_thunar_job_load_statusbar_text, 8,
                                          THUNAR_TYPE_STANDARD_VIEW, g_object_ref (standard_view),
                                          THUNAR_TYPE_FILE, g_object_ref (file),
                                          THUNAR_TYPE_G_FILE_HASH_TABLE, g_files,
                                          G_TYPE_BOOLEAN, show_hidden,
                                          G_TYPE_BOOLEAN, show_file_size_binary_format,
                                          THUNAR_TYPE_DATE_STYLE, date_style,
                                          G_TYPE_STRING, date_custom_style,
                                          G_TYPE_UINT, status_bar_active_info);
  g_free (date_custom_style);
  g_hash_table_destroy (g_files);

  g_signal_connect_swapped (job, "finished", G_CALLBACK (g_object_unref), file);
  g_signal_connect_swapped (job, "finished", G_CALLBACK (g_object_unref), standard_view);
  return job;
}



ThunarJob *
thunar_io_jobs_load_statusbar_text_for_selection (ThunarStandardView *standard_view, GHashTable *selected_g_files)
{
  ThunarPreferences *preferences;
  gboolean           show_hidden;
  gboolean           show_file_size_binary_format;
  ThunarDateStyle    date_style;
  gchar             *date_custom_style;
  guint              status_bar_active_info;

  preferences = thunar_preferences_get ();
  g_object_get (G_OBJECT (preferences), "last-show-hidden", &show_hidden,
                "misc-date-style", &date_style,
                "misc-date-custom-style", &date_custom_style,
                "misc-file-size-binary", &show_file_size_binary_format,
                "misc-status-bar-active-info", &status_bar_active_info, NULL);

  ThunarJob *job = thunar_simple_job_new (_thunar_job_load_statusbar_text, 8,
                                          THUNAR_TYPE_STANDARD_VIEW, g_object_ref (standard_view),
                                          THUNAR_TYPE_FILE, NULL,
                                          THUNAR_TYPE_G_FILE_HASH_TABLE, selected_g_files,
                                          G_TYPE_BOOLEAN, show_hidden,
                                          G_TYPE_BOOLEAN, show_file_size_binary_format,
                                          THUNAR_TYPE_DATE_STYLE, date_style,
                                          G_TYPE_STRING, date_custom_style,
                                          G_TYPE_UINT, status_bar_active_info);
  g_free (date_custom_style);

  g_signal_connect_swapped (job, "finished", G_CALLBACK (g_object_unref), standard_view);
  return job;
}
