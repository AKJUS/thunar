/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2005-2007 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2009-2011 Jannis Pohlmann <jannis@xfce.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "thunar/thunar-application.h"
#include "thunar/thunar-chooser-dialog.h"
#include "thunar/thunar-dialogs.h"
#include "thunar/thunar-file.h"
#include "thunar/thunar-gio-extensions.h"
#include "thunar/thunar-gobject-extensions.h"
#include "thunar/thunar-icon-factory.h"
#include "thunar/thunar-io-jobs.h"
#include "thunar/thunar-preferences.h"
#include "thunar/thunar-private.h"
#include "thunar/thunar-thumbnailer.h"
#include "thunar/thunar-user.h"
#include "thunar/thunar-util.h"
#include "thunarx/thunarx.h"

#include <gio/gio.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>



/* Dump the file cache every X second, set to 0 to disable */
#define DUMP_FILE_CACHE 0

/* Minimum delay between two 'changed' signals of the same file */
#define FILE_CHANGED_SIGNAL_RATE_LIMIT 100 /* in milliseconds */

/* Signal identifiers */
/* Note that the signals 'CHANGED' and 'RENAMED' are provided by THUNARX_FILE_INFO */
enum
{
  DESTROY,
  THUMBNAIL_UPDATED,
  LAST_SIGNAL,
};



static void
thunar_file_info_init (ThunarxFileInfoIface *iface);
static void
thunar_file_dispose (GObject *object);
static void
thunar_file_finalize (GObject *object);
static gchar *
thunar_file_info_get_name (ThunarxFileInfo *file_info);
static gchar *
thunar_file_info_get_uri (ThunarxFileInfo *file_info);
static gchar *
thunar_file_info_get_parent_uri (ThunarxFileInfo *file_info);
static gchar *
thunar_file_info_get_uri_scheme (ThunarxFileInfo *file_info);
static gchar *
thunar_file_info_get_mime_type (ThunarxFileInfo *file_info);
static gboolean
thunar_file_info_has_mime_type (ThunarxFileInfo *file_info,
                                const gchar     *mime_type);
static gboolean
thunar_file_info_is_directory (ThunarxFileInfo *file_info);
static GFileInfo *
thunar_file_info_get_file_info (ThunarxFileInfo *file_info);
static GFileInfo *
thunar_file_info_get_filesystem_info (ThunarxFileInfo *file_info);
static GFile *
thunar_file_info_get_location (ThunarxFileInfo *file_info);
static void
thunar_file_info_changed (ThunarxFileInfo *file_info);
static gboolean
thunar_file_denies_access_permission (const ThunarFile *file,
                                      ThunarFileMode    usr_permissions,
                                      ThunarFileMode    grp_permissions,
                                      ThunarFileMode    oth_permissions);
static void
thunar_file_monitor (GFileMonitor     *monitor,
                     GFile            *path,
                     GFile            *other_path,
                     GFileMonitorEvent event_type,
                     gpointer          user_data);
static void
thunar_file_watch_reconnect (ThunarFile *file);
static gboolean
thunar_file_load (ThunarFile   *file,
                  GCancellable *cancellable,
                  GError      **error);
static gboolean
thunar_file_is_readable (const ThunarFile *file);
static gboolean
thunar_file_same_filesystem (const ThunarFile *file_a,
                             const ThunarFile *file_b);
static void
thunar_file_load_content_type (ThunarFile *file);
static void
thunar_file_thumbnailing_finished (ThunarFile        *file,
                                   guint              request_id,
                                   ThunarThumbnailer *thumbnailer);
static void
thunar_file_reset_thumbnail (ThunarFile         *file,
                             ThunarThumbnailSize size);



static GRecMutex G_LOCK_NAME (file_cache_mutex);

/* In order to limit the number of total file watches */
/* Note that a global, system-wide limit is defined in '/proc/sys/fs/inotify/max_user_watches' */
#define THUNAR_FILE_WATCH_MAX 10000
static gint thunar_file_watch_total_count = 0;

#define G_REC_LOCK(name) g_rec_mutex_lock (&G_LOCK_NAME (name))
#define G_REC_UNLOCK(name) g_rec_mutex_unlock (&G_LOCK_NAME (name))



static ThunarUserManager *user_manager;
static GHashTable        *file_cache;
static guint32            effective_user_id;
static GQuark             thunar_file_watch_quark;
static guint              file_signals[LAST_SIGNAL];



#define FLAG_SET(file, flag) \
  G_STMT_START { ((file)->flags |= (flag)); } \
  G_STMT_END
#define FLAG_UNSET(file, flag) \
  G_STMT_START { ((file)->flags &= ~(flag)); } \
  G_STMT_END
#define FLAG_IS_SET(file, flag) (((file)->flags & (flag)) != 0)
#define UNKNOWN_FILE_NAME "<UNKNOWN>"


typedef enum
{
  THUNAR_FILE_FLAG_THUMB_MASK = 0x03,       /* storage for ThunarFileThumbState */
  THUNAR_FILE_FLAG_IN_DESTRUCTION = 1 << 2, /* for avoiding recursion during destroy */
  THUNAR_FILE_FLAG_IS_MOUNTED = 1 << 3,     /* whether this file is mounted */
} ThunarFileFlags;

struct _ThunarFileClass
{
  GObjectClass __parent__;

  /* signals */
  void (*destroy) (ThunarFile *file);
  void (*thumbnail_updated) (ThunarFile         *file,
                             ThunarThumbnailSize size);
};

struct _ThunarFile
{
  GObject __parent__;

  /* storage for the file information */
  GFileInfo *info;
  GFileInfo *recent_info;
  GFileType  kind;
  GFile     *gfile;

  gchar *content_type;

  gchar *icon_name;

  gchar               *custom_icon_name;
  gchar               *display_name;
  gchar               *basename;
  const gchar         *device_type;
  gboolean             is_thumbnail;
  gchar               *thumbnail_path[N_THUMBNAIL_SIZES];
  ThunarFileThumbState thumbnail_state[N_THUMBNAIL_SIZES];
  guint                thumbnail_request_id[N_THUMBNAIL_SIZES];
  gulong               thumbnail_finished_handler_id;

  ThunarThumbnailer *thumbnailer;

  /* sorting */
  gchar *collate_key;
  gchar *collate_key_nocase;

  /* flags for thumbnail state etc */
  ThunarFileFlags flags;

  /* event source id and variable to rate-limit file-changed signals */
  guint    signal_changed_source_id;
  gboolean signal_change_requested;

  /* Number of files in this directory (only used if this #Thunarfile is a directory) */
  /* Note that this feature was added into #ThunarFile on purpose, because having inside #ThunarFolder caused lag when
   * there were > 10.000 files in a folder (Creation of #ThunarFolder seems to be slow) */
  guint   file_count;
  guint64 file_count_timestamp;
};

typedef struct
{
  GFileMonitor *monitor;
  guint         watch_count;
} ThunarFileWatch;

typedef struct
{
  ThunarFileGetFunc func;
  gpointer          user_data;
  GCancellable     *cancellable;
} ThunarFileGetData;

static struct
{
  GUserDirectory type;
  const gchar   *icon_name;
  const gchar   *xdg_name;
} thunar_file_dirs[] = {
  { G_USER_DIRECTORY_DESKTOP, "user-desktop", "DESKTOP" },
  { G_USER_DIRECTORY_DOCUMENTS, "folder-documents", "DOCUMENTS" },
  { G_USER_DIRECTORY_DOWNLOAD, "folder-download", "DOWNLOAD" },
  { G_USER_DIRECTORY_MUSIC, "folder-music", "MUSIC" },
  { G_USER_DIRECTORY_PICTURES, "folder-pictures", "PICTURES" },
  { G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare", "PUBLICSHARE" },
  { G_USER_DIRECTORY_TEMPLATES, "folder-templates", "TEMPLATES" },
  { G_USER_DIRECTORY_VIDEOS, "folder-videos", "VIDEOS" }
};



G_DEFINE_TYPE_WITH_CODE (ThunarFile, thunar_file, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (THUNARX_TYPE_FILE_INFO, thunar_file_info_init))


static GWeakRef *
weak_ref_new (GObject *obj)
{
  GWeakRef *ref;
  ref = g_slice_new (GWeakRef);
  g_weak_ref_init (ref, obj);
  return ref;
}

static void
weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_slice_free (GWeakRef, ref);
}


#ifdef G_ENABLE_DEBUG
#ifdef HAVE_ATEXIT
static gboolean thunar_file_atexit_registered = FALSE;



static void
thunar_file_atexit_foreach (gpointer key,
                            gpointer value,
                            gpointer user_data)
{
  gchar *uri;

  uri = g_file_get_uri (key);
  g_print ("--> %s\n", uri);
  if (G_OBJECT (key)->ref_count > 2)
    g_print ("    GFile (%u)\n", G_OBJECT (key)->ref_count - 2);
  g_free (uri);
}



static void
thunar_file_atexit (void)
{
  G_REC_LOCK (file_cache_mutex);

  if (file_cache == NULL || g_hash_table_size (file_cache) == 0)
    {
      G_REC_UNLOCK (file_cache_mutex);
      return;
    }

  g_print ("--- Leaked a total of %u ThunarFile objects:\n",
           g_hash_table_size (file_cache));

  g_hash_table_foreach (file_cache, thunar_file_atexit_foreach, NULL);

  g_print ("\n");

  G_REC_UNLOCK (file_cache_mutex);
}
#endif
#endif



#if DUMP_FILE_CACHE
static void
thunar_file_cache_dump_foreach (gpointer gfile,
                                gpointer value,
                                gpointer user_data)
{
  gchar *name;

  name = g_file_get_parse_name (G_FILE (gfile));
  g_print ("    %s\n", name);
  g_free (name);
}



static gboolean
thunar_file_cache_dump (gpointer user_data)
{
  G_REC_LOCK (file_cache_mutex);

  if (file_cache != NULL)
    {
      g_print ("--- %d ThunarFile objects in cache:\n",
               g_hash_table_size (file_cache));

      g_hash_table_foreach (file_cache, thunar_file_cache_dump_foreach, NULL);

      g_print ("\n");
    }

  G_REC_UNLOCK (file_cache_mutex);

  return TRUE;
}
#endif



static void
thunar_file_class_init (ThunarFileClass *klass)
{
  GObjectClass *gobject_class;

#ifdef G_ENABLE_DEBUG
#ifdef HAVE_ATEXIT
  if (G_UNLIKELY (!thunar_file_atexit_registered))
    {
      atexit ((void (*) (void)) thunar_file_atexit);
      thunar_file_atexit_registered = TRUE;
    }
#endif
#endif

#if DUMP_FILE_CACHE
  g_timeout_add_seconds (DUMP_FILE_CACHE, thunar_file_cache_dump, NULL);
#endif

  /* pre-allocate the required quarks */
  thunar_file_watch_quark = g_quark_from_static_string ("thunar-file-watch");

  /* grab a reference on the user manager */
  user_manager = thunar_user_manager_get_default ();

  /* determine the effective user id of the process */
  effective_user_id = geteuid ();

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = thunar_file_dispose;
  gobject_class->finalize = thunar_file_finalize;

  /**
   * ThunarFile::destroy:
   * @file : the #ThunarFile instance.
   *
   * Emitted when @file is destroyed.
   **/
  file_signals[DESTROY] =
  g_signal_new (I_ ("destroy"),
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_CLEANUP | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                G_STRUCT_OFFSET (ThunarFileClass, destroy),
                NULL, NULL,
                g_cclosure_marshal_VOID__VOID,
                G_TYPE_NONE, 0);

  /**
   * ThunarFile::thumbnail-updated:
   * @file : the #ThunarFile instance.
   * @size : The #ThunarThumbnailSize for which the thumbnail was updated
   *
   * Emitted when the file got a new thumbnail, or the thumbnail was removed
   **/
  file_signals[THUMBNAIL_UPDATED] =
  g_signal_new (I_ ("thumbnail-updated"),
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_NO_HOOKS, 0,
                NULL, NULL,
                g_cclosure_marshal_generic,
                G_TYPE_NONE, 1, G_TYPE_INT);
}



static void
thunar_file_init (ThunarFile *file)
{
  file->file_count = 0;
  file->file_count_timestamp = 0;
  file->display_name = NULL;
  file->is_thumbnail = FALSE;
  for (gint i = 0; i < N_THUMBNAIL_SIZES; i++)
    thunar_file_reset_thumbnail (file, i);

  file->thumbnailer = thunar_thumbnailer_get ();

  file->thumbnail_finished_handler_id = g_signal_connect_swapped (file->thumbnailer, "request-finished", G_CALLBACK (thunar_file_thumbnailing_finished), file);
}



static void
thunar_file_info_init (ThunarxFileInfoIface *iface)
{
  iface->get_name = thunar_file_info_get_name;
  iface->get_uri = thunar_file_info_get_uri;
  iface->get_parent_uri = thunar_file_info_get_parent_uri;
  iface->get_uri_scheme = thunar_file_info_get_uri_scheme;
  iface->get_mime_type = thunar_file_info_get_mime_type;
  iface->has_mime_type = thunar_file_info_has_mime_type;
  iface->is_directory = thunar_file_info_is_directory;
  iface->get_file_info = thunar_file_info_get_file_info;
  iface->get_filesystem_info = thunar_file_info_get_filesystem_info;
  iface->get_location = thunar_file_info_get_location;
  iface->changed = thunar_file_info_changed;
}



static void
thunar_file_dispose (GObject *object)
{
  ThunarFile *file = THUNAR_FILE (object);

  /* check that we don't recurse here */
  if (!FLAG_IS_SET (file, THUNAR_FILE_FLAG_IN_DESTRUCTION))
    {
      /* emit the "destroy" signal */
      FLAG_SET (file, THUNAR_FILE_FLAG_IN_DESTRUCTION);
      g_signal_emit (object, file_signals[DESTROY], 0);
      FLAG_UNSET (file, THUNAR_FILE_FLAG_IN_DESTRUCTION);
    }

  (*G_OBJECT_CLASS (thunar_file_parent_class)->dispose) (object);
}



static void
thunar_file_finalize (GObject *object)
{
  ThunarFile *file = THUNAR_FILE (object);

  if (file->signal_changed_source_id != 0)
    g_source_remove (file->signal_changed_source_id);

    /* verify that nobody's watching the file anymore */
#ifdef G_ENABLE_DEBUG
  ThunarFileWatch *file_watch = g_object_get_qdata (G_OBJECT (file), thunar_file_watch_quark);
  if (file_watch != NULL)
    {
      g_error ("Attempt to finalize a ThunarFile, which has an active "
               "watch count of %d",
               file_watch->watch_count);
    }
#endif

  g_signal_handler_disconnect (G_OBJECT (file->thumbnailer), file->thumbnail_finished_handler_id);

  if (file->thumbnailer != NULL)
    g_object_unref (file->thumbnailer);

  /* drop the entry from the cache */
  G_REC_LOCK (file_cache_mutex);
  g_hash_table_remove (file_cache, file->gfile);
  G_REC_UNLOCK (file_cache_mutex);

  /* release file info */
  if (file->info != NULL)
    g_object_unref (file->info);

  /* release file info */
  if (file->recent_info != NULL)
    g_object_unref (file->recent_info);

  /* free the custom icon name */
  g_free (file->custom_icon_name);

  /* content type info */
  g_free (file->content_type);

  g_free (file->icon_name);

  /* free display name and basename */
  g_free (file->display_name);
  g_free (file->basename);

  /* free collate keys */
  if (file->collate_key_nocase != file->collate_key)
    g_free (file->collate_key_nocase);
  g_free (file->collate_key);

  /* free the thumbnail path */
  for (gint i = 0; i < N_THUMBNAIL_SIZES; i++)
    {
      g_free (file->thumbnail_path[i]);
      file->thumbnail_path[i] = NULL;
    }

  /* release file */
  g_object_unref (file->gfile);

  (*G_OBJECT_CLASS (thunar_file_parent_class)->finalize) (object);
}



static gchar *
thunar_file_info_get_name (ThunarxFileInfo *file_info)
{
  return g_strdup (thunar_file_get_basename (THUNAR_FILE (file_info)));
}



static gchar *
thunar_file_info_get_uri (ThunarxFileInfo *file_info)
{
  return thunar_file_dup_uri (THUNAR_FILE (file_info));
}



static gchar *
thunar_file_info_get_parent_uri (ThunarxFileInfo *file_info)
{
  GFile *parent;
  gchar *uri = NULL;

  parent = g_file_get_parent (THUNAR_FILE (file_info)->gfile);
  if (G_LIKELY (parent != NULL))
    {
      uri = g_file_get_uri (parent);
      g_object_unref (parent);
    }

  return uri;
}



static gchar *
thunar_file_info_get_uri_scheme (ThunarxFileInfo *file_info)
{
  return g_file_get_uri_scheme (THUNAR_FILE (file_info)->gfile);
}



static gchar *
thunar_file_info_get_mime_type (ThunarxFileInfo *file_info)
{
  return g_strdup (thunar_file_get_content_type (THUNAR_FILE (file_info)));
}



static gboolean
thunar_file_info_has_mime_type (ThunarxFileInfo *file_info,
                                const gchar     *mime_type)
{
  if (THUNAR_FILE (file_info)->info == NULL)
    return FALSE;

  return g_content_type_is_a (thunar_file_get_content_type (THUNAR_FILE (file_info)), mime_type);
}



static gboolean
thunar_file_info_is_directory (ThunarxFileInfo *file_info)
{
  return thunar_file_is_directory (THUNAR_FILE (file_info));
}



static GFileInfo *
thunar_file_info_get_file_info (ThunarxFileInfo *file_info)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_info), NULL);

  if (THUNAR_FILE (file_info)->info != NULL)
    return g_object_ref (THUNAR_FILE (file_info)->info);
  else
    return NULL;
}



static GFileInfo *
thunar_file_info_get_filesystem_info (ThunarxFileInfo *file_info)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_info), NULL);

  return g_file_query_filesystem_info (THUNAR_FILE (file_info)->gfile,
                                       THUNARX_FILESYSTEM_INFO_NAMESPACE,
                                       NULL, NULL);
}



static GFile *
thunar_file_info_get_location (ThunarxFileInfo *file_info)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_info), NULL);
  return g_object_ref (THUNAR_FILE (file_info)->gfile);
}



static void
thunar_file_info_changed (ThunarxFileInfo *file_info)
{
  ThunarFile *file = THUNAR_FILE (file_info);

  _thunar_return_if_fail (THUNAR_IS_FILE (file_info));

  /* set the new thumbnail state manually, so we only emit file
   * changed once */
  for (gint i = 0; i < N_THUMBNAIL_SIZES; i++)
    thunar_file_reset_thumbnail (file, i);
}



static gboolean
thunar_file_denies_access_permission (const ThunarFile *file,
                                      ThunarFileMode    usr_permissions,
                                      ThunarFileMode    grp_permissions,
                                      ThunarFileMode    oth_permissions)
{
  ThunarFileMode mode;
  ThunarGroup   *group;
  ThunarUser    *user;
  gboolean       result;
  GList         *groups;
  GList         *lp;

  /* query the file mode */
  mode = thunar_file_get_mode (file);

  /* query the owner of the file, if we cannot determine
   * the owner, we can't tell if we're denied to access
   * the file, so we simply return FALSE then.
   */
  user = thunar_file_get_user (file);
  if (G_UNLIKELY (user == NULL))
    return FALSE;

  /* root is allowed to do everything */
  if (G_UNLIKELY (effective_user_id == 0))
    return FALSE;

  if (thunar_user_is_me (user))
    {
      /* we're the owner, so the usr permissions must be granted */
      result = ((mode & usr_permissions) == 0);

      /* release the user */
      g_object_unref (G_OBJECT (user));
    }
  else
    {
      group = thunar_file_get_group (file);
      if (G_LIKELY (group != NULL))
        {
          /* release the file owner */
          g_object_unref (G_OBJECT (user));

          /* determine the effective user */
          user = thunar_user_manager_get_user_by_id (user_manager, effective_user_id);
          if (G_LIKELY (user != NULL))
            {
              /* check the group permissions */
              groups = thunar_user_get_groups (user);
              for (lp = groups; lp != NULL; lp = lp->next)
                if (THUNAR_GROUP (lp->data) == group)
                  {
                    g_object_unref (G_OBJECT (user));
                    g_object_unref (G_OBJECT (group));
                    return ((mode & grp_permissions) == 0);
                  }

              /* release the effective user */
              g_object_unref (G_OBJECT (user));
            }

          /* release the file group */
          g_object_unref (G_OBJECT (group));
        }

      /* check other permissions */
      result = ((mode & oth_permissions) == 0);
    }

  return result;
}



void
thunar_file_move_thumbnail_cache_file (GFile *old_file,
                                       GFile *new_file)
{
  ThunarApplication    *application;
  ThunarThumbnailCache *thumbnail_cache;

  _thunar_return_if_fail (G_IS_FILE (old_file));
  _thunar_return_if_fail (G_IS_FILE (new_file));

  application = thunar_application_get ();
  thumbnail_cache = thunar_application_get_thumbnail_cache (application);
  thunar_thumbnail_cache_move_file (thumbnail_cache, old_file, new_file);

  g_object_unref (thumbnail_cache);
  g_object_unref (application);
}



void
thunar_file_replace_file (ThunarFile *file,
                          GFile      *renamed_file)
{
  GFile *previous_file;

  G_REC_LOCK (file_cache_mutex);

  /* get the old location */
  previous_file = file->gfile;

  /* set the new file */
  file->gfile = g_object_ref (renamed_file);

  /* drop the previous entry from the cache */
  g_hash_table_remove (file_cache, previous_file);

  /* need to re-register the monitor handle for the new uri */
  thunar_file_watch_reconnect (file);

  /* drop the reference on the previous file */
  g_object_unref (previous_file);

  /* insert the new entry */
  g_hash_table_insert (file_cache,
                       g_object_ref (file->gfile),
                       weak_ref_new (G_OBJECT (file)));

  G_REC_UNLOCK (file_cache_mutex);
}



static void
thunar_file_monitor (GFileMonitor     *monitor,
                     GFile            *event_path,
                     GFile            *other_path,
                     GFileMonitorEvent event_type,
                     gpointer          user_data)
{
  ThunarFile *file = THUNAR_FILE (user_data);

  _thunar_return_if_fail (G_IS_FILE_MONITOR (monitor));
  _thunar_return_if_fail (G_IS_FILE (event_path));
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  if (g_file_equal (event_path, file->gfile))
    {
      /* the event occurred for the monitored ThunarFile */
      switch (event_type)
        {
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
          break;
        case G_FILE_MONITOR_EVENT_DELETED:
          thunar_file_signal_destroy (file);
          return;
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
          thunar_file_reload (file);
          break;

        default:
          break;
        }

      /* Notify subscriber of the ThunarFile 'changed' signal */
      thunar_file_changed (file);
    }
  else
    {
      /* The event did not occur for the monitored ThunarFile, but for
         a file that is contained in ThunarFile which is actually a
         directory. It's handled in "thunar_folder_monitor" */
    }
}



static void
thunar_file_watch_destroyed (gpointer data)
{
  ThunarFileWatch *file_watch = data;

  if (G_LIKELY (file_watch->monitor != NULL))
    {
      g_file_monitor_cancel (file_watch->monitor);
      g_object_unref (file_watch->monitor);
      thunar_file_watch_total_count--;
    }

  g_slice_free (ThunarFileWatch, file_watch);
}



static void
thunar_file_watch_reconnect (ThunarFile *file)
{
  ThunarFileWatch *file_watch;

  /* recreate the monitor without changing the watch_count for file renames */
  file_watch = g_object_get_qdata (G_OBJECT (file), thunar_file_watch_quark);
  if (file_watch != NULL)
    {
      /* reset the old monitor */
      if (G_LIKELY (file_watch->monitor != NULL))
        {
          g_file_monitor_cancel (file_watch->monitor);
          g_object_unref (file_watch->monitor);
        }

      /* create a file or directory monitor */
      file_watch->monitor = g_file_monitor (file->gfile, G_FILE_MONITOR_WATCH_MOUNTS, NULL, NULL);
      if (G_LIKELY (file_watch->monitor != NULL))
        {
          /* watch monitor for file changes */
          g_signal_connect (file_watch->monitor, "changed", G_CALLBACK (thunar_file_monitor), file);
        }
    }
}



static void
thunar_file_info_clear (ThunarFile *file)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  /* release the current file info */
  if (file->info != NULL)
    {
      g_object_unref (file->info);
      file->info = NULL;
    }

  /* unset */
  file->kind = G_FILE_TYPE_UNKNOWN;

  /* free the custom icon name */
  g_free (file->custom_icon_name);
  file->custom_icon_name = NULL;

  /* free display name and basename */
  g_free (file->display_name);
  file->display_name = NULL;

  g_free (file->basename);
  file->basename = NULL;

  /* content type */
  g_free (file->content_type);
  file->content_type = NULL;

  g_free (file->icon_name);
  file->icon_name = NULL;

  /* device type */
  file->device_type = NULL;

  /* free collate keys */
  if (file->collate_key_nocase != file->collate_key)
    g_free (file->collate_key_nocase);
  file->collate_key_nocase = NULL;

  g_free (file->collate_key);
  file->collate_key = NULL;

  file->is_thumbnail = FALSE;

  /* free thumbnail path */
  for (gint i = 0; i < N_THUMBNAIL_SIZES; i++)
    {
      g_free (file->thumbnail_path[i]);
      file->thumbnail_path[i] = NULL;
    }

  /* assume the file is mounted by default */
  FLAG_SET (file, THUNAR_FILE_FLAG_IS_MOUNTED);

  /* set thumb state to unknown */
  for (gint i = 0; i < N_THUMBNAIL_SIZES; i++)
    thunar_file_reset_thumbnail (file, i);
}



gchar *
thunar_collate_key_for_filename (const gchar *str)
{
  /* Read the user's preference */
  gboolean smart_sorting = TRUE;

  ThunarPreferences *preferences = thunar_preferences_get ();
  g_object_get (preferences, "smart-sort", &smart_sorting, NULL);
  g_object_unref (preferences);

  /* If enabled, 'g_utf8_collate_key_for_filename' will be used for sorting,
   instead of plain ASCII comparison. */
  if (smart_sorting)
    return g_utf8_collate_key_for_filename (str, -1);
  else
    return g_strdup (str);
}



static void
thunar_file_info_reload (ThunarFile   *file,
                         GCancellable *cancellable)
{
  const gchar       *target_uri;
  const gchar       *display_name;
  gchar             *p;
  gchar             *casefold;
  gchar             *path;
  GKeyFile          *key_file;
  gboolean           launcher_name;
  ThunarPreferences *preferences;

  _thunar_return_if_fail (THUNAR_IS_FILE (file));
  _thunar_return_if_fail (file->info == NULL || G_IS_FILE_INFO (file->info));

  if (G_LIKELY (file->info != NULL))
    {
      /* this is requested so often, cache it */
      file->kind = g_file_info_get_file_type (file->info);

      if (file->kind == G_FILE_TYPE_MOUNTABLE)
        {
          target_uri = g_file_info_get_attribute_string (file->info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
          if (target_uri != NULL
              && !g_file_info_get_attribute_boolean (file->info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT))
            FLAG_SET (file, THUNAR_FILE_FLAG_IS_MOUNTED);
          else
            FLAG_UNSET (file, THUNAR_FILE_FLAG_IS_MOUNTED);
        }
    }

  /* determine the basename */
  g_free (file->basename);
  file->basename = g_file_get_basename (file->gfile);
  if (file->basename == NULL)
    file->basename = g_strdup (UNKNOWN_FILE_NAME);

  /* problematic files with content type reading */
  if (g_strcmp0 (file->basename, "kmsg") == 0
      && g_file_is_native (file->gfile))
    {
      path = g_file_get_path (file->gfile);
      if (g_strcmp0 (path, "/proc/kmsg") == 0)
        thunar_file_set_content_type (file, DEFAULT_CONTENT_TYPE);
      g_free (path);
    }

  /* check if this file is a desktop entry and we have the permission to execute it */
  if (thunar_file_is_desktop_file (file) && thunar_file_can_execute (file, NULL))
    {
      /* determine the custom icon and display name for .desktop files */

      /* query a key file for the .desktop file */
      key_file = thunar_g_file_query_key_file (file->gfile, cancellable, NULL);
      if (key_file != NULL)
        {
          /* read the icon name from the .desktop file */
          file->custom_icon_name = g_key_file_get_string (key_file,
                                                          G_KEY_FILE_DESKTOP_GROUP,
                                                          G_KEY_FILE_DESKTOP_KEY_ICON,
                                                          NULL);

          if (G_UNLIKELY (xfce_str_is_empty (file->custom_icon_name)))
            {
              /* make sure we set null if the string is empty else the assertion in
               * thunar_icon_factory_lookup_icon() will fail */
              g_free (file->custom_icon_name);
              file->custom_icon_name = NULL;
            }
          else
            {
              /* drop freedesktop.org supported suffixes from themed icons, if any */
              if (!g_path_is_absolute (file->custom_icon_name))
                {
                  p = strrchr (file->custom_icon_name, '.');
                  if (g_strcmp0 (p, ".png") == 0 || g_strcmp0 (p, ".xpm") == 0 || g_strcmp0 (p, ".svg") == 0)
                    *p = '\0';
                }
            }

          /* read the display name from the .desktop file (will be overwritten later
           * if it's undefined here) */
          preferences = thunar_preferences_get ();
          g_object_get (preferences, "misc-display-launcher-name-as-filename", &launcher_name, NULL);
          g_object_unref (preferences);
          if (thunar_g_vfs_metadata_is_supported () && xfce_g_file_is_trusted (file->gfile, NULL, NULL) && launcher_name == TRUE)
            {
              g_free (file->display_name);

              file->display_name = g_key_file_get_locale_string (key_file,
                                                                 G_KEY_FILE_DESKTOP_GROUP,
                                                                 G_KEY_FILE_DESKTOP_KEY_NAME,
                                                                 NULL, NULL);

              /* drop the name if it's empty or has invalid encoding */
              if (xfce_str_is_empty (file->display_name) || !g_utf8_validate (file->display_name, -1, NULL))
                {
                  g_free (file->display_name);
                  file->display_name = NULL;
                }
            }

          /* free the key file */
          g_key_file_free (key_file);
        }
    }
  else if (!thunar_file_is_desktop_file (file))
    {
      /* Check if a custom icon is defined for this file */
      gchar *custom_icon_name = thunar_file_get_metadata_setting (file, "thunar-custom-icon-name");
      if (!xfce_str_is_empty (custom_icon_name))
        {
          g_free (file->custom_icon_name);
          file->custom_icon_name = custom_icon_name;
        }
    }

  /* determine the display name */
  if (file->display_name == NULL)
    {
      if (G_UNLIKELY (thunar_file_is_trash (file)))
        file->display_name = g_strdup (_("Trash"));
      else if (G_LIKELY (file->info != NULL))
        {
          display_name = g_file_info_get_display_name (file->info);
          if (G_LIKELY (display_name != NULL))
            {
              if (strcmp (display_name, "/") == 0)
                file->display_name = g_strdup (_("File System"));
              else
                file->display_name = g_strdup (display_name);
            }
        }

      /* fall back to a name for the gfile */
      if (file->display_name == NULL)
        file->display_name = thunar_g_file_get_display_name (file->gfile);
    }

  /* create case sensitive collation key */
  file->collate_key = thunar_collate_key_for_filename (file->display_name);

  /* lowercase the display name */
  casefold = g_utf8_casefold (file->display_name, -1);

  /* if the lowercase name is equal, only peek the already hash key */
  if (casefold != NULL && strcmp (casefold, file->display_name) != 0)
    file->collate_key_nocase = thunar_collate_key_for_filename (casefold);
  else
    file->collate_key_nocase = file->collate_key;

  /* cleanup */
  g_free (casefold);
}



static void
thunar_file_get_async_finish (GObject      *object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  ThunarFileGetData *data = user_data;
  ThunarFile        *file;
  GFileInfo         *file_info;
  GError            *error = NULL;
  GFile             *location = G_FILE (object);

  _thunar_return_if_fail (G_IS_FILE (location));
  _thunar_return_if_fail (G_IS_ASYNC_RESULT (result));

  /* finish querying the file information */
  file_info = g_file_query_info_finish (location, result, &error);

  /* allocate a new file object */
  file = g_object_new (THUNAR_TYPE_FILE, NULL);
  file->gfile = g_object_ref (location);

  /* reset the file */
  thunar_file_info_clear (file);

  /* set the file information */
  file->info = file_info;

  /* update the file from the information */
  thunar_file_info_reload (file, data->cancellable);

  /* update the mounted info */
  if (error != NULL
      && error->domain == G_IO_ERROR
      && error->code == G_IO_ERROR_NOT_MOUNTED)
    {
      FLAG_UNSET (file, THUNAR_FILE_FLAG_IS_MOUNTED);
      g_clear_error (&error);
    }

  /* insert the file into the cache */
  G_REC_LOCK (file_cache_mutex);
  g_hash_table_insert (file_cache,
                       g_object_ref (file->gfile),
                       weak_ref_new (G_OBJECT (file)));
  G_REC_UNLOCK (file_cache_mutex);

  /* pass the loaded file and possible errors to the return function */
  (data->func) (location, file, error, data->user_data);

  /* release the file, see description in ThunarFileGetFunc */
  g_object_unref (file);

  /* free the error, if there is any */
  if (error != NULL)
    g_error_free (error);

  /* release the get data */
  if (data->cancellable != NULL)
    g_object_unref (data->cancellable);
  g_slice_free (ThunarFileGetData, data);
}



/**
 * thunar_file_load:
 * @file        : a #ThunarFile.
 * @cancellable : a #GCancellable.
 * @error       : return location for errors or %NULL.
 *
 * Loads all information about the file. As this is a possibly
 * blocking call, it can be cancelled using @cancellable.
 *
 * If loading the file fails or the operation is cancelled,
 * @error will be set.
 *
 * Return value: %TRUE on success, %FALSE on error or interruption.
 **/
static gboolean
thunar_file_load (ThunarFile   *file,
                  GCancellable *cancellable,
                  GError      **error)
{
  GError    *err = NULL;
  GFileInfo *info = NULL;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  _thunar_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
  _thunar_return_val_if_fail (G_IS_FILE (file->gfile), FALSE);

  G_REC_LOCK (file_cache_mutex);

  /* remove the file from cache */
  g_hash_table_remove (file_cache, file->gfile);

  /* query a new file info */
  info = g_file_query_info (file->gfile,
                            THUNARX_FILE_INFO_NAMESPACE,
                            G_FILE_QUERY_INFO_NONE,
                            cancellable, &err);

  /* update the mounted info */
  if (err != NULL
      && err->domain == G_IO_ERROR
      && err->code == G_IO_ERROR_NOT_MOUNTED)
    {
      FLAG_UNSET (file, THUNAR_FILE_FLAG_IS_MOUNTED);
      g_clear_error (&err);
    }

  if (err != NULL)
    {
      if (info != NULL)
        g_object_unref (info);

      g_propagate_error (error, err);

      /* even if we failed to load it, re-add the file into the cache */
      g_hash_table_insert (file_cache,
                           g_object_ref (file->gfile),
                           weak_ref_new (G_OBJECT (file)));
      G_REC_UNLOCK (file_cache_mutex);
      return FALSE;
    }

  /* reset the file */
  thunar_file_info_clear (file);

  /* update the file with the new info */
  file->info = info;

  /* update the file from the information */
  thunar_file_info_reload (file, cancellable);

  /* (re)insert the file into the cache */
  if (file->kind != G_FILE_TYPE_UNKNOWN)
    {
      g_hash_table_insert (file_cache,
                           g_object_ref (file->gfile),
                           weak_ref_new (G_OBJECT (file)));
    }

  G_REC_UNLOCK (file_cache_mutex);

  return TRUE;
}



/**
 * thunar_file_get:
 * @file  : a #GFile.
 * @error : return location for errors.
 *
 * Looks up the #ThunarFile referred to by @file. This function may return a
 * ThunarFile even though the file doesn't actually exist. This is the case
 * with remote URIs (like SFTP) for instance, if they are not mounted.
 *
 * The caller is responsible to call g_object_unref()
 * when done with the returned object.
 *
 * Return value: the #ThunarFile for @file or %NULL on errors.
 **/
ThunarFile *
thunar_file_get (GFile   *gfile,
                 GError **error)
{
  ThunarFile *file;

  _thunar_return_val_if_fail (G_IS_FILE (gfile), NULL);

  /* both lookup and insert must happen in the same critical section
   * because the insert is contigent upon the lookup */
  G_REC_LOCK (file_cache_mutex);

  /* check if we already have a cached version of that file */
  file = thunar_file_cache_lookup (gfile);
  if (G_UNLIKELY (file != NULL))
    {
      /* return the file, it already has an additional ref set
       * in thunar_file_cache_lookup */
    }
  else
    {
      /* allocate a new object */
      file = g_object_new (THUNAR_TYPE_FILE, NULL);
      file->gfile = g_object_ref (gfile);

      if (thunar_file_load (file, NULL, error))
        {
          /* Just check that it's been cached, if appropriate */
          if (file->kind != G_FILE_TYPE_UNKNOWN)
            _thunar_assert (g_hash_table_contains (file_cache, file->gfile) == TRUE);
        }
      else
        {
          /* failed loading, destroy the file */
          g_object_unref (file);

          /* make sure we return NULL */
          file = NULL;
        }
    }

  /* finished related activity on the cache */
  G_REC_UNLOCK (file_cache_mutex);

  return file;
}


/**
 * thunar_file_get_with_info:
 * @uri         : an URI or an absolute filename.
 * @info        : #GFileInfo to use when loading the info.
 * @recent_info : additional #GFileInfo to use when loading the info, only for files in `recent:///`.
 * @not_mounted : if the file is mounted.
 *
 * Looks up the #ThunarFile referred to by @file. This function may return a
 * ThunarFile even though the file doesn't actually exist. This is the case
 * with remote URIs (like SFTP) for instance, if they are not mounted.
 *
 * This function does not use g_file_query_info() to get the info,
 * but takes a reference on the @info,
 *
 * The caller is responsible to call g_object_unref()
 * when done with the returned object.
 *
 * Return value: the #ThunarFile for @file or %NULL on errors.
 **/
ThunarFile *
thunar_file_get_with_info (GFile     *gfile,
                           GFileInfo *info,
                           GFileInfo *recent_info,
                           gboolean   not_mounted)
{
  ThunarFile *file;

  _thunar_return_val_if_fail (G_IS_FILE (gfile), NULL);
  _thunar_return_val_if_fail (G_IS_FILE_INFO (info), NULL);

  /* all contingent lookups and inserts must happen in the same critical section */
  G_REC_LOCK (file_cache_mutex);

  /* check if we already have a cached version of that file */
  file = thunar_file_cache_lookup (gfile);
  if (G_UNLIKELY (file != NULL))
    {
      /* return the file, it already has an additional ref set
       * in thunar_file_cache_lookup */
    }
  else
    {
      /* allocate a new object */
      file = g_object_new (THUNAR_TYPE_FILE, NULL);
      file->gfile = g_object_ref (gfile);

      /* reset the file */
      thunar_file_info_clear (file);

      /* set the passed info */
      file->info = g_object_ref (info);

      /* update the file from the information */
      thunar_file_info_reload (file, NULL);

      /* update the mounted info */
      if (not_mounted)
        FLAG_UNSET (file, THUNAR_FILE_FLAG_IS_MOUNTED);

      /* insert the file into the cache */
      g_hash_table_insert (file_cache,
                           g_object_ref (file->gfile),
                           weak_ref_new (G_OBJECT (file)));
    }

  /* done reading and writing the cache for this file instance */
  G_REC_UNLOCK (file_cache_mutex);

  if (recent_info != NULL)
    file->recent_info = g_object_ref (recent_info);

  return file;
}



/**
 * thunar_file_get_for_uri:
 * @uri   : an URI or an absolute filename.
 * @error : return location for errors or %NULL.
 *
 * Convenience wrapper function for thunar_file_get_for_path(), as its
 * often required to determine a #ThunarFile for a given @uri.
 *
 * The caller is responsible to free the returned object using
 * g_object_unref() when no longer needed.
 *
 * Return value: the #ThunarFile for the given @uri or %NULL if
 *               unable to determine.
 **/
ThunarFile *
thunar_file_get_for_uri (const gchar *uri,
                         GError     **error)
{
  ThunarFile *file;
  GFile      *path;

  _thunar_return_val_if_fail (uri != NULL, NULL);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, NULL);

  path = g_file_new_for_commandline_arg (uri);
  file = thunar_file_get (path, error);
  g_object_unref (path);

  return file;
}



/**
 * thunar_file_get_async:
 **/
void
thunar_file_get_async (GFile            *location,
                       GCancellable     *cancellable,
                       ThunarFileGetFunc func,
                       gpointer          user_data)
{
  ThunarFile        *file;
  ThunarFileGetData *data;

  _thunar_return_if_fail (G_IS_FILE (location));
  _thunar_return_if_fail (func != NULL);

  /* check if we already have a cached version of that file */
  file = thunar_file_cache_lookup (location);
  if (G_UNLIKELY (file != NULL))
    {
      /* call the return function with the file from the cache */
      (func) (location, file, NULL, user_data);
      g_object_unref (file);
    }
  else
    {
      /* allocate get data */
      data = g_slice_new0 (ThunarFileGetData);
      data->user_data = user_data;
      data->func = func;
      if (cancellable != NULL)
        data->cancellable = g_object_ref (cancellable);

      /* load the file information asynchronously */
      g_file_query_info_async (location,
                               THUNARX_FILE_INFO_NAMESPACE,
                               G_FILE_QUERY_INFO_NONE,
                               G_PRIORITY_DEFAULT,
                               cancellable,
                               thunar_file_get_async_finish,
                               data);
    }
}



/**
 * thunar_file_get_file:
 * @file : a #ThunarFile instance.
 *
 * Returns the #GFile that refers to the location of @file.
 *
 * The returned #GFile is owned by @file and must not be released
 * with g_object_unref().
 *
 * Return value: the #GFile corresponding to @file.
 **/
GFile *
thunar_file_get_file (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (G_IS_FILE (file->gfile), NULL);
  return file->gfile;
}



/**
 * thunar_file_get_info:
 * @file : a #ThunarFile instance.
 *
 * Returns the #GFileInfo for @file.
 *
 * Note, that there's no reference taken for the caller on the
 * returned #GFileInfo, so if you need the object for a longer
 * perioud, you'll need to take a reference yourself using the
 * g_object_ref() method.
 *
 * Return value: the #GFileInfo for @file or %NULL.
 **/
GFileInfo *
thunar_file_get_info (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (file->info == NULL || G_IS_FILE_INFO (file->info), NULL);
  return file->info;
}



/**
 * thunar_file_get_parent:
 * @file  : a #ThunarFile instance.
 * @error : return location for errors.
 *
 * Determines the parent #ThunarFile for @file. If @file has no parent or
 * the user is not allowed to open the parent folder of @file, %NULL will
 * be returned and @error will be set to point to a #GError that
 * describes the cause. Else, the #ThunarFile will be returned, and
 * the caller must call g_object_unref() on it.
 *
 * You may want to call thunar_file_has_parent() first to
 * determine whether @file has a parent.
 *
 * Return value: the parent #ThunarFile or %NULL.
 **/
ThunarFile *
thunar_file_get_parent (const ThunarFile *file,
                        GError          **error)
{
  ThunarFile *parent = NULL;
  GFile      *parent_file;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, NULL);

  parent_file = g_file_get_parent (file->gfile);

  if (parent_file == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT, _("The root folder has no parent"));
      return NULL;
    }

  parent = thunar_file_get (parent_file, error);
  g_object_unref (parent_file);

  return parent;
}



/**
 * thunar_file_check_loaded:
 * @file              : a #ThunarFile instance.
 *
 * Check if @file has its information loaded, if not, try this once else
 * return %FALSE.
 *
 * Return value: %TRUE on success, else %FALSE.
 **/
gboolean
thunar_file_check_loaded (ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (G_UNLIKELY (file->info == NULL))
    thunar_file_load (file, NULL, NULL);

  return (file->info != NULL);
}



/**
 * thunar_file_execute:
 * @file              : a #ThunarFile instance.
 * @working_directory : the working directory used to resolve relative filenames
 *                      in @file_list.
 * @parent            : %NULL, a #GdkScreen or #GtkWidget.
 * @file_list         : the list of #GFile<!---->s to supply to @file on execution.
 * @startup_id        : startup id for the new window (send over for dbus) or %NULL.
 * @error             : return location for errors or %NULL.
 *
 * Tries to execute @file on the specified @screen. If @file is executable
 * and could have been spawned successfully, %TRUE is returned, else %FALSE
 * will be returned and @error will be set to point to the error location.
 *
 * Return value: %TRUE on success, else %FALSE.
 **/
gboolean
thunar_file_execute (ThunarFile  *file,
                     GFile       *working_directory,
                     gpointer     parent,
                     gboolean     in_terminal,
                     GList       *file_list,
                     const gchar *startup_id,
                     GError     **error)
{
  gboolean  snotify = FALSE;
  gboolean  prefers_no_default_gpu = FALSE;
  gboolean  terminal;
  gboolean  result = FALSE;
  GKeyFile *key_file;
  GError   *err = NULL;
  GFile    *file_parent;
  GList    *li;
  GSList   *uri_list = NULL;
  gchar    *icon_name = NULL;
  gchar    *name;
  gchar    *type;
  gchar    *url;
  gchar    *location;
  gchar    *escaped_location;
  gchar   **argv = NULL;
  gchar   **envp = NULL;
  gchar    *exec;
  gchar    *command;
  gchar    *directory = NULL;
  gboolean  is_secure = FALSE;
  guint32   stimestamp = 0;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  location = thunar_g_file_get_location (file->gfile);
  for (li = file_list; li != NULL; li = li->next)
    uri_list = g_slist_prepend (uri_list, g_file_get_uri (li->data));
  uri_list = g_slist_reverse (uri_list);

  if (thunar_file_is_desktop_file (file))
    {
      is_secure = thunar_file_can_execute (file, NULL);

      key_file = thunar_g_file_query_key_file (file->gfile, NULL, &err);
      if (key_file == NULL)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                       _("Failed to parse the desktop file: %s"), err->message);
          g_error_free (err);
          return FALSE;
        }

      type = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
      if (G_LIKELY (g_strcmp0 (type, "Application") == 0))
        {
          /* load exec property but ignore escape sequences */
          exec = g_key_file_get_value (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
          if (G_LIKELY (exec != NULL))
            {
              /* if the .desktop file is not secure, ask user what to do */
              if (is_secure || thunar_dialogs_show_insecure_program (parent, _("Untrusted application launcher"), file, exec))
                {
                  /* parse other fields */
                  name = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
                  icon_name = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
                  directory = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_PATH, NULL);
                  terminal = g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TERMINAL, NULL);
                  snotify = g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_STARTUP_NOTIFY, NULL);

                  /* New key available since version 1.4 of the Desktop Entry Specification - No G_KEY_FILE_DESKTOP macro available yet */
                  prefers_no_default_gpu = g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, "PrefersNonDefaultGPU", NULL);

                  /* expand the field codes and parse the execute command */
                  command = xfce_expand_desktop_entry_field_codes (exec, uri_list, icon_name,
                                                                   name, location, terminal);
                  g_free (name);
                  result = g_shell_parse_argv (command, NULL, &argv, error);
                  g_free (command);
                }
              else
                {
                  /* fall-through to free value and leave without execution */
                  result = TRUE;
                }

              g_free (exec);
            }
          else
            {
              /* TRANSLATORS: `Exec' is a field name in a .desktop file. Don't translate it. */
              g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                           _("No Exec field specified"));
            }
        }
      else if (g_strcmp0 (type, "Link") == 0)
        {
          url = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_URL, NULL);
          if (G_LIKELY (url != NULL))
            {
              /* if the .desktop file is not secure, ask user what to do */
              if (is_secure || thunar_dialogs_show_insecure_program (parent, _("Untrusted link launcher"), file, url))
                {
                  /* pass the URL to the webbrowser, this could be a bit strange,
                   * but then at least we are on the secure side */
                  argv = g_new (gchar *, 3);
                  argv[0] = g_strdup ("xfce-open");
                  argv[1] = url;
                  argv[2] = NULL;
                }

              result = TRUE;
            }
          else
            {
              /* TRANSLATORS: `URL' is a field name in a .desktop file. Don't translate it. */
              g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                           _("No URL field specified"));
            }
        }
      else
        {
          g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, _("Invalid desktop file"));
        }

      g_free (type);
      g_key_file_free (key_file);
    }
  else /* Not a desktop file */
    {
      /* fake the Exec line */
      escaped_location = g_shell_quote (location);
      exec = g_strconcat (escaped_location, " %F", NULL);
      command = xfce_expand_desktop_entry_field_codes (exec, uri_list, NULL, NULL, NULL, FALSE);
      if (in_terminal)
        {
          gchar *escaped = g_shell_quote (command);
          g_free (command);
          command = g_strconcat ("xfce-open --launch TerminalEmulator ", escaped, NULL);
          g_free (escaped);
        }
      result = g_shell_parse_argv (command, NULL, &argv, error);
      g_free (escaped_location);
      g_free (exec);
      g_free (command);
    }

  if (G_LIKELY (result && argv != NULL))
    {
      /* use other directory if the Path from the desktop file was not set */
      if (xfce_str_is_empty (directory))
        {
          /* determine the working directory */
          if (G_LIKELY (working_directory != NULL))
            {
              /* copy the working directory provided to this method */
              directory = g_file_get_path (working_directory);
            }
          else if (file_list != NULL)
            {
              /* use the directory of the first list item */
              file_parent = g_file_get_parent (file_list->data);
              directory = (file_parent != NULL) ? thunar_g_file_get_location (file_parent) : NULL;
              g_object_unref (file_parent);
            }
          else
            {
              /* use the directory of the executable file */
              file_parent = g_file_get_parent (file->gfile);
              directory = (file_parent != NULL) ? thunar_g_file_get_location (file_parent) : NULL;
              g_object_unref (file_parent);
            }
        }

      /* check if a startup id was passed (launch request over dbus) */
      if (startup_id != NULL && *startup_id != '\0')
        {
          /* parse startup_id string and extract timestamp
           * format: <unique>_TIME<timestamp>) */
          gchar *time_str = g_strrstr (startup_id, "_TIME");
          if (time_str != NULL)
            {
              gchar *end;

              /* ignore the "_TIME" part */
              time_str += 5;

              stimestamp = strtoul (time_str, &end, 0);
              if (end == time_str)
                stimestamp = 0;
            }
        }
      else
        {
          /* use current event time */
          stimestamp = gtk_get_current_event_time ();
        }

      if (prefers_no_default_gpu == TRUE)
        {
          envp = g_get_environ ();

          /* For nvidia discrete GPUs these environment variables do the trick */
          envp = g_environ_setenv (envp, "__NV_PRIME_RENDER_OFFLOAD", "1", TRUE);
          envp = g_environ_setenv (envp, "__GLX_VENDOR_LIBRARY_NAME", "nvidia", TRUE);
          envp = g_environ_setenv (envp, "__VK_LAYER_NV_optimus", "NVIDIA_only", TRUE);

          /* TODO: What is needed for AMD discrete GPUs ? */
        }

      /* execute the command */
      result = xfce_spawn (thunar_util_parse_parent (parent),
                           directory, argv, envp, G_SPAWN_SEARCH_PATH,
                           snotify, stimestamp, icon_name, TRUE, error);
    }

  /* clean up */
  g_slist_free_full (uri_list, g_free);
  g_strfreev (argv);
  g_strfreev (envp);
  g_free (location);
  g_free (directory);
  g_free (icon_name);

  return result;
}



/**
 * thunar_file_launch:
 * @file       : a #ThunarFile instance.
 * @parent     : a #GtkWidget or a #GdkScreen on which to launch the @file.
 *               May also be %NULL in which case the default #GdkScreen will
 *               be used.
 * @startup_id : startup id for the new window (send over for dbus) or %NULL.
 * @error      : return location for errors or %NULL.
 *
 * If @file is an executable file, tries to execute it. Else if @file is
 * a directory, opens a new #ThunarWindow to display the directory. Else,
 * the default handler for @file is determined and run.
 *
 * The @parent can be either a #GtkWidget or a #GdkScreen, on which to
 * launch the @file. If @parent is a #GtkWidget, the chooser dialog (if
 * no default application is available for @file) will be transient for
 * @parent. Else if @parent is a #GdkScreen it specifies the screen on
 * which to launch @file.
 *
 * Return value: %TRUE on success, else %FALSE.
 **/
gboolean
thunar_file_launch (ThunarFile  *file,
                    gpointer     parent,
                    const gchar *startup_id,
                    GError     **error)
{
  GdkAppLaunchContext *context;
  ThunarApplication   *application;
  GAppInfo            *app_info;
  gboolean             succeed;
  GList                path_list;
  GdkScreen           *screen;
  gboolean             ask_execute = FALSE;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  _thunar_return_val_if_fail (parent == NULL || GDK_IS_SCREEN (parent) || GTK_IS_WIDGET (parent), FALSE);

  screen = thunar_util_parse_parent (parent);

  /* check if we have a folder here */
  if (thunar_file_is_directory (file))
    {
      application = thunar_application_get ();
      thunar_application_open_window (application, file, screen, startup_id, FALSE);
      g_object_unref (G_OBJECT (application));
      return TRUE;
    }

  /* check if we should execute the file */
  if (thunar_file_can_execute (file, &ask_execute))
    {
      gint response = THUNAR_FILE_ASK_EXECUTE_RESPONSE_RUN;

      if (ask_execute)
        response = thunar_dialog_ask_execute (file, parent, TRUE, TRUE);

      if (response == THUNAR_FILE_ASK_EXECUTE_RESPONSE_RUN)
        return thunar_file_execute (file, NULL, parent, FALSE, NULL, NULL, error);
      else if (response == THUNAR_FILE_ASK_EXECUTE_RESPONSE_RUN_IN_TERMINAL)
        return thunar_file_execute (file, NULL, parent, TRUE, NULL, NULL, error);
      else if (response != THUNAR_FILE_ASK_EXECUTE_RESPONSE_OPEN)
        return TRUE;
    }

  /* determine the default application to open the file */
  /* TODO We should probably add a cancellable argument to thunar_file_launch() */
  app_info = thunar_file_get_default_handler (THUNAR_FILE (file));

  /* display the application chooser if no application is defined for this file
   * type yet */
  if (G_UNLIKELY (app_info == NULL))
    {
      thunar_show_chooser_dialog (parent, file, TRUE, FALSE, startup_id);
      return TRUE;
    }

  /* HACK: check if we're not trying to launch another file manager again, possibly
   * ourselfs which will end in a loop */
  if (g_strcmp0 (g_app_info_get_id (app_info), "xfce-file-manager.desktop") == 0
      || g_strcmp0 (g_app_info_get_id (app_info), "thunar.desktop") == 0
      || g_strcmp0 (g_app_info_get_name (app_info), "xfce-file-manager") == 0)
    {
      g_object_unref (G_OBJECT (app_info));
      thunar_show_chooser_dialog (parent, file, TRUE, FALSE, startup_id);
      return TRUE;
    }

  /* fake a path list */
  path_list.data = file->gfile;
  path_list.next = path_list.prev = NULL;

  /* create a launch context */
  context = gdk_display_get_app_launch_context (gdk_screen_get_display (screen));
  gdk_app_launch_context_set_screen (context, screen);
  gdk_app_launch_context_set_timestamp (context, gtk_get_current_event_time ());

  /* otherwise try to execute the application */
  succeed = g_app_info_launch (app_info, &path_list, G_APP_LAUNCH_CONTEXT (context), error);

  /* destroy the launch context */
  g_object_unref (context);

  /* release the handler reference */
  g_object_unref (G_OBJECT (app_info));

  return succeed;
}



static const gchar *
thunar_file_get_xdg_name (ThunarFile *file)
{
  gchar       *path;
  const gchar *special_dir;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  path = g_file_get_path (file->gfile);
  if (path == NULL)
    return NULL;

  /* check all special directories */
  for (guint i = 0; i < G_N_ELEMENTS (thunar_file_dirs); i++)
    {
      special_dir = g_get_user_special_dir (thunar_file_dirs[i].type);
      if (special_dir != NULL && g_strcmp0 (path, special_dir) == 0)
        return thunar_file_dirs[i].xdg_name;
    }

  g_free (path);

  return NULL;
}



/**
 * thunar_file_rename:
 * @file  : a #ThunarFile instance.
 * @name  : the new file name in UTF-8 encoding.
 * @error : return location for errors or %NULL.
 *
 * Tries to rename @file to the new @name. If @file cannot be renamed,
 * %FALSE will be returned and @error will be set accordingly. Else, if
 * the operation succeeds, %TRUE will be returned, and @file will have
 * a new URI and a new display name.
 *
 * When offering a rename action in the user interface, the implementation
 * should first check whether the file is available, using the
 * thunar_file_is_renameable() method.
 *
 * Return value: %TRUE on success, else %FALSE.
 **/
gboolean
thunar_file_rename (ThunarFile   *file,
                    const gchar  *name,
                    GCancellable *cancellable,
                    gboolean      called_from_job,
                    GError      **error)
{
  GFile *renamed_file;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (g_utf8_validate (name, -1, NULL), FALSE);
  _thunar_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* try to rename the file */
  renamed_file = g_file_set_display_name (file->gfile, name, cancellable, error);

  /* check if we succeeded */
  if (renamed_file != NULL)
    {
      /* if the file is a special directory, update its configuration */
      const gchar *xdg_name = thunar_file_get_xdg_name (file);
      if (xdg_name != NULL)
        thunar_g_update_user_special_dir (renamed_file, xdg_name);

      /* replace GFile in ThunarFile for the renamed file */
      thunar_file_replace_file (file, renamed_file);

      /* reload file information */
      thunar_file_load (file, NULL, NULL);

      if (!called_from_job)
        {
          /* emit the file changed signal */
          thunar_file_changed (file);
        }

      g_object_unref (renamed_file);
      return TRUE;
    }

  return FALSE;
}



/**
 * thunar_file_accepts_drop:
 * @file                    : a #ThunarFile instance.
 * @file_list               : the list of #GFile<!---->s that will be dropped.
 * @context                 : the current #GdkDragContext, which is used for the drop.
 * @suggested_action_return : return location for the suggested #GdkDragAction or %NULL.
 *
 * Checks whether @file can accept @file_list for the given @context and
 * returns the #GdkDragAction<!---->s that can be used or 0 if no actions
 * apply.
 *
 * If any #GdkDragAction<!---->s apply and @suggested_action_return is not
 * %NULL, the suggested #GdkDragAction for this drop will be stored to the
 * location pointed to by @suggested_action_return.
 *
 * Return value: the #GdkDragAction<!---->s supported for the drop or
 *               0 if no drop is possible.
 **/
GdkDragAction
thunar_file_accepts_drop (ThunarFile     *file,
                          GList          *file_list,
                          GdkDragContext *context,
                          GdkDragAction  *suggested_action_return)
{
  GdkDragAction suggested_action;
  GdkDragAction actions;
  ThunarFile   *ofile;
  ThunarFile   *parent_thunar_file;
  GList        *lp;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), 0);
  _thunar_return_val_if_fail (GDK_IS_DRAG_CONTEXT (context), 0);

  /* we can never drop an empty list */
  if (G_UNLIKELY (file_list == NULL))
    return 0;

  /* default to whatever GTK+ thinks for the suggested action */
  suggested_action = gdk_drag_context_get_suggested_action (context);

  /* get the possible actions */
  actions = gdk_drag_context_get_actions (context);

  /* when the option to ask the user is set, make it the preferred action */
  if (G_UNLIKELY ((actions & GDK_ACTION_ASK) != 0))
    suggested_action = GDK_ACTION_ASK;

  /* check if we have a writable directory here or an executable file */
  if (thunar_file_is_directory (file) && thunar_file_is_writable (file))
    {
      /* determine the possible actions */
      actions &= (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK);

      /* cannot create symbolic links in the trash or copy to the trash */
      if (thunar_file_is_trash (file))
        actions &= ~(GDK_ACTION_COPY | GDK_ACTION_LINK);

      for (lp = file_list; lp != NULL; lp = lp->next)
        {
          /* we cannot drop a file on itself */
          if (G_UNLIKELY (g_file_equal (file->gfile, lp->data)))
            return 0;

          /* copy/move/link within the trash not possible */
          if (G_UNLIKELY (thunar_g_file_is_trashed (lp->data) && thunar_file_is_trashed (file)))
            return 0;
        }

      /* if the source offers both copy and move and the GTK+ suggested action is copy, try to be smart telling whether
       * we should copy or move by default by checking whether the source and target are on the same disk.
       */
      if ((actions & (GDK_ACTION_COPY | GDK_ACTION_MOVE)) != 0
          && (suggested_action == GDK_ACTION_COPY))
        {
          /* default to move as suggested action */
          suggested_action = GDK_ACTION_MOVE;

          for (lp = file_list; lp != NULL; lp = lp->next)
            {
              /* dropping from the trash always suggests move */
              if (G_UNLIKELY (thunar_g_file_is_trashed (lp->data)))
                break;

              /* determine the cached version of the source file */
              ofile = thunar_file_cache_lookup (lp->data);

              /* fallback to non-cached version */
              if (ofile == NULL)
                ofile = thunar_file_get (lp->data, NULL);

              /* we only can 'move' if we know the source folder is writable (i.e. the file can be deleted)
               * and both the source and the target are on the same disk.
               */
              if (ofile == NULL)
                {
                  suggested_action = GDK_ACTION_COPY;
                  break;
                }
              parent_thunar_file = thunar_file_get_parent (ofile, NULL);
              if (parent_thunar_file == NULL || !thunar_file_is_writable (parent_thunar_file) || !thunar_file_same_filesystem (file, ofile))
                {
                  /* default to copy and get outta here */
                  suggested_action = GDK_ACTION_COPY;
                  if (parent_thunar_file != NULL)
                    g_object_unref (parent_thunar_file);
                  break;
                }
              if (parent_thunar_file != NULL)
                g_object_unref (parent_thunar_file);

              if (ofile != NULL)
                g_object_unref (ofile);
            }
        }
    }
  else if (thunar_file_can_execute (file, NULL))
    {
      /* determine the possible actions */
      actions &= (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_PRIVATE);
    }
  else
    return 0;

  /* determine the preferred action based on the context */
  if (G_LIKELY (suggested_action_return != NULL))
    {
      /* determine a working action */
      if (G_LIKELY ((suggested_action & actions) != 0))
        *suggested_action_return = suggested_action;
      else if ((actions & GDK_ACTION_ASK) != 0)
        *suggested_action_return = GDK_ACTION_ASK;
      else if ((actions & GDK_ACTION_COPY) != 0)
        *suggested_action_return = GDK_ACTION_COPY;
      else if ((actions & GDK_ACTION_LINK) != 0)
        *suggested_action_return = GDK_ACTION_LINK;
      else if ((actions & GDK_ACTION_MOVE) != 0)
        *suggested_action_return = GDK_ACTION_MOVE;
      else
        *suggested_action_return = GDK_ACTION_PRIVATE;
    }

  /* yeppa, we can drop here */
  return actions;
}



/**
 * thunar_file_get_date:
 * @file        : a #ThunarFile instance.
 * @date_type   : the kind of date you are interested in.
 *
 * Queries the given @date_type from @file and returns the result.
 *
 * Return value: the time for @file of the given @date_type.
 **/
guint64
thunar_file_get_date (const ThunarFile  *file,
                      ThunarFileDateType date_type)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), 0);

  if (file->info == NULL)
    return 0;

  if (date_type == THUNAR_FILE_RECENCY)
    return g_file_info_get_attribute_int64 (file->recent_info ? file->recent_info : file->info, G_FILE_ATTRIBUTE_RECENT_MODIFIED);

  return thunar_util_get_file_time (file->info, date_type);
}



/**
 * thunar_file_get_date_string:
 * @file       : a #ThunarFile instance.
 * @date_type  : the kind of date you are interested to know about @file.
 * @date_style : the style used to format the date.
 * @date_custom_style : custom style to apply, if @date_style is set to custom
 *
 * Tries to determine the @date_type of @file, and if @file supports the
 * given @date_type, it'll be formatted as string and returned. The
 * caller is responsible for freeing the string using the g_free()
 * function.
 *
 * Return value: the @date_type of @file formatted as string.
 **/
gchar *
thunar_file_get_date_string (const ThunarFile  *file,
                             ThunarFileDateType date_type,
                             ThunarDateStyle    date_style,
                             const gchar       *date_custom_style)
{
  return thunar_util_humanize_file_time (thunar_file_get_date (file, date_type), date_style, date_custom_style);
}



/**
 * thunar_file_get_mode_string:
 * @file : a #ThunarFile instance.
 *
 * Returns the mode of @file as text. You'll need to free
 * the result using g_free() when you're done with it.
 *
 * Return value: the mode of @file as string.
 **/
gchar *
thunar_file_get_mode_string (const ThunarFile *file)
{
  ThunarFileMode mode;
  GFileType      kind;
  gchar         *text;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  kind = thunar_file_get_kind (file);
  mode = thunar_file_get_mode (file);
  text = g_new (gchar, 11);

  /* file type */
  /* TODO earlier versions of Thunar had 'P' for ports and
   * 'D' for doors. Do we still need those? */
  switch (kind)
    {
    case G_FILE_TYPE_SYMBOLIC_LINK:
      text[0] = 'l';
      break;
    case G_FILE_TYPE_REGULAR:
      text[0] = '-';
      break;
    case G_FILE_TYPE_DIRECTORY:
      text[0] = 'd';
      break;
    case G_FILE_TYPE_SPECIAL:
    case G_FILE_TYPE_UNKNOWN:
    default:
      if (S_ISCHR (mode))
        text[0] = 'c';
      else if (S_ISSOCK (mode))
        text[0] = 's';
      else if (S_ISFIFO (mode))
        text[0] = 'f';
      else if (S_ISBLK (mode))
        text[0] = 'b';
      else
        text[0] = ' ';
    }

  /* permission flags */
  text[1] = (mode & THUNAR_FILE_MODE_USR_READ) ? 'r' : '-';
  text[2] = (mode & THUNAR_FILE_MODE_USR_WRITE) ? 'w' : '-';
  text[3] = (mode & THUNAR_FILE_MODE_USR_EXEC) ? 'x' : '-';
  text[4] = (mode & THUNAR_FILE_MODE_GRP_READ) ? 'r' : '-';
  text[5] = (mode & THUNAR_FILE_MODE_GRP_WRITE) ? 'w' : '-';
  text[6] = (mode & THUNAR_FILE_MODE_GRP_EXEC) ? 'x' : '-';
  text[7] = (mode & THUNAR_FILE_MODE_OTH_READ) ? 'r' : '-';
  text[8] = (mode & THUNAR_FILE_MODE_OTH_WRITE) ? 'w' : '-';
  text[9] = (mode & THUNAR_FILE_MODE_OTH_EXEC) ? 'x' : '-';

  /* special flags */
  if (G_UNLIKELY (mode & THUNAR_FILE_MODE_SUID))
    text[3] = 's';
  if (G_UNLIKELY (mode & THUNAR_FILE_MODE_SGID))
    text[6] = 's';
  if (G_UNLIKELY (mode & THUNAR_FILE_MODE_STICKY))
    text[9] = 't';

  text[10] = '\0';

  return text;
}



/**
 * thunar_file_get_size_string:
 * @file : a #ThunarFile instance.
 *
 * Returns the size of the file as text in a human readable
 * format. You'll need to free the result using g_free()
 * if you're done with it.
 *
 * Return value: the size of @file in a human readable
 *               format.
 **/
gchar *
thunar_file_get_size_string (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  return g_format_size (thunar_file_get_size (file));
}



/**
 * thunar_file_get_size_in_bytes_string:
 * @file : a #ThunarFile instance.
 *
 * Returns the size in bytes of the file as text.
 * You'll need to free the result using g_free()
 * if you're done with it.
 *
 * Return value: the size of @file in bytes.
 **/
gchar *
thunar_file_get_size_in_bytes_string (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  return g_strdup_printf ("%'" G_GUINT64_FORMAT, thunar_file_get_size (file));
}



/**
 * thunar_file_get_size_string_formatted:
 * @file             : a #ThunarFile instance.
 * @file_size_binary : indicates if file size format
 *                     should be binary or not.
 *
 * Returns the size of the file as text in a human readable
 * format in decimal or binary format. You'll need to free
 * the result using g_free() if you're done with it.
 *
 * Return value: the size of @file in a human readable
 *               format.
 **/
gchar *
thunar_file_get_size_string_formatted (const ThunarFile *file, const gboolean file_size_binary)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  return g_format_size_full (thunar_file_get_size (file),
                             file_size_binary ? G_FORMAT_SIZE_IEC_UNITS : G_FORMAT_SIZE_DEFAULT);
}



/**
 * thunar_file_get_size_string_long:
 * @file             : a #ThunarFile instance.
 * @file_size_binary : indicates if file size format
 *                     should be binary or not.
 *
 * Returns the size of the file as text in a human readable
 * format in decimal or binary format, including the exact
 * size in bytes. You'll need to free the result using
 * g_free() if you're done with it.
 *
 * Return value: the size of @file in a human readable
 *               format, including size in bytes.
 **/
gchar *
thunar_file_get_size_string_long (const ThunarFile *file, const gboolean file_size_binary)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  return g_format_size_full (thunar_file_get_size (file),
                             G_FORMAT_SIZE_LONG_FORMAT | (file_size_binary ? G_FORMAT_SIZE_IEC_UNITS : G_FORMAT_SIZE_DEFAULT));
}



/**
 * thunar_file_get_volume:
 * @file           : a #ThunarFile instance.
 *
 * Attempts to determine the #GVolume on which @file is located. If @file cannot
 * determine it's volume, then %NULL will be returned. Else a #GVolume instance
 * is returned which has to be released by the caller using g_object_unref().
 *
 * Return value: the #GVolume for @file or %NULL.
 **/
GVolume *
thunar_file_get_volume (const ThunarFile *file)
{
  GVolume *volume = NULL;
  GMount  *mount;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  /* TODO make this function call asynchronous */
  mount = g_file_find_enclosing_mount (file->gfile, NULL, NULL);
  if (mount != NULL)
    {
      volume = g_mount_get_volume (mount);
      g_object_unref (mount);
    }

  return volume;
}



/**
 * thunar_file_get_group:
 * @file : a #ThunarFile instance.
 *
 * Determines the #ThunarGroup for @file. If there's no
 * group associated with @file or if the system is unable to
 * determine the group, %NULL will be returned.
 *
 * The caller is responsible for freeing the returned object
 * using g_object_unref().
 *
 * Return value: the #ThunarGroup for @file or %NULL.
 **/
ThunarGroup *
thunar_file_get_group (const ThunarFile *file)
{
  guint32 gid;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->info == NULL)
    return NULL;

  /* TODO what are we going to do on non-UNIX systems? */
  gid = g_file_info_get_attribute_uint32 (file->info,
                                          G_FILE_ATTRIBUTE_UNIX_GID);

  return thunar_user_manager_get_group_by_id (user_manager, gid);
}



/**
 * thunar_file_get_user:
 * @file : a #ThunarFile instance.
 *
 * Determines the #ThunarUser for @file. If there's no
 * user associated with @file or if the system is unable
 * to determine the user, %NULL will be returned.
 *
 * The caller is responsible for freeing the returned object
 * using g_object_unref().
 *
 * Return value: the #ThunarUser for @file or %NULL.
 **/
ThunarUser *
thunar_file_get_user (const ThunarFile *file)
{
  guint32 uid;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->info == NULL)
    return NULL;

  /* TODO what are we going to do on non-UNIX systems? */
  uid = g_file_info_get_attribute_uint32 (file->info,
                                          G_FILE_ATTRIBUTE_UNIX_UID);

  return thunar_user_manager_get_user_by_id (user_manager, uid);
}



/**
 * thunar_file_get_content_type:
 * @file : a #ThunarFile.
 *
 * Returns the content type of @file.
 *
 * Return value: content type of @file.
 **/
const gchar *
thunar_file_get_content_type (ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (G_UNLIKELY (file->content_type == NULL))
    thunar_file_load_content_type (file);

  return file->content_type;
}



/**
 * thunar_file_set_content_type:
 * @file : a #ThunarFile.
 * @content_type : The content type
 *
 * Sets the conetnt type of a #ThunarFile.
 **/
void
thunar_file_set_content_type (ThunarFile  *file,
                              const gchar *content_type)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  if (G_LIKELY (file->content_type == NULL))
    file->content_type = g_strdup (content_type);
}



/**
 * thunar_file_get_content_type_desc:
 * @file            : a #ThunarFile.
 * @add_link_target : if the target path of a link should be added.
 *
 * Returns the content type description of @file.
 * Free the returned string with g_free().
 *
 * Return value: (non-nullable) (transfer full): content type description of @file.
 **/
gchar *
thunar_file_get_content_type_desc (ThunarFile *file,
                                   gboolean    add_link_target)
{
  const gchar *content_type;
  gchar       *description;
  gchar       *type_text;

  /* determine the content type of the file */
  content_type = thunar_file_get_content_type (file);
  if (G_UNLIKELY (content_type == NULL))
    return g_strdup ("");

  /* handle broken symlink */
  if (G_UNLIKELY (g_content_type_equals (content_type, "inode/symlink")))
    return g_strdup (_("broken link"));

  /* append " (link to <target>)" to description if link is not broken */
  if (G_UNLIKELY (thunar_file_is_symlink (file)))
    {
      type_text = g_content_type_get_description (content_type);
      if (add_link_target)
        description = g_strdup_printf (_("%s (link to %s)"), type_text, thunar_file_get_symlink_target (file));
      else
        description = g_strdup_printf (_("%s (link)"), type_text);
      g_free (type_text);
      return description;
    }

  /* append " (mount point)" to description if file is a mount point */
  if (G_UNLIKELY (thunar_file_is_mountpoint (file)))
    {
      type_text = g_content_type_get_description (content_type);
      description = g_strdup_printf (_("%s (mount point)"), type_text);
      g_free (type_text);
      return description;
    }

  return g_content_type_get_description (content_type);
}



static void
thunar_file_load_content_type (ThunarFile *file)
{
  gchar *content_type = NULL;

  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  /* make sure this is not loaded in the general info */
  _thunar_assert (file->info == NULL
                  || !g_file_info_has_attribute (file->info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE));

  if (G_UNLIKELY (file->kind == G_FILE_TYPE_DIRECTORY))
    {
      /* this we known for sure */
      thunar_file_set_content_type (file, "inode/directory");
      return;
    }

  content_type = thunar_g_file_get_content_type (file->gfile);
  thunar_file_set_content_type (file, content_type);
  g_free (content_type);
}



/**
 * thunar_file_get_symlink_target:
 * @file : a #ThunarFile.
 *
 * Returns the path of the symlink target or %NULL if the @file
 * is not a symlink.
 *
 * Return value: path of the symlink target or %NULL.
 **/
const gchar *
thunar_file_get_symlink_target (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->info == NULL)
    return NULL;

  return g_file_info_get_attribute_byte_string (file->info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
}



/**
 * thunar_file_get_basename:
 * @file : a #ThunarFile.
 *
 * Returns the basename of the @file in UTF-8 encoding.
 *
 * Return value: UTF-8 encoded basename of the @file.
 **/
const gchar *
thunar_file_get_basename (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  return file->basename;
}



/**
 * thunar_file_is_symlink:
 * @file : a #ThunarFile.
 *
 * Returns %TRUE if @file is a symbolic link.
 *
 * Return value: %TRUE if @file is a symbolic link.
 **/
gboolean
thunar_file_is_symlink (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  return g_file_info_get_attribute_boolean (file->info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK);
}



/**
 * thunar_file_get_size:
 * @file : a #ThunarFile instance.
 *
 * Tries to determine the size of @file in bytes and
 * returns the size.
 *
 * Return value: the size of @file in bytes.
 **/
guint64
thunar_file_get_size (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), 0);

  if (file->info == NULL)
    return 0;

  /* The attribute 'standard::size' is not defined for locations like 'trash:///', */
  /* 'computer:///' and others. So we would get GLib-GIO-CRITICALs when using 'g_file_info_get_size'  */
  return g_file_info_get_attribute_uint64 (file->info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
}



/**
 * thunar_file_get_size_on_disk:
 * @file : a #ThunarFile instance.
 *
 * Tries to determine the allocated size of @file in bytes and
 * returns the allocated size if available, otherwise returns -1.
 *
 * Return value: the allocated size of @file in bytes.
 **/
guint64
thunar_file_get_size_on_disk (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), 0);

  if (file->info == NULL)
    return (guint64) -1;

  if (g_file_info_has_attribute (file->info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE))
    return g_file_info_get_attribute_uint64 (file->info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);

  return (guint64) -1;
}



/**
 * thunar_file_get_default_handler:
 * @file : a #ThunarFile instance.
 *
 * Returns the default #GAppInfo for @file or %NULL if there is none.
 *
 * The caller is responsible to free the returned #GAppInfo using
 * g_object_unref().
 *
 * Return value: Default #GAppInfo for @file or %NULL if there is none.
 **/
GAppInfo *
thunar_file_get_default_handler (const ThunarFile *file)
{
  const gchar *content_type;
  GAppInfo    *app_info = NULL;
  gboolean     must_support_uris = FALSE;
  gchar       *path;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  content_type = thunar_file_get_content_type (THUNAR_FILE (file));
  if (content_type != NULL)
    {
      path = g_file_get_path (file->gfile);
      must_support_uris = (path == NULL);
      g_free (path);

      app_info = g_app_info_get_default_for_type (content_type, must_support_uris);
    }

  if (app_info == NULL)
    app_info = g_file_query_default_handler (file->gfile, NULL, NULL);

  return app_info;
}



/**
 * thunar_file_get_kind:
 * @file : a #ThunarFile instance.
 *
 * Returns the kind of @file.
 *
 * Return value: the kind of @file.
 **/
GFileType
thunar_file_get_kind (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), G_FILE_TYPE_UNKNOWN);
  return file->kind;
}



GFile *
thunar_file_get_target_location (const ThunarFile *file)
{
  const gchar *uri;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->info == NULL)
    return g_object_ref (file->gfile);

  uri = g_file_info_get_attribute_string (file->info,
                                          G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);

  return (uri != NULL) ? g_file_new_for_uri (uri) : NULL;
}



/**
 * thunar_file_get_mode:
 * @file : a #ThunarFile instance.
 *
 * Returns the permission bits of @file.
 *
 * Return value: the permission bits of @file.
 **/
ThunarFileMode
thunar_file_get_mode (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), 0);

  if (file->info == NULL)
    return 0;

  if (g_file_info_has_attribute (file->info, G_FILE_ATTRIBUTE_UNIX_MODE))
    return g_file_info_get_attribute_uint32 (file->info, G_FILE_ATTRIBUTE_UNIX_MODE);
  else
    return thunar_file_is_directory (file) ? 0777 : 0666;
}



gboolean
thunar_file_is_mounted (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return FLAG_IS_SET (file, THUNAR_FILE_FLAG_IS_MOUNTED);
}



gboolean
thunar_file_exists (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return g_file_query_exists (file->gfile, NULL);
}



/**
 * thunar_file_is_directory:
 * @file : a #ThunarFile instance.
 *
 * Checks whether @file refers to a directory.
 *
 * Return value: %TRUE if @file is a directory.
 **/
gboolean
thunar_file_is_directory (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return file->kind == G_FILE_TYPE_DIRECTORY;
}



/**
 * thunar_g_file_is_empty_directory:
 * @file : a #ThunarFile instance.
 *
 * Checks whether @file refers to an empty directory
 * If the directory is not readable, the method will as well return TRUE
 *
 * Return value: %TRUE if @file is an empty directory.
 **/
gboolean
thunar_file_is_empty_directory (const ThunarFile *file)
{
  GFileEnumerator *enumerator;
  GError          *err = NULL;
  gboolean         is_empty = TRUE;
  GFileInfo       *info;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (thunar_file_is_directory (file) == FALSE)
    return FALSE;

  /* Threat non-readable direcories like they were empty */
  if (!thunar_file_is_readable (file))
    return TRUE;

  enumerator = g_file_enumerate_children (file->gfile, NULL,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, &err);

  if (err != NULL)
    {
      g_warning ("Failed to check if the file has any children! Reason: %s", err->message);
      g_error_free (err);
      return TRUE;
    }

  if (g_file_enumerator_iterate (enumerator, &info, NULL, NULL, &err) && info != NULL)
    is_empty = FALSE;

  g_object_unref (enumerator);

  return is_empty;
}



/**
 * thunar_file_is_shortcut:
 * @file : a #ThunarFile instance.
 *
 * Checks whether @file refers to a shortcut to something else.
 *
 * Return value: %TRUE if @file is a shortcut.
 **/
gboolean
thunar_file_is_shortcut (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return file->kind == G_FILE_TYPE_SHORTCUT;
}



/**
 * thunar_file_is_mountable:
 * @file : a #ThunarFile instance.
 *
 * Checks whether @file refers to a mountable file/directory.
 *
 * Return value: %TRUE if @file is a mountable file/directory.
 **/
gboolean
thunar_file_is_mountable (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return file->kind == G_FILE_TYPE_MOUNTABLE;
}



/**
 * thunar_file_is_mountpoint:
 * @file : a #ThunarFile.
 *
 * Returns %TRUE if @file is a mount point.
 *
 * Return value: %TRUE if @file is a mount point.
 **/
gboolean
thunar_file_is_mountpoint (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  return g_file_info_get_attribute_boolean (file->info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT);
}



/**
 * thunar_file_is_local:
 * @file : a #ThunarFile instance.
 *
 * Returns %TRUE if @file is a local file with the
 * file:// URI scheme.
 *
 * Return value: %TRUE if @file is local.
 **/
gboolean
thunar_file_is_local (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return g_file_has_uri_scheme (file->gfile, "file");
}



/**
 * thunar_file_is_parent:
 * @file  : a #ThunarFile instance.
 * @child : another #ThunarFile instance.
 *
 * Determines whether @file is the parent directory of @child.
 *
 * Return value: %TRUE if @file is the parent of @child.
 **/
gboolean
thunar_file_is_parent (const ThunarFile *file,
                       const ThunarFile *child)
{
  gboolean is_parent = FALSE;
  GFile   *parent;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (child), FALSE);

  parent = g_file_get_parent (child->gfile);
  if (parent != NULL)
    {
      is_parent = g_file_equal (file->gfile, parent);
      g_object_unref (parent);
    }

  return is_parent;
}



/**
 * thunar_file_is_ancestor:
 * @file     : a #ThunarFile instance.
 * @ancestor : another #GFile instance.
 *
 * Determines whether @file is somewhere inside @ancestor,
 * possibly with intermediate folders.
 *
 * Return value: %TRUE if @ancestor contains @file as a
 *               child, grandchild, great grandchild, etc.
 **/
gboolean
thunar_file_is_gfile_ancestor (const ThunarFile *file,
                               GFile            *ancestor)
{
  GFile *current = NULL;
  GFile *tmp;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (G_IS_FILE (file->gfile), FALSE);
  _thunar_return_val_if_fail (G_IS_FILE (ancestor), FALSE);

  current = g_object_ref (file->gfile);
  while (TRUE)
    {
      tmp = g_file_get_parent (current);
      g_object_unref (current);
      current = tmp;

      if (current == NULL) /* parent of root is NULL */
        return FALSE;

      if (G_UNLIKELY (g_file_equal (current, ancestor)))
        {
          g_object_unref (current);
          return TRUE;
        }
    }

  return FALSE;
}



/**
 * thunar_file_is_ancestor:
 * @file     : a #ThunarFile instance.
 * @ancestor : another #ThunarFile instance.
 *
 * Determines whether @file is somewhere inside @ancestor,
 * possibly with intermediate folders.
 *
 * Return value: %TRUE if @ancestor contains @file as a
 *               child, grandchild, great grandchild, etc.
 **/
gboolean
thunar_file_is_ancestor (const ThunarFile *file,
                         const ThunarFile *ancestor)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (ancestor), FALSE);

  return thunar_file_is_gfile_ancestor (file, ancestor->gfile);
}



/**
 * thunar_file_can_execute:
 * @file : a #ThunarFile instance.
 *
 * Determines whether the owner of the current process is allowed
 * to execute the @file (or enter the directory refered to by
 * @file). On UNIX it also returns %TRUE if @file refers to a
 * desktop entry.
 *
 * Return value: %TRUE if @file can be executed.
 **/
gboolean
thunar_file_can_execute (ThunarFile *file,
                         gboolean   *ask_execute)
{
  ThunarFile        *file_to_check;
  GFile             *link_target;
  ThunarPreferences *preferences;
  gint               exec_shell_scripts = THUNAR_EXECUTE_SHELL_SCRIPT_NEVER;
  const gchar       *content_type;
  gboolean           exec_bit_set = FALSE;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (thunar_file_is_symlink (file))
    {
      link_target = thunar_g_file_resolve_symlink (file->gfile);
      if (link_target == NULL)
        return FALSE;

      if (g_file_equal (link_target, file->gfile))
        file_to_check = g_object_ref (file);
      else
        file_to_check = thunar_file_get (link_target, NULL);

      g_object_unref (link_target);
      if (file_to_check == NULL)
        return FALSE;
    }
  else
    file_to_check = g_object_ref (file);

  if (file_to_check->info == NULL)
    {
      g_object_unref (file_to_check);
      return FALSE;
    }

  exec_bit_set = g_file_info_get_attribute_boolean (file_to_check->info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);

  if (!thunar_file_is_desktop_file (file_to_check))
    {
      if (!exec_bit_set)
        {
          g_object_unref (file_to_check);
          return FALSE;
        }

      /* get the content type of the file */
      content_type = thunar_file_get_content_type (THUNAR_FILE (file_to_check));
      if (G_UNLIKELY (content_type == NULL))
        {
          g_object_unref (file_to_check);
          return FALSE;
        }

      if (!g_content_type_can_be_executable (content_type))
        {
          g_object_unref (file_to_check);
          return FALSE;
        }

      /* do never execute plain text files which are not shell scripts but marked executable */
      if (g_content_type_equals (content_type, "text/plain") || g_content_type_equals (content_type, "application/json") || g_content_type_equals (content_type, "application/xml"))
        {
          g_object_unref (file_to_check);
          return FALSE;
        }

      if (g_content_type_is_a (content_type, "text/plain"))
        {
          /* check if the shell scripts should be executed or opened by default */
          preferences = thunar_preferences_get ();
          g_object_get (preferences, "misc-exec-shell-scripts-by-default", &exec_shell_scripts, NULL);
          g_object_unref (preferences);

          if (exec_shell_scripts == THUNAR_EXECUTE_SHELL_SCRIPT_NEVER)
            {
              g_object_unref (file_to_check);
              return FALSE;
            }

          if (exec_shell_scripts == THUNAR_EXECUTE_SHELL_SCRIPT_ASK && ask_execute)
            *ask_execute = TRUE;
        }

      g_object_unref (file_to_check);
      return TRUE;
    }

  /* desktop files in XDG_DATA_DIRS dont need an executable bit to be executed */
  if (thunar_g_file_is_in_xdg_data_dir (file_to_check->gfile))
    {
      g_object_unref (file_to_check);
      return TRUE;
    }

  /* Desktop files outside XDG_DATA_DIRS need to have at least the execute bit set */
  if (!exec_bit_set)
    {
      g_object_unref (file_to_check);
      return FALSE;
    }

  /* Additional security measure only applicable if gvfs is installed: */
  /* Desktop files outside XDG_DATA_DIRS, need to be 'trusted'. */
  if (thunar_g_vfs_metadata_is_supported ())
    {
      gboolean can_execute = xfce_g_file_is_trusted (file_to_check->gfile, NULL, NULL);
      g_object_unref (file_to_check);
      return can_execute;
    }

  g_object_unref (file_to_check);
  return TRUE;
}



/**
 * thunar_file_is_readable:
 * @file : a #ThunarFile instance.
 *
 * Determines whether the owner of the current process is allowed
 * to read the @file.
 *
 * Return value: %TRUE if @file can be read.
 **/
static gboolean
thunar_file_is_readable (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  if (!g_file_info_has_attribute (file->info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
    return TRUE;

  return g_file_info_get_attribute_boolean (file->info,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
}



/**
 * thunar_file_is_writable:
 * @file : a #ThunarFile instance.
 *
 * Determines whether the owner of the current process is allowed
 * to write the @file.
 *
 * Return value: %TRUE if @file can be read.
 **/
gboolean
thunar_file_is_writable (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  if (!g_file_info_has_attribute (file->info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    return TRUE;

  return g_file_info_get_attribute_boolean (file->info,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
}



/**
 * thunar_file_is_hidden:
 * @file : a #ThunarFile instance.
 *
 * Checks whether @file can be considered a hidden file.
 *
 * Return value: %TRUE if @file is a hidden file, else %FALSE.
 **/
gboolean
thunar_file_is_hidden (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  return g_file_info_get_attribute_boolean (file->info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN)
         || g_file_info_get_attribute_boolean (file->info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP);
}



/**
 * thunar_file_is_home:
 * @file : a #ThunarFile.
 *
 * Checks whether @file refers to the users home directory.
 *
 * Return value: %TRUE if @file is the users home directory.
 **/
gboolean
thunar_file_is_home (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return thunar_g_file_is_home (file->gfile);
}



/**
 * thunar_file_is_regular:
 * @file : a #ThunarFile.
 *
 * Checks whether @file refers to a regular file.
 *
 * Return value: %TRUE if @file is a regular file.
 **/
gboolean
thunar_file_is_regular (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return file->kind == G_FILE_TYPE_REGULAR;
}



/**
 * thunar_file_is_trash:
 * @file : a #ThunarFile instance.
 *
 * Returns %TRUE if @file is the trash bin.
 *
 * Return value: %TRUE if @file is in the trash bin
 **/
gboolean
thunar_file_is_trash (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return thunar_g_file_is_trash (file->gfile);
}



/**
 * thunar_file_is_trashed:
 * @file : a #ThunarFile instance.
 *
 * Returns %TRUE if @file is a local file that resides in
 * the trash bin.
 *
 * Return value: %TRUE if @file is in the trash, or
 *               the trash folder itself.
 **/
gboolean
thunar_file_is_trashed (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return thunar_g_file_is_trashed (file->gfile);
}



/**
 * thunar_file_is_recent:
 * @file : a #ThunarFile instance.
 *
 * Returns %TRUE if @file is the recent folder.
 *
 * Return value: %TRUE if @file is the recent folder bin
 **/
gboolean
thunar_file_is_recent (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return thunar_g_file_is_recent (file->gfile);
}



gboolean
thunar_file_is_in_recent (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return thunar_g_file_is_in_recent (file->gfile);
}



/**
 * thunar_file_add_to_recent:
 * @file : a #ThunarFile instance.
 *
 * Adds the file to recent:/// if it is not a directory
 **/
void
thunar_file_add_to_recent (const ThunarFile *file)
{
  gchar *uri;

  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  /* dont add directories to recent */
  if (thunar_file_is_directory (file))
    return;

  uri = g_file_get_uri (file->gfile);
  gtk_recent_manager_add_item (gtk_recent_manager_get_default (), uri);
  g_free (uri);
}



/**
 * thunar_file_is_desktop_file:
 * @file      : a #ThunarFile.
 *
 * Returns %TRUE if @file is a .desktop file.
 *
 * Return value: %TRUE if @file is a .desktop file.
 **/
gboolean
thunar_file_is_desktop_file (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  /* only allow regular files with a .desktop extension */
  if (!g_str_has_suffix (file->basename, ".desktop")
      || file->kind != G_FILE_TYPE_REGULAR)
    return FALSE;

  return TRUE;
}



/**
 * thunar_file_get_display_name:
 * @file : a #ThunarFile instance.
 *
 * Returns the @file name in the UTF-8 encoding, which is
 * suitable for displaying the file name in the GUI.
 *
 * Return value: the @file name suitable for display.
 **/
const gchar *
thunar_file_get_display_name (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);
  return file->display_name;
}



/**
 * thunar_file_get_deletion_date:
 * @file       : a #ThunarFile instance.
 * @date_style : the style used to format the date.
 * @date_custom_style : custom style to apply, if @date_style is set to custom
 *
 * Returns the deletion date of the @file if the @file
 * is located in the trash. Otherwise %NULL will be
 * returned.
 *
 * The caller is responsible to free the returned string
 * using g_free() when no longer needed.
 *
 * Return value: the deletion date of @file if @file is
 *               in the trash, %NULL otherwise.
 **/
gchar *
thunar_file_get_deletion_date (const ThunarFile *file,
                               ThunarDateStyle   date_style,
                               const gchar      *date_custom_style)
{
  const gchar *date;
  time_t       deletion_time;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (G_IS_FILE_INFO (file->info), NULL);

  date = g_file_info_get_attribute_string (file->info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE);
  if (G_UNLIKELY (date == NULL))
    return NULL;

  /* try to parse the DeletionDate (RFC 3339 string) */
  deletion_time = thunar_util_time_from_rfc3339 (date);

  /* humanize the time value */
  return thunar_util_humanize_file_time (deletion_time, date_style, date_custom_style);
}



/**
 * thunar_file_get_recency:
 * @file       : a #ThunarFile instance.
 * @date_style : the style used to format the date.
 * @date_custom_style : custom style to apply, if @date_style is set to custom
 *
 * Returns the recency date of the @file if the @file
 * is in the `recent:///` location. Recency differs from date accessed and date
 * modified. It refers to the time of the last metadata change of a file in `recent:///`.
 *
 * Return value: the recency date of @file if @file is
 *               in `recent:///`, %NULL otherwise.
 **/
gchar *
thunar_file_get_recency (const ThunarFile *file,
                         ThunarDateStyle   date_style,
                         const gchar      *date_custom_style)
{
  const gchar *date;
  time_t       recency_time;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (G_IS_FILE_INFO (file->recent_info), NULL);

  date = g_file_info_get_attribute_string (file->recent_info, G_FILE_ATTRIBUTE_RECENT_MODIFIED);
  if (G_UNLIKELY (date == NULL))
    return NULL;

  /* try to parse the DeletionDate (RFC 3339 string) */
  recency_time = thunar_util_time_from_rfc3339 (date);

  /* humanize the time value */
  return thunar_util_humanize_file_time (recency_time, date_style, date_custom_style);
}



/**
 * thunar_file_get_original_path:
 * @file : a #ThunarFile instance.
 *
 * Returns the original path of the @file if the @file
 * is located in the trash. Otherwise %NULL will be
 * returned.
 *
 * Return value: the original path of @file if @file is
 *               in the trash, %NULL otherwise.
 **/
const gchar *
thunar_file_get_original_path (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->info == NULL)
    return NULL;

  return g_file_info_get_attribute_byte_string (file->info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
}



/**
 * thunar_file_get_trash_item_count:
 * @file : a #ThunarFile instance.
 *
 * Returns the number of items in the trash, if @file refers to the
 * trash root directory. Otherwise returns 0.
 *
 * Return value: number of files in the trash if @file is the trash
 *               root dir, 0 otherwise.
 **/
guint32
thunar_file_get_trash_item_count (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), 0);

  if (file->info == NULL)
    return 0;

  return g_file_info_get_attribute_uint32 (file->info,
                                           G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT);
}



/**
 * thunar_file_is_chmodable:
 * @file : a #ThunarFile instance.
 *
 * Determines whether the owner of the current process is allowed
 * to changed the file mode of @file.
 *
 * Return value: %TRUE if the mode of @file can be changed.
 **/
gboolean
thunar_file_is_chmodable (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  /* we can only change the mode if we the euid is
   *   a) equal to the file owner id
   * or
   *   b) the super-user id
   * and the file is not in the trash.
   */
  if (file->info == NULL)
    {
      return (effective_user_id == 0 && !thunar_file_is_trashed (file));
    }
  else
    {
      return ((effective_user_id == 0
               || effective_user_id == g_file_info_get_attribute_uint32 (file->info, G_FILE_ATTRIBUTE_UNIX_UID))
              && !thunar_file_is_trashed (file));
    }
}



/**
 * thunar_file_is_renameable:
 * @file : a #ThunarFile instance.
 *
 * Determines whether @file can be renamed using
 * #thunar_file_rename(). Note that the return
 * value is just a guess and #thunar_file_rename()
 * may fail even if this method returns %TRUE.
 *
 * Return value: %TRUE if @file can be renamed.
 **/
gboolean
thunar_file_is_renameable (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  return g_file_info_get_attribute_boolean (file->info,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME);
}



gboolean
thunar_file_can_be_trashed (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  return g_file_info_get_attribute_boolean (file->info,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH);
}



/**
 * thunar_file_get_file_count
 * @file    : a #ThunarFile instance.
 * @callback: a #GCallback to be executed after the file count
 * @data    : a #gpointer containing user data to pass to the callback
 *
 * Returns the number of items in the directory
 * Counts the number of files in the directory as fast as possible.
 * Will use cached data to do calculations only when the file has
 * been modified since the last time its contents were counted.
 *
 * Will return -1 if the file info cannot be loaded
 *
 * Return value: Number of files in a folder
 **/
gint
thunar_file_get_file_count (ThunarFile *file,
                            GCallback   callback,
                            gpointer    data)
{
  GError    *err = NULL;
  ThunarJob *job;
  GFileInfo *info;
  guint64    last_modified;

  _thunar_return_val_if_fail (thunar_file_is_directory (file), 0);

  /* Forcefully get cached values by passing NULL as the second argument */
  if (callback == NULL)
    return file->file_count;

  info = g_file_query_info (thunar_file_get_file (file),
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            &err);

  /* If the file info cannot be loaded, return -1 */
  if (err != NULL)
    {
      if (info != NULL)
        g_object_unref (info);
      g_error_free (err);
      return -1;
    }

  last_modified = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  g_object_unref (info);

  /* return a cached value if last time that the file count was computed is later
   * than the last time the file was modified */
  if (G_LIKELY (last_modified < file->file_count_timestamp))
    {
      /* For consistent behavior we might as well call the callback;
       * Although the ThunarJob parameter will be NULL which should be
       * dealt with accordingly by the callback */
      ((void (*) (ThunarJob *, gpointer)) (*callback)) (NULL, data);
      return file->file_count;
    }

  /* put the timestamp calculation at the *start* of the process to prevent another call to
   * thunar_file_get_count starting another job on the same folder before one has ended.
   * Divide by 1e6 to convert from microseconds to seconds */
  file->file_count_timestamp = g_get_real_time () / (guint64) 1e6;

  /* set up a job to actually enumerate over the folder's contents and get its file count */
  job = thunar_io_jobs_count_files (file);

  /* set up the signal on finish to call the callback and keep a reference on the data */
  if (callback != NULL && data != NULL && G_IS_OBJECT (data))
    {
      g_signal_connect_data (job,
                             "finished",
                             G_CALLBACK (callback),
                             g_object_ref (G_OBJECT (data)),
                             (GClosureNotify) (void (*) (void)) g_object_unref,
                             G_CONNECT_AFTER);
    }

  thunar_job_launch (THUNAR_JOB (job));

  /* release our reference on the job (a ref is hold by itself, once it is launched) */
  g_object_unref (job);

  return file->file_count;
}



/**
 * thunar_file_set_file_count
 * @file: A #ThunarFileInstance
 * @count: The value to set the file's count to
 *
 * Set @file's count to the given number if it is a directory.
 * Does *NOT* update the file's count timestamp.
 **/
void
thunar_file_set_file_count (ThunarFile *file,
                            const guint count)
{
  _thunar_return_if_fail (thunar_file_is_directory (file));

  file->file_count = count;
}


/**
 * thunar_file_get_emblem_names:
 * @file : a #ThunarFile instance.
 *
 * Determines the names of the emblems that should be displayed for
 * @file. Sfter usage the returned list must released with g_list_free_full (list,g_free)
 *
 * Return value: the names of the emblems for @file.
 **/
GList *
thunar_file_get_emblem_names (ThunarFile *file)
{
  guint32 uid;
  gchar  *emblem_names_joined;
  gchar **emblem_names;
  GList  *emblems = NULL;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  /* leave if there is no info */
  if (file->info == NULL)
    return NULL;

  /* determine the custom emblems and transform them to a g_list */
  emblem_names_joined = thunar_g_file_get_metadata_setting (file->gfile, file->info, THUNAR_GTYPE_STRINGV, "emblems");
  if (emblem_names_joined != NULL)
    {
      emblem_names = g_strsplit (emblem_names_joined, THUNAR_METADATA_STRING_DELIMETER, 100);
      g_free (emblem_names_joined);

      if (G_LIKELY (emblem_names != NULL))
        {
          for (gchar **lp = emblem_names; *lp != NULL; ++lp)
            emblems = g_list_append (emblems, g_strdup (*lp));
        }
      g_strfreev (emblem_names);
    }

  if (thunar_file_is_symlink (file))
    emblems = g_list_prepend (emblems, g_strdup (THUNAR_FILE_EMBLEM_NAME_SYMBOLIC_LINK));

  /* determine the user ID of the file owner */
  /* TODO what are we going to do here on non-UNIX systems? */
  uid = file->info != NULL
        ? g_file_info_get_attribute_uint32 (file->info, G_FILE_ATTRIBUTE_UNIX_UID)
        : 0;

  /* we add "cant-read" if either (a) the file is not readable or (b) a directory, that lacks the
   * x-bit, see https://bugzilla.xfce.org/show_bug.cgi?id=1408 for the details about this change.
   */
  if (!thunar_file_is_readable (file)
      || (thunar_file_is_directory (file)
          && thunar_file_denies_access_permission (file, THUNAR_FILE_MODE_USR_EXEC,
                                                   THUNAR_FILE_MODE_GRP_EXEC,
                                                   THUNAR_FILE_MODE_OTH_EXEC)))
    {
      emblems = g_list_prepend (emblems, g_strdup (THUNAR_FILE_EMBLEM_NAME_CANT_READ));
    }
  else if (G_UNLIKELY (uid == effective_user_id && !thunar_file_is_writable (file) && !thunar_file_is_trashed (file) && !thunar_file_is_in_recent (file)))
    {
      /* we own the file, but we cannot write to it, that's why we mark it as "cant-write", so
       * users won't be surprised when opening the file in a text editor, but are unable to save.
       */
      emblems = g_list_prepend (emblems, g_strdup (THUNAR_FILE_EMBLEM_NAME_CANT_WRITE));
    }

  /* add mount icon as emblem to mount points */
  if (thunar_file_is_mountpoint (file))
    {
      GMount *mount = g_file_find_enclosing_mount (file->gfile, NULL, NULL);
      if (mount != NULL)
        {
          GIcon *icon = g_mount_get_icon (mount);
          if (icon != NULL)
            {
              if (G_IS_THEMED_ICON (icon))
                {
                  const gchar *icon_name = g_themed_icon_get_names (G_THEMED_ICON (icon))[0];
                  if (icon_name != NULL)
                    emblems = g_list_prepend (emblems, g_strdup (icon_name));
                }
              g_object_unref (icon);
            }
          g_object_unref (mount);
        }
    }

  return emblems;
}



/**
 * thunar_file_set_custom_icon:
 * @file        : a #ThunarFile instance.
 * @custom_icon : the new custom icon for the @file.
 * @error       : return location for errors or %NULL.
 *
 * Tries to change the custom icon of @file.
 * If that fails, %FALSE is returned and the @error is set accordingly.
 *
 * Return value: %TRUE if the icon of @file was changed, %FALSE otherwise.
 **/
gboolean
thunar_file_set_custom_icon (ThunarFile  *file,
                             const gchar *custom_icon,
                             GError     **error)
{
  _thunar_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (thunar_file_is_desktop_file (file))
    {
      GKeyFile *key_file = thunar_g_file_query_key_file (file->gfile, NULL, error);

      if (key_file == NULL)
        return FALSE;

      g_key_file_set_string (key_file, G_KEY_FILE_DESKTOP_GROUP,
                             G_KEY_FILE_DESKTOP_KEY_ICON, custom_icon);

      if (thunar_g_file_write_key_file (file->gfile, key_file, NULL, error))
        {
          /* Make sure to update the trusted checksum */
          xfce_g_file_set_trusted (file->gfile, TRUE, NULL, NULL);

          /* tell everybody that we have changed */
          thunar_file_changed (file);

          g_key_file_free (key_file);
          return TRUE;
        }
      else
        {
          g_key_file_free (key_file);
          return FALSE;
        }
    }
  else
    {
      if (xfce_str_is_empty (custom_icon))
        thunar_file_clear_metadata_setting (file, "thunar-custom-icon-name");
      else
        thunar_file_set_metadata_setting (file, "thunar-custom-icon-name", custom_icon, FALSE);

      thunar_file_reload (file);
      return TRUE;
    }
}


/**
 * thunar_file_is_desktop:
 * @file : a #ThunarFile.
 *
 * Checks whether @file refers to the users desktop directory.
 *
 * Return value: %TRUE if @file is the users desktop directory.
 **/
gboolean
thunar_file_is_desktop (const ThunarFile *file)
{
  GFile   *desktop;
  gboolean is_desktop = FALSE;

  desktop = g_file_new_for_path (g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP));
  is_desktop = g_file_equal (file->gfile, desktop);
  g_object_unref (desktop);

  return is_desktop;
}



void
thunar_file_set_is_thumbnail (ThunarFile *file,
                              gboolean    is_thumbnail)
{
  file->is_thumbnail = is_thumbnail;
}



static gchar *
thunar_file_get_thumbnail_path_real (ThunarFile         *file,
                                     ThunarThumbnailSize thumbnail_size)
{
  GChecksum *checksum;
  gchar     *filename;
  gchar     *uri;
  gchar     *thumbnail_path = NULL;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  /* if the file is a thumbnail itself, use it as a thumbnail */
  if (G_UNLIKELY (file->is_thumbnail))
    return g_file_get_path (file->gfile);

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  if (G_LIKELY (checksum != NULL))
    {
      uri = thunar_file_dup_uri (file);
      g_checksum_update (checksum, (const guchar *) uri, strlen (uri));
      g_free (uri);

      filename = g_strconcat (g_checksum_get_string (checksum), ".png", NULL);

      /* The thumbnail is in the format/location
       * $XDG_CACHE_HOME/thumbnails/(nromal|large)/MD5_Hash_Of_URI.png
       * for version 0.8.0 if XDG_CACHE_HOME is defined, otherwise
       * /homedir/.thumbnails/(normal|large)/MD5_Hash_Of_URI.png
       * will be used, which is also always used for versions prior
       * to 0.7.0.
       */

      /* build and check if the thumbnail is in the new location */
      thumbnail_path = g_build_path ("/", g_get_user_cache_dir (),
                                     "thumbnails", thunar_thumbnail_size_get_nick (thumbnail_size),
                                     filename, NULL);

      if (!g_file_test (thumbnail_path, G_FILE_TEST_EXISTS))
        {
          /* Fallback to old version */
          g_free (thumbnail_path);

          thumbnail_path = g_build_filename (xfce_get_homedir (),
                                             ".thumbnails", thunar_thumbnail_size_get_nick (thumbnail_size),
                                             filename, NULL);

          if (!g_file_test (thumbnail_path, G_FILE_TEST_EXISTS))
            {
              g_free (thumbnail_path);
              thumbnail_path = NULL;

              if (thunar_file_is_directory (file) == FALSE)
                {
                  /* Thumbnail doesn't exist in either spot, look for shared repository */
                  uri = thunar_file_dup_uri (file);
                  thumbnail_path = xfce_create_shared_thumbnail_path (uri, thunar_thumbnail_size_get_nick (thumbnail_size));
                  g_free (uri);

                  if (thumbnail_path != NULL && !g_file_test (thumbnail_path, G_FILE_TEST_EXISTS))
                    {
                      /* Thumbnail doesn't exist */
                      g_free (thumbnail_path);
                      thumbnail_path = NULL;
                    }
                }
            }
        }

      g_free (filename);
      g_checksum_free (checksum);
    }

  return thumbnail_path;
}



const gchar *
thunar_file_get_thumbnail_path (ThunarFile         *file,
                                ThunarThumbnailSize thumbnail_size)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  /* if the thumbstate is known to be not there, return null */
  if (thunar_file_get_thumb_state (file, thumbnail_size) == THUNAR_FILE_THUMB_STATE_NONE)
    return NULL;

  /* cache the real thumbnail path */
  if (G_UNLIKELY (file->thumbnail_path[thumbnail_size] == NULL))
    file->thumbnail_path[thumbnail_size] = thunar_file_get_thumbnail_path_real (file, thumbnail_size);

  return file->thumbnail_path[thumbnail_size];
}



/**
 * thunar_file_get_thumb_state:
 * @file : a #ThunarFile.
 * @size : requested #ThunarThumbnailSize
 *
 * Returns the current #ThunarFileThumbState for @file. This
 * method is intended to be used by #ThunarIconFactory only.
 *
 * Return value: the #ThunarFileThumbState for @file.
 **/
ThunarFileThumbState
thunar_file_get_thumb_state (const ThunarFile   *file,
                             ThunarThumbnailSize size)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), THUNAR_FILE_THUMB_STATE_UNKNOWN);
  return file->thumbnail_state[size];
}



/**
 * thunar_file_get_custom_icon:
 * @file : a #ThunarFile instance.
 *
 * Queries the custom icon from @file if any, else %NULL is returned.
 * The custom icon can be either a themed icon name or an absolute path
 * to an icon file in the local file system.
 *
 * Return value: the custom icon for @file or %NULL.
 **/
const gchar *
thunar_file_get_custom_icon (const ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  return file->custom_icon_name;
}



/**
 * thunar_file_get_preview_icon:
 * @file : a #ThunarFile instance.
 *
 * Returns the preview icon for @file if any, else %NULL is returned.
 *
 * Return value: the custom icon for @file or %NULL, the GIcon is owner
 * by the file, so do not unref it.
 **/
GIcon *
thunar_file_get_preview_icon (const ThunarFile *file)
{
  GObject *icon;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->info == NULL)
    return NULL;

  icon = g_file_info_get_attribute_object (file->info, G_FILE_ATTRIBUTE_PREVIEW_ICON);
  if (G_LIKELY (icon != NULL))
    return G_ICON (icon);

  return NULL;
}



static const gchar *
thunar_file_get_icon_name_for_state (const gchar        *icon_name,
                                     ThunarFileIconState icon_state)
{
  if (xfce_str_is_empty (icon_name))
    return NULL;

  /* check if we have an accept icon for the icon we found */
  if (icon_state != THUNAR_FILE_ICON_STATE_DEFAULT
      && (strcmp (icon_name, "inode-directory") == 0
          || strcmp (icon_name, "folder") == 0))
    {
      if (icon_state == THUNAR_FILE_ICON_STATE_DROP)
        return "folder-drag-accept";
      else if (icon_state == THUNAR_FILE_ICON_STATE_OPEN)
        return "folder-open";
    }

  return icon_name;
}



/**
 * thunar_file_get_icon_name:
 * @file       : a #ThunarFile instance.
 * @icon_state : the state of the @file<!---->s icon we are interested in.
 * @icon_theme : the #GtkIconTheme on which to lookup up the icon name.
 *
 * Returns the name of the icon that can be used to present @file, based
 * on the given @icon_state and @icon_theme.
 *
 * Return value: the icon name for @file in @icon_theme.
 **/
const gchar *
thunar_file_get_icon_name (ThunarFile         *file,
                           ThunarFileIconState icon_state,
                           GtkIconTheme       *icon_theme)
{
  GFile              *icon_file;
  GIcon              *icon = NULL;
  const gchar *const *names;
  gchar              *icon_name = NULL;
  gchar              *path;
  const gchar        *special_names[] = { NULL, "folder", NULL };
  guint               i;
  const gchar        *special_dir;
  GFileInfo          *fileinfo;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);
  _thunar_return_val_if_fail (GTK_IS_ICON_THEME (icon_theme), NULL);

  /* return cached name */
  if (G_LIKELY (file->icon_name != NULL))
    return thunar_file_get_icon_name_for_state (file->icon_name, icon_state);

  /* the system root folder has a special icon */
  if (thunar_file_is_directory (file))
    {
      if (G_LIKELY (thunar_file_is_local (file)))
        {
          path = g_file_get_path (file->gfile);
          if (G_LIKELY (path != NULL))
            {
              if (strcmp (path, G_DIR_SEPARATOR_S) == 0)
                *special_names = "drive-harddisk";
              else if (strcmp (path, xfce_get_homedir ()) == 0)
                *special_names = "user-home";
              else
                {
                  for (i = 0; i < G_N_ELEMENTS (thunar_file_dirs); i++)
                    {
                      special_dir = g_get_user_special_dir (thunar_file_dirs[i].type);
                      if (special_dir != NULL
                          && strcmp (path, special_dir) == 0)
                        {
                          *special_names = thunar_file_dirs[i].icon_name;
                          break;
                        }
                    }
                }

              g_free (path);
            }
        }
      else if (!thunar_file_has_parent (file))
        {
          if (g_file_has_uri_scheme (file->gfile, "trash"))
            {
              special_names[0] = thunar_file_get_trash_item_count (file) > 0 ? "user-trash-full" : "user-trash";
              special_names[1] = "user-trash";
            }
          else if (g_file_has_uri_scheme (file->gfile, "network"))
            {
              special_names[0] = "network-workgroup";
            }
          else if (g_file_has_uri_scheme (file->gfile, "recent"))
            {
              special_names[0] = "document-open-recent";
            }
          else if (g_file_has_uri_scheme (file->gfile, "computer"))
            {
              special_names[0] = "computer";
            }
        }

      if (*special_names != NULL)
        {
          names = special_names;
          goto check_names;
        }
    }
  else if (thunar_file_is_mountable (file)
           || g_file_has_uri_scheme (file->gfile, "network"))
    {
      /* query the icon (computer:// and network:// backend) */
      fileinfo = g_file_query_info (file->gfile,
                                    G_FILE_ATTRIBUTE_STANDARD_ICON,
                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
      if (G_LIKELY (fileinfo != NULL))
        {
          /* take the icon from the info */
          icon = G_ICON (g_file_info_get_attribute_object (fileinfo, G_FILE_ATTRIBUTE_STANDARD_ICON));
          if (G_LIKELY (icon != NULL))
            g_object_ref (icon);

          /* release */
          g_object_unref (G_OBJECT (fileinfo));

          if (G_LIKELY (icon != NULL))
            goto check_icon;
        }
    }

  /* no special icon required and we have a folder? --> use the default folder icon */
  if (file->kind == G_FILE_TYPE_DIRECTORY && gtk_icon_theme_has_icon (icon_theme, "folder"))
    {
      file->icon_name = g_strdup ("folder");
      return thunar_file_get_icon_name_for_state (file->icon_name, icon_state);
    }

  /* try again later */
  if (file->info == NULL)
    return NULL;

  /* lookup for content type, just like gio does for local files */
  icon = g_content_type_get_icon (thunar_file_get_content_type (file));
  if (G_LIKELY (icon != NULL))
    {
check_icon:
      if (G_IS_THEMED_ICON (icon))
        {
          names = g_themed_icon_get_names (G_THEMED_ICON (icon));

check_names:

          if (G_LIKELY (names != NULL))
            {
              for (i = 0; names[i] != NULL; ++i)
                if (*names[i] != '(' /* see gnome bug 688042 */
                    && gtk_icon_theme_has_icon (icon_theme, names[i]))
                  {
                    icon_name = g_strdup (names[i]);
                    break;
                  }
            }
        }
      else if (G_IS_FILE_ICON (icon))
        {
          icon_file = g_file_icon_get_file (G_FILE_ICON (icon));
          if (icon_file != NULL)
            icon_name = g_file_get_path (icon_file);
        }

      if (G_LIKELY (icon != NULL))
        g_object_unref (icon);
    }

  /* store new name, fallback to empty string to avoid recursion */
  g_free (file->icon_name);
  if (G_LIKELY (icon_name != NULL))
    file->icon_name = icon_name;
  else
    file->icon_name = g_strdup ("");

  return thunar_file_get_icon_name_for_state (file->icon_name, icon_state);
}



/**
 * thunar_file_get_device_type:
 * @file : a #ThunarFile instance.
 *
 * Returns : (transfer none) (nullable): the string of the device type.
 */
const gchar *
thunar_file_get_device_type (ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), NULL);

  if (file->kind != G_FILE_TYPE_MOUNTABLE)
    return NULL;

  if (G_LIKELY (file->device_type != NULL))
    return file->device_type;

  file->device_type = thunar_g_file_guess_device_type (file->gfile);
  return file->device_type;
}



/**
 * thunar_file_watch:
 * @file : a #ThunarFile instance.
 *
 * Tells @file to watch itself for changes. Not all #ThunarFile
 * implementations must support this, but if a #ThunarFile
 * implementation implements the thunar_file_watch() method,
 * it must also implement the thunar_file_unwatch() method.
 *
 * The #ThunarFile base class implements automatic "ref
 * counting" for watches, that says, you can call thunar_file_watch()
 * multiple times, but the virtual method will be invoked only
 * once. This also means that you MUST call thunar_file_unwatch()
 * for every thunar_file_watch() invokation, else the application
 * will abort.
 **/
void
thunar_file_watch (ThunarFile *file)
{
  ThunarFileWatch *file_watch;
  GError          *error = NULL;

  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  file_watch = g_object_get_qdata (G_OBJECT (file), thunar_file_watch_quark);
  if (file_watch == NULL)
    {
      file_watch = g_slice_new (ThunarFileWatch);
      file_watch->monitor = NULL;
      file_watch->watch_count = 0;

      if (thunar_file_watch_total_count < THUNAR_FILE_WATCH_MAX)
        {
          /* try to create a file or directory monitor */
          file_watch->monitor = g_file_monitor (file->gfile, G_FILE_MONITOR_WATCH_MOUNTS, NULL, &error);

          if (G_UNLIKELY (file_watch->monitor == NULL))
            {
              g_debug ("Failed to create file monitor: %s", error->message);
              g_error_free (error);
            }
          else
            {
              /* watch monitor for file changes */
              g_signal_connect (file_watch->monitor, "changed", G_CALLBACK (thunar_file_monitor), file);
              thunar_file_watch_total_count++;
              if (thunar_file_watch_total_count == THUNAR_FILE_WATCH_MAX)
                g_message ("Maximum number of monitored ThunarFiles reached. Creation of additional FileMonitors will be skipped.");
            }
        }

      /* attach to file */
      g_object_set_qdata_full (G_OBJECT (file), thunar_file_watch_quark, file_watch, thunar_file_watch_destroyed);
    }

  /* increase watch count */
  file_watch->watch_count++;
}



/**
 * thunar_file_unwatch:
 * @file : a #ThunarFile instance.
 *
 * See thunar_file_watch() for a description of how watching
 * #ThunarFile<!---->s works.
 **/
void
thunar_file_unwatch (ThunarFile *file)
{
  ThunarFileWatch *file_watch;

  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  file_watch = g_object_get_qdata (G_OBJECT (file), thunar_file_watch_quark);
  if (file_watch != NULL)
    {
      /* remove if this was the last ref */
      if (--file_watch->watch_count == 0)
        g_object_set_qdata (G_OBJECT (file), thunar_file_watch_quark, NULL);
    }
  else
    {
      _thunar_assert_not_reached ();
    }
}



/**
 * thunar_file_reload:
 * @file : a #ThunarFile instance.
 *
 * Tells @file to reload its internal state, e.g. by reacquiring
 * the file info from the underlying media.
 *
 * If no fail information can be obtained, a destroy signal will be send,
 * so that reference holders can release it
 *
 * Return value: TRUE on success, FALSE otherwise
 **/
gboolean
thunar_file_reload (ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  /* clear file pxmap cache */
  thunar_icon_factory_clear_pixmap_cache (file);

  if (!thunar_file_load (file, NULL, NULL))
    {
      /* send destroy signal for the file if we cannot query any file information */
      thunar_file_signal_destroy (file);
      return FALSE;
    }

  /* ... and tell others */
  thunar_file_changed (file);

  return TRUE;
}



static gboolean
thunar_file_reload_cb_once (gpointer user_data)
{
  ThunarFile *file = user_data;

  thunar_file_reload (file);
  return FALSE;
}



/**
 * thunar_file_reload_idle:
 * @file : a #ThunarFile instance.
 *
 * Schedules a single reload of the @file by calling thunar_file_reload
 * when idle.
 *
 **/
void
thunar_file_reload_idle (ThunarFile *file)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  g_idle_add (thunar_file_reload_cb_once, file);
}



/**
 * thunar_file_reload_idle_unref:
 * @file : a #ThunarFile instance.
 *
 * Schedules a reload of the @file by calling thunar_file_reload
 * when idle. When scheduled function returns @file object will be
 * unreferenced.
 *
 **/
void
thunar_file_reload_idle_unref (ThunarFile *file)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                   thunar_file_reload_cb_once,
                   file,
                   (GDestroyNotify) g_object_unref);
}



/**
 * thunar_file_signal_destroy:
 * @file : a #ThunarFile instance.
 *
 * Emits the ::destroy signal notifying all reference holders
 * that they should release their references to the @file, so that it can be finallized.
 *
 **/
void
thunar_file_signal_destroy (ThunarFile *file)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  if (!FLAG_IS_SET (file, THUNAR_FILE_FLAG_IN_DESTRUCTION))
    g_signal_emit (file, file_signals[DESTROY], 0);
}



/**
 * thunar_file_compare_by_type:
 * @a : the first #ThunarFile.
 * @b : the second #ThunarFile.
 *
 * Compares items so that directories come before files and ancestors come
 * before descendants.
 *
 * Return value: -1 if @file_a should be sorted before @file_b, 1 if
 *               @file_b should be sorted before @file_a, 0 if equal.
 **/
gint
thunar_file_compare_by_type (ThunarFile *a,
                             ThunarFile *b)
{
  GFile *file_a;
  GFile *file_b;
  GFile *parent_a;
  GFile *parent_b;
  gint   ret = 0;

  file_a = thunar_file_get_file (a);
  file_b = thunar_file_get_file (b);

  /* check whether the files are equal */
  if (g_file_equal (file_a, file_b))
    return 0;

  /* directories always come first */
  if (a->kind == G_FILE_TYPE_DIRECTORY
      && b->kind != G_FILE_TYPE_DIRECTORY)
    {
      return -1;
    }
  if (a->kind != G_FILE_TYPE_DIRECTORY
      && b->kind == G_FILE_TYPE_DIRECTORY)
    {
      return 1;
    }

  /* ancestors come first */
  if (g_file_has_prefix (file_b, file_a))
    return -1;
  if (g_file_has_prefix (file_a, file_b))
    return 1;

  parent_a = g_file_get_parent (file_a);
  parent_b = g_file_get_parent (file_b);

  if (g_file_equal (parent_a, parent_b))
    {
      /* compare siblings by their display name */
      ret = g_utf8_collate (a->display_name,
                            b->display_name);
    }
  /* again, ancestors come first */
  else if (g_file_has_prefix (file_b, parent_a))
    ret = -1;
  else if (g_file_has_prefix (file_a, parent_b))
    ret = 1;

  g_object_unref (parent_a);
  g_object_unref (parent_b);
  return ret;
}



/**
 * thunar_file_compare_by_name:
 * @file_a         : the first #ThunarFile.
 * @file_b         : the second #ThunarFile.
 * @case_sensitive : whether the comparison should be case-sensitive.
 *
 * Compares @file_a and @file_b by their display names. If @case_sensitive
 * is %TRUE the comparison will be case-sensitive.
 *
 * Return value: -1 if @file_a should be sorted before @file_b, 1 if
 *               @file_b should be sorted before @file_a, 0 if equal.
 **/
gint
thunar_file_compare_by_name (const ThunarFile *file_a,
                             const ThunarFile *file_b,
                             gboolean          case_sensitive)
{
  gint result = 0;

#ifdef G_ENABLE_DEBUG
  /* probably too expensive to do the instance check every time
   * this function is called, so only for debugging builds.
   */
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_a), 0);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_b), 0);
#endif

  /* case insensitive checking */
  if (G_LIKELY (!case_sensitive))
    result = g_strcmp0 (file_a->collate_key_nocase, file_b->collate_key_nocase);

  /* fall-back to case sensitive */
  if (result == 0)
    result = g_strcmp0 (file_a->collate_key, file_b->collate_key);

  /* this happens in the trash */
  if (result == 0)
    {
      result = g_strcmp0 (thunar_file_get_original_path (file_a),
                          thunar_file_get_original_path (file_b));
    }

  return result;
}



static gboolean
thunar_file_same_filesystem (const ThunarFile *file_a,
                             const ThunarFile *file_b)
{
  const gchar *filesystem_id_a;
  const gchar *filesystem_id_b;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_a), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file_b), FALSE);

  /* return false if we have no information about one of the files */
  if (file_a->info == NULL || file_b->info == NULL)
    return FALSE;

  /* determine the filesystem IDs */
  filesystem_id_a = g_file_info_get_attribute_string (file_a->info,
                                                      G_FILE_ATTRIBUTE_ID_FILESYSTEM);

  filesystem_id_b = g_file_info_get_attribute_string (file_b->info,
                                                      G_FILE_ATTRIBUTE_ID_FILESYSTEM);

  /* compare the filesystem IDs */
  return (g_strcmp0 (filesystem_id_a, filesystem_id_b) == 0);
}



/**
 * thunar_file_cache_lookup:
 * @file : a #GFile.
 *
 * Looks up the #ThunarFile for @file in the internal file
 * cache and returns the file present for @file in the
 * cache or %NULL if no #ThunarFile is cached for @file.
 *
 * Note that no reference is taken for the caller.
 *
 * This method should not be used but in very rare cases.
 * Consider using thunar_file_get() instead.
 *
 * Return value: the #ThunarFile for @file in the internal
 *               cache, or %NULL. If you are done with the
 *               file, use g_object_unref to release.
 **/
ThunarFile *
thunar_file_cache_lookup (const GFile *file)
{
  GWeakRef   *ref;
  ThunarFile *cached_file;

  _thunar_return_val_if_fail (G_IS_FILE (file), NULL);

  G_REC_LOCK (file_cache_mutex);

  /* allocate the ThunarFile cache on-demand */
  if (G_UNLIKELY (file_cache == NULL))
    {
      file_cache = g_hash_table_new_full (g_file_hash,
                                          (GEqualFunc) g_file_equal,
                                          (GDestroyNotify) g_object_unref,
                                          (GDestroyNotify) weak_ref_free);
    }

  ref = g_hash_table_lookup (file_cache, file);

  if (ref == NULL)
    cached_file = NULL;
  else
    cached_file = g_weak_ref_get (ref);

  G_REC_UNLOCK (file_cache_mutex);

  return cached_file;
}



gchar *
thunar_file_cached_display_name (const GFile *file)
{
  ThunarFile *cached_file;
  gchar      *display_name;

  /* check if we have a ThunarFile for it in the cache (usually is the case) */
  cached_file = thunar_file_cache_lookup (file);
  if (cached_file != NULL)
    {
      /* determine the display name of the file */
      display_name = g_strdup (thunar_file_get_display_name (cached_file));
      g_object_unref (cached_file);
    }
  else
    {
      /* determine something a hopefully good approximation of the display name */
      display_name = thunar_g_file_get_display_name (G_FILE (file));
    }

  return display_name;
}



static gint
compare_app_infos (gconstpointer a,
                   gconstpointer b)
{
  return g_app_info_equal (G_APP_INFO (a), G_APP_INFO (b)) ? 0 : 1;
}



/**
 * thunar_file_list_get_applications:
 * @file_list : a #GList of #ThunarFile<!---->s.
 *
 * Returns the #GList of #GAppInfo<!---->s that can be used to open
 * all #ThunarFile<!---->s in the given @file_list.
 *
 * The caller is responsible to free the returned list using something like:
 * <informalexample><programlisting>
 * g_list_free_full (list, g_object_unref);
 * </programlisting></informalexample>
 *
 * Return value: the list of #GAppInfo<!---->s that can be used to open all
 *               items in the @file_list.
 **/
GList *
thunar_file_list_get_applications (GList *file_list)
{
  GList       *applications = NULL;
  GList       *list;
  GList       *next;
  GList       *ap;
  GList       *lp;
  GAppInfo    *default_application;
  const gchar *previous_type = NULL;
  const gchar *current_type;

  /* determine the set of applications that can open all files */
  for (lp = file_list; lp != NULL; lp = lp->next)
    {
      current_type = thunar_file_get_content_type (lp->data);

      /* no need to check anything if this file has the same mimetype as the previous file */
      if (current_type != NULL && previous_type != NULL)
        if (G_LIKELY (g_content_type_equals (previous_type, current_type)))
          continue;

      /* store the previous type */
      previous_type = current_type;

      /* determine the list of applications that can open this file */
      if (G_UNLIKELY (current_type != NULL))
        {
          list = g_app_info_get_all_for_type (current_type);

          /* move any default application in front of the list */
          default_application = g_app_info_get_default_for_type (current_type, FALSE);
          if (G_LIKELY (default_application != NULL))
            {
              for (ap = list; ap != NULL; ap = ap->next)
                {
                  if (g_app_info_equal (ap->data, default_application))
                    {
                      g_object_unref (ap->data);
                      list = g_list_delete_link (list, ap);
                      break;
                    }
                }
              list = g_list_prepend (list, default_application);
            }
        }
      else
        list = NULL;

      if (G_UNLIKELY (applications == NULL))
        {
          /* first file, so just use the applications list */
          applications = list;
        }
      else
        {
          /* keep only the applications that are also present in list */
          for (ap = applications; ap != NULL; ap = next)
            {
              /* grab a pointer on the next application */
              next = ap->next;

              /* check if the application is present in list */
              if (g_list_find_custom (list, ap->data, compare_app_infos) == NULL)
                {
                  /* drop our reference on the application */
                  g_object_unref (G_OBJECT (ap->data));

                  /* drop this application from the list */
                  applications = g_list_delete_link (applications, ap);
                }
            }

          /* release the list of applications for this file */
          g_list_free_full (list, g_object_unref);
        }

      /* check if the set is still not empty */
      if (G_LIKELY (applications == NULL))
        break;
    }

  /* remove hidden applications */
  for (ap = applications; ap != NULL; ap = next)
    {
      /* grab a pointer on the next application */
      next = ap->next;

      if (!thunar_g_app_info_should_show (ap->data))
        {
          /* drop our reference on the application */
          g_object_unref (G_OBJECT (ap->data));

          /* drop this application from the list */
          applications = g_list_delete_link (applications, ap);
        }
    }

  return applications;
}



/**
 * thunar_file_list_to_thunar_g_file_list:
 * @file_list : a #GList of #ThunarFile<!---->s.
 *
 * Transforms the @file_list to a #GList of #GFile<!---->s for
 * the #ThunarFile<!---->s contained within @file_list.
 *
 * The caller is responsible to free the returned list using
 * thunar_g_list_free_full() when no longer needed.
 *
 * Return value: the list of #GFile<!---->s for @file_list.
 **/
GList *
thunar_file_list_to_thunar_g_file_list (GList *file_list)
{
  GList *list = NULL;
  GList *lp;

  for (lp = g_list_last (file_list); lp != NULL; lp = lp->prev)
    list = g_list_prepend (list, g_object_ref (THUNAR_FILE (lp->data)->gfile));

  return list;
}



/**
 * thunar_file_get_metadata_setting:
 * @file         : a #ThunarFile instance.
 * @setting_name : the name of the setting to get
 *
 * Gets the stored value of the metadata setting @setting_name for @file. Returns %NULL
 * if there is no stored setting. Release with g_free agfter usage.
 *
 * Return value: (transfer full): the stored value of the setting for @file, or %NULL
 **/
gchar *
thunar_file_get_metadata_setting (ThunarFile  *file,
                                  const gchar *setting_name)
{
  return thunar_g_file_get_metadata_setting (file->gfile, file->info, THUNAR_GTYPE_STRING, setting_name);
}



/**
 * thunar_file_set_metadata_setting:
 * @file          : a #ThunarFile instance.
 * @setting_name  : the name of the setting to set
 * @setting_value : the value to set
 * @async         : whether g_file_set_attributes_async or g_file_set_attributes_from_info should be used
 *
 * Sets the setting @setting_name of @file to @setting_value and stores it in
 * the @file<!---->s metadata.
 **/
void
thunar_file_set_metadata_setting (ThunarFile  *file,
                                  const gchar *setting_name,
                                  const gchar *setting_value,
                                  gboolean     async)
{
  return thunar_g_file_set_metadata_setting (file->gfile, file->info, THUNAR_GTYPE_STRING, setting_name, setting_value, async);
}



/**
 * thunar_file_clear_metadata_setting:
 * @file          : a #ThunarFile instance.
 * @setting_name  : the name of the setting to clear
 *
 * Clear the metadata setting @setting_name of @file
 **/
void
thunar_file_clear_metadata_setting (ThunarFile  *file,
                                    const gchar *setting_name)
{
  return thunar_g_file_clear_metadata_setting (file->gfile, file->info, setting_name);
}



/**
 * thunar_file_clear_directory_specific_settings:
 * @file : a #ThunarFile instance.
 *
 * Clears all directory specific settings stored in the metadata of the folder represented by @file
 **/
void
thunar_file_clear_directory_specific_settings (ThunarFile *file)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  if (file->info == NULL)
    return;

  g_file_info_remove_attribute (file->info, "metadata::thunar-view-type");
  g_file_info_remove_attribute (file->info, "metadata::thunar-sort-column");
  g_file_info_remove_attribute (file->info, "metadata::thunar-sort-folders-first");
  g_file_info_remove_attribute (file->info, "metadata::thunar-sort-order");
  g_file_info_remove_attribute (file->info, "metadata::thunar-zoom-level");
  g_file_info_remove_attribute (file->info, "metadata::thunar-zoom-level-ThunarDetailsView");
  g_file_info_remove_attribute (file->info, "metadata::thunar-zoom-level-ThunarIconView");
  g_file_info_remove_attribute (file->info, "metadata::thunar-zoom-level-ThunarCompactView");

  g_file_set_attribute (file->gfile, "metadata::thunar-view-type", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-sort-column", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-sort-folders-first", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-sort-order", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-zoom-level", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-zoom-level-ThunarDetailsView", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-zoom-level-ThunarIconView", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_file_set_attribute (file->gfile, "metadata::thunar-zoom-level-ThunarCompactView", G_FILE_ATTRIBUTE_TYPE_INVALID,
                        NULL, G_FILE_QUERY_INFO_NONE, NULL, NULL);

  thunar_file_changed (file);
}



/**
 * thunar_file_has_directory_specific_settings:
 * @file : a #ThunarFile instance.
 *
 * Checks whether @file has any directory specific settings stored in its metadata.
 *
 * Return value: %TRUE if @file has any directory specific settings stored in its metadata, and %FALSE otherwise
 **/
gboolean
thunar_file_has_directory_specific_settings (ThunarFile *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  if (file->info == NULL)
    return FALSE;

  if (g_file_info_has_attribute (file->info, "metadata::thunar-view-type"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-sort-column"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-sort-folders-first"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-sort-order"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-zoom-level"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-zoom-level-ThunarDetailsView"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-zoom-level-ThunarIconView"))
    return TRUE;
  if (g_file_info_has_attribute (file->info, "metadata::thunar-zoom-level-ThunarCompactView"))
    return TRUE;

  return FALSE;
}



gint
thunar_cmp_files_by_date (const ThunarFile  *a,
                          const ThunarFile  *b,
                          gboolean           case_sensitive,
                          ThunarFileDateType type)
{
  guint64 date_a;
  guint64 date_b;

  date_a = thunar_file_get_date (a, type);
  date_b = thunar_file_get_date (b, type);

  if (date_a < date_b)
    return -1;
  else if (date_a > date_b)
    return 1;

  return thunar_file_compare_by_name (a, b, case_sensitive);
}



gint
thunar_cmp_files_by_date_created (const ThunarFile *a,
                                  const ThunarFile *b,
                                  gboolean          case_sensitive)
{
  return thunar_cmp_files_by_date (a, b, case_sensitive, THUNAR_FILE_DATE_CREATED);
}



gint
thunar_cmp_files_by_date_accessed (const ThunarFile *a,
                                   const ThunarFile *b,
                                   gboolean          case_sensitive)
{
  return thunar_cmp_files_by_date (a, b, case_sensitive, THUNAR_FILE_DATE_ACCESSED);
}



gint
thunar_cmp_files_by_date_modified (const ThunarFile *a,
                                   const ThunarFile *b,
                                   gboolean          case_sensitive)
{
  return thunar_cmp_files_by_date (a, b, case_sensitive, THUNAR_FILE_DATE_MODIFIED);
}



gint
thunar_cmp_files_by_date_deleted (const ThunarFile *a,
                                  const ThunarFile *b,
                                  gboolean          case_sensitive)
{
  return thunar_cmp_files_by_date (a, b, case_sensitive, THUNAR_FILE_DATE_DELETED);
}



gint
thunar_cmp_files_by_recency (const ThunarFile *a,
                             const ThunarFile *b,
                             gboolean          case_sensitive)
{
  return thunar_cmp_files_by_date (a, b, case_sensitive, THUNAR_FILE_RECENCY);
}



gint
thunar_cmp_files_by_location (const ThunarFile *a,
                              const ThunarFile *b,
                              gboolean          case_sensitive)
{
  gchar *uri_a;
  gchar *uri_b;
  gchar *location_a;
  gchar *location_b;
  gint   result;

  uri_a = thunar_file_dup_uri (a);
  uri_b = thunar_file_dup_uri (b);

  location_a = g_path_get_dirname (uri_a);
  location_b = g_path_get_dirname (uri_b);

  result = strcasecmp (location_a, location_b);

  g_free (uri_a);
  g_free (uri_b);
  g_free (location_a);
  g_free (location_b);

  return result;
}



gint
thunar_cmp_files_by_group (const ThunarFile *a,
                           const ThunarFile *b,
                           gboolean          case_sensitive)
{
  ThunarGroup *group_a;
  ThunarGroup *group_b;
  guint32      gid_a;
  guint32      gid_b;
  gint         result;
  const gchar *name_a;
  const gchar *name_b;

  if (thunar_file_get_info (a) == NULL || thunar_file_get_info (b) == NULL)
    return thunar_file_compare_by_name (a, b, case_sensitive);

  group_a = thunar_file_get_group (a);
  group_b = thunar_file_get_group (b);

  if (group_a != NULL && group_b != NULL)
    {
      name_a = thunar_group_get_name (group_a);
      name_b = thunar_group_get_name (group_b);

      if (!case_sensitive)
        result = strcasecmp (name_a, name_b);
      else
        result = strcmp (name_a, name_b);
    }
  else
    {
      gid_a = g_file_info_get_attribute_uint32 (thunar_file_get_info (a),
                                                G_FILE_ATTRIBUTE_UNIX_GID);
      gid_b = g_file_info_get_attribute_uint32 (thunar_file_get_info (b),
                                                G_FILE_ATTRIBUTE_UNIX_GID);

      result = CLAMP ((gint) gid_a - (gint) gid_b, -1, 1);
    }

  if (group_a != NULL)
    g_object_unref (group_a);

  if (group_b != NULL)
    g_object_unref (group_b);

  if (result == 0)
    return thunar_file_compare_by_name (a, b, case_sensitive);
  else
    return result;
}



gint
thunar_cmp_files_by_mime_type (const ThunarFile *a,
                               const ThunarFile *b,
                               gboolean          case_sensitive)
{
  const gchar *content_type_a;
  const gchar *content_type_b;
  gint         result;

  content_type_a = thunar_file_get_content_type (THUNAR_FILE (a));
  content_type_b = thunar_file_get_content_type (THUNAR_FILE (b));

  if (content_type_a == NULL)
    content_type_a = "";
  if (content_type_b == NULL)
    content_type_b = "";

  result = strcasecmp (content_type_a, content_type_b);

  if (result == 0)
    result = thunar_file_compare_by_name (a, b, case_sensitive);

  return result;
}



gint
thunar_cmp_files_by_owner (const ThunarFile *a,
                           const ThunarFile *b,
                           gboolean          case_sensitive)
{
  const gchar *name_a;
  const gchar *name_b;
  ThunarUser  *user_a;
  ThunarUser  *user_b;
  guint32      uid_a;
  guint32      uid_b;
  gint         result;

  if (thunar_file_get_info (a) == NULL || thunar_file_get_info (b) == NULL)
    return thunar_file_compare_by_name (a, b, case_sensitive);

  user_a = thunar_file_get_user (a);
  user_b = thunar_file_get_user (b);

  if (user_a != NULL && user_b != NULL)
    {
      /* compare the system names */
      name_a = thunar_user_get_name (user_a);
      name_b = thunar_user_get_name (user_b);

      if (!case_sensitive)
        result = strcasecmp (name_a, name_b);
      else
        result = strcmp (name_a, name_b);
    }
  else
    {
      uid_a = g_file_info_get_attribute_uint32 (thunar_file_get_info (a),
                                                G_FILE_ATTRIBUTE_UNIX_UID);
      uid_b = g_file_info_get_attribute_uint32 (thunar_file_get_info (b),
                                                G_FILE_ATTRIBUTE_UNIX_UID);

      result = CLAMP ((gint) uid_a - (gint) uid_b, -1, 1);
    }

  if (user_a != NULL)
    g_object_unref (user_a);
  if (user_b != NULL)
    g_object_unref (user_b);

  if (result == 0)
    return thunar_file_compare_by_name (a, b, case_sensitive);
  else
    return result;
}



gint
thunar_cmp_files_by_permissions (const ThunarFile *a,
                                 const ThunarFile *b,
                                 gboolean          case_sensitive)
{
  ThunarFileMode mode_a;
  ThunarFileMode mode_b;

  mode_a = thunar_file_get_mode (a);
  mode_b = thunar_file_get_mode (b);

  if (mode_a < mode_b)
    return -1;
  else if (mode_a > mode_b)
    return 1;

  return thunar_file_compare_by_name (a, b, case_sensitive);
}



gint
thunar_cmp_files_by_size (const ThunarFile *a,
                          const ThunarFile *b,
                          gboolean          case_sensitive)
{
  guint64 size_a;
  guint64 size_b;

  size_a = thunar_file_get_size (a);
  size_b = thunar_file_get_size (b);

  if (size_a < size_b)
    return -1;
  else if (size_a > size_b)
    return 1;

  return thunar_file_compare_by_name (a, b, case_sensitive);
}



gint
thunar_cmp_files_by_size_in_bytes (const ThunarFile *a,
                                   const ThunarFile *b,
                                   gboolean          case_sensitive)
{
  return thunar_cmp_files_by_size (a, b, case_sensitive);
}



gint
thunar_cmp_files_by_size_and_items_count (ThunarFile *a,
                                          ThunarFile *b,
                                          gboolean    case_sensitive)
{
  gint32 count_a;
  gint32 count_b;

  if (thunar_file_is_directory (a) && thunar_file_is_directory (b))
    {
      count_a = thunar_file_get_file_count (a, NULL, NULL);
      count_b = thunar_file_get_file_count (b, NULL, NULL);

      if (count_a < count_b)
        return -1;
      else if (count_a > count_b)
        return 1;
      else
        return thunar_file_compare_by_name (a, b, case_sensitive);
    }

  return thunar_cmp_files_by_size (a, b, case_sensitive);
}



gint
thunar_cmp_files_by_type (const ThunarFile *a,
                          const ThunarFile *b,
                          gboolean          case_sensitive)
{
  gchar *description_a = NULL;
  gchar *description_b = NULL;
  gint   result;

  /* fetch the content type description for @file(s) a & b */
  description_a = thunar_file_get_content_type_desc (THUNAR_FILE (a), TRUE);
  description_b = thunar_file_get_content_type_desc (THUNAR_FILE (b), TRUE);

  /* avoid calling strcasecmp with NULL parameters */
  if (description_a == NULL || description_b == NULL)
    {
      g_free (description_a);
      g_free (description_b);

      return 0;
    }

  if (!case_sensitive)
    result = strcasecmp (description_a, description_b);
  else
    result = strcmp (description_a, description_b);

  g_free (description_a);
  g_free (description_b);

  if (result == 0)
    return thunar_file_compare_by_name (a, b, case_sensitive);
  else
    return result;
}



/**
 * thunar_file_update_thumbnail:
 * @file        : a #ThunarFile.
 * @thumb_state : the new #ThunarFileThumbState.
 * @thumb_size  : the required #ThunarThumbnailSize
 *
 * Sets the #ThunarFileThumbState for @file to @thumb_state, loads the thumbnail path and triggers a redraw, if required
 **/
void
thunar_file_update_thumbnail (ThunarFile          *file,
                              ThunarFileThumbState state,
                              ThunarThumbnailSize  size)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  /* check if the state changes */
  if (thunar_file_get_thumb_state (file, size) == state)
    return;

  /* set the new thumbnail state */
  file->thumbnail_state[size] = state;

  /* Either there was some thumbnailing error for that file, or thumbnailing is just not supported for it */
  if (state == THUNAR_FILE_THUMB_STATE_NONE)
    {
      g_free (file->thumbnail_path[size]);
      file->thumbnail_path[size] = NULL;
      return;
    }

  if (state == THUNAR_FILE_THUMB_STATE_READY)
    {
      /* Try to set the internal path, so the thumbnail can be loaded from it */
      thunar_file_get_thumbnail_path (file, size);

      if (file->thumbnail_path[size] == NULL)
        {
          g_warning ("Error: Thumbnailing for '%s' signaled ready, but no thumbnail was generated", thunar_file_get_basename (file));

          /* For some reason thumbnailing seems not to always work reliably when multiple thumbnails are requested in parallel (e.g. in different size for the same file) */
          /* If there was no thumbnailing error for the file (THUNAR_FILE_THUMB_STATE_NONE), allow to send another request for that file */
          file->thumbnail_state[size] = THUNAR_FILE_THUMB_STATE_UNKNOWN;
          return;
        }
    }

  thunar_icon_factory_clear_pixmap_cache (file);

  g_signal_emit (file, file_signals[THUMBNAIL_UPDATED], 0, size);
}



static void
thunar_file_thumbnailing_finished (ThunarFile        *file,
                                   guint              request_id,
                                   ThunarThumbnailer *thumbnailer)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  for (gint i = 0; i < N_THUMBNAIL_SIZES; i++)
    {
      if (file->thumbnail_request_id[i] == request_id)
        {
          /* reset the request id */
          file->thumbnail_request_id[i] = 0;
          break;
        }
    }
}



void
thunar_file_request_thumbnail (ThunarFile         *file,
                               ThunarThumbnailSize size)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  /* For all other states, the thumbnailer already processed the file or is currently working on it */
  if (file->thumbnail_state[size] != THUNAR_FILE_THUMB_STATE_UNKNOWN)
    return;

  if (file->thumbnail_request_id[size] != 0)
    return;

  file->thumbnail_state[size] = THUNAR_FILE_THUMB_STATE_LOADING;

  thunar_thumbnailer_queue_file (file->thumbnailer, file, &file->thumbnail_request_id[size], size);
}



static void
thunar_file_reset_thumbnail (ThunarFile         *file,
                             ThunarThumbnailSize size)
{
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  /* Dont do anything if the thumbnailer is still working on it */
  if (file->thumbnail_request_id[size] != 0)
    return;

  file->thumbnail_state[size] = THUNAR_FILE_THUMB_STATE_UNKNOWN;
  g_free (file->thumbnail_path[size]);
  file->thumbnail_path[size] = NULL;
  file->thumbnail_request_id[size] = 0;
}



static gboolean
thunar_file_changed_signal_timeout (gpointer data)
{
  ThunarFile *file = THUNAR_FILE (data);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), G_SOURCE_REMOVE);

  /* emit the changed signal on thunarx level, if required */
  if (file->signal_change_requested)
    thunarx_file_info_changed (THUNARX_FILE_INFO (file));

  file->signal_change_requested = FALSE;
  file->signal_changed_source_id = 0;

  return G_SOURCE_REMOVE;
}



/**
 * thunar_file_changed:
 * @file : a #ThunarFile instance.
 *
 * Emits the ::changed signal on @file. This function is meant to be called
 * by derived classes whenever they notice changes to the @file.
 **/
void
thunar_file_changed (ThunarFile *file)
{
  /* in case a timeout is running, the change-signal will be delayed (performance) */
  if (file->signal_changed_source_id != 0)
    {
      file->signal_change_requested = TRUE;
      return;
    }

  /* handle the first request directly */
  thunarx_file_info_changed (THUNARX_FILE_INFO (file));

  /* start a timer to throttle consecutive calls */
  file->signal_changed_source_id = g_timeout_add (FILE_CHANGED_SIGNAL_RATE_LIMIT,
                                                  thunar_file_changed_signal_timeout, file);
}
