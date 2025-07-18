/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2004-2007 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2009-2011 Jannis Pohlmann <jannis@xfce.org>
 * Copyright (c) 2012      Nick Schermer <nick@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "thunar/thunar-application.h"
#include "thunar/thunar-file.h"
#include "thunar/thunar-gobject-extensions.h"
#include "thunar/thunar-io-jobs.h"
#include "thunar/thunar-list-model.h"
#include "thunar/thunar-preferences.h"
#include "thunar/thunar-private.h"
#include "thunar/thunar-simple-job.h"
#include "thunar/thunar-standard-view-model.h"
#include "thunar/thunar-user.h"
#include "thunar/thunar-util.h"

#include <libxfce4util/libxfce4util.h>



/* Property identifiers */
enum
{
  PROP_0,
  PROP_CASE_SENSITIVE,
  PROP_DATE_STYLE,
  PROP_DATE_CUSTOM_STYLE,
  PROP_FOLDER,
  PROP_FOLDERS_FIRST,
  PROP_HIDDEN_LAST,
  PROP_NUM_FILES,
  PROP_SHOW_HIDDEN,
  PROP_FOLDER_ITEM_COUNT,
  PROP_FILE_SIZE_BINARY,
  PROP_LOADING,
  N_PROPERTIES
};



static void
thunar_list_model_standard_view_model_init (ThunarStandardViewModelIface *iface);
static void
thunar_list_model_tree_model_init (GtkTreeModelIface *iface);
static void
thunar_list_model_drag_dest_init (GtkTreeDragDestIface *iface);
static void
thunar_list_model_sortable_init (GtkTreeSortableIface *iface);
static void
thunar_list_model_dispose (GObject *object);
static void
thunar_list_model_finalize (GObject *object);
static void
thunar_list_model_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec);
static void
thunar_list_model_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec);
static GtkTreeModelFlags
thunar_list_model_get_flags (GtkTreeModel *model);
static gint
thunar_list_model_get_n_columns (GtkTreeModel *model);
static GType
thunar_list_model_get_column_type (GtkTreeModel *model,
                                   gint          idx);
static gboolean
thunar_list_model_get_iter (GtkTreeModel *model,
                            GtkTreeIter  *iter,
                            GtkTreePath  *path);
static GtkTreePath *
thunar_list_model_get_path (GtkTreeModel *model,
                            GtkTreeIter  *iter);
static void
thunar_list_model_get_value (GtkTreeModel *model,
                             GtkTreeIter  *iter,
                             gint          column,
                             GValue       *value);
static gboolean
thunar_list_model_iter_next (GtkTreeModel *model,
                             GtkTreeIter  *iter);
static gboolean
thunar_list_model_iter_children (GtkTreeModel *model,
                                 GtkTreeIter  *iter,
                                 GtkTreeIter  *parent);
static gboolean
thunar_list_model_iter_has_child (GtkTreeModel *model,
                                  GtkTreeIter  *iter);
static gint
thunar_list_model_iter_n_children (GtkTreeModel *model,
                                   GtkTreeIter  *iter);
static gboolean
thunar_list_model_iter_nth_child (GtkTreeModel *model,
                                  GtkTreeIter  *iter,
                                  GtkTreeIter  *parent,
                                  gint          n);
static gboolean
thunar_list_model_iter_parent (GtkTreeModel *model,
                               GtkTreeIter  *iter,
                               GtkTreeIter  *child);
static gboolean
thunar_list_model_drag_data_received (GtkTreeDragDest  *dest,
                                      GtkTreePath      *path,
                                      GtkSelectionData *data);
static gboolean
thunar_list_model_row_drop_possible (GtkTreeDragDest  *dest,
                                     GtkTreePath      *path,
                                     GtkSelectionData *data);
static gboolean
thunar_list_model_get_sort_column_id (GtkTreeSortable *sortable,
                                      gint            *sort_column_id,
                                      GtkSortType     *order);
static void
thunar_list_model_set_sort_column_id (GtkTreeSortable *sortable,
                                      gint             sort_column_id,
                                      GtkSortType      order);
static void
thunar_list_model_set_default_sort_func (GtkTreeSortable       *sortable,
                                         GtkTreeIterCompareFunc func,
                                         gpointer               data,
                                         GDestroyNotify         destroy);
static void
thunar_list_model_set_sort_func (GtkTreeSortable       *sortable,
                                 gint                   sort_column_id,
                                 GtkTreeIterCompareFunc func,
                                 gpointer               data,
                                 GDestroyNotify         destroy);
static gboolean
thunar_list_model_has_default_sort_func (GtkTreeSortable *sortable);
static gint
thunar_list_model_cmp_func (gconstpointer a,
                            gconstpointer b,
                            gpointer      user_data);
static void
thunar_list_model_sort (ThunarListModel *store);
static void
thunar_list_model_files_changed (ThunarFolder    *folder,
                                 GHashTable      *files,
                                 ThunarListModel *store);
static void
thunar_list_model_folder_destroy (ThunarFolder    *folder,
                                  ThunarListModel *store);
static void
thunar_list_model_folder_error (ThunarFolder    *folder,
                                const GError    *error,
                                ThunarListModel *store);
static void
thunar_list_model_notify_loading (ThunarFolder    *folder,
                                  GParamSpec      *pspec,
                                  ThunarListModel *model);
static void
thunar_list_model_files_added (ThunarFolder    *folder,
                               GHashTable      *files,
                               ThunarListModel *store);
static void
thunar_list_model_files_removed (ThunarFolder    *folder,
                                 GHashTable      *files,
                                 ThunarListModel *store);
static void
thunar_list_model_insert_files (ThunarListModel *store,
                                GHashTable      *files);

static gboolean
thunar_list_model_get_case_sensitive (ThunarListModel *store);
static void
thunar_list_model_set_case_sensitive (ThunarListModel *store,
                                      gboolean         case_sensitive);
static ThunarDateStyle
thunar_list_model_get_date_style (ThunarListModel *store);
static void
thunar_list_model_set_date_style (ThunarListModel *store,
                                  ThunarDateStyle  date_style);
static const char *
thunar_list_model_get_date_custom_style (ThunarListModel *store);
static void
thunar_list_model_set_date_custom_style (ThunarListModel *store,
                                         const char      *date_custom_style);
static gint
thunar_list_model_get_num_files (ThunarListModel *store);
static gboolean
thunar_list_model_get_folders_first (ThunarListModel *store);
static gboolean
thunar_list_model_get_hidden_last (ThunarListModel *store);
static void
thunar_list_model_cancel_search_job (ThunarListModel *model);

static void
thunar_list_model_search_error (ThunarJob *job);
static void
thunar_list_model_search_finished (ThunarJob       *job,
                                   ThunarListModel *store);
static void
thunar_list_model_add_search_files (ThunarStandardViewModel *model,
                                    GList                   *files);
static gboolean
thunar_list_model_update_search_files (ThunarListModel *model);

static gint
thunar_list_model_get_folder_item_count (ThunarListModel *store);
static void
thunar_list_model_set_folder_item_count (ThunarListModel      *store,
                                         ThunarFolderItemCount count_as_dir_size);
static gboolean
thunar_list_model_get_loading (ThunarListModel *store);
static void
thunar_list_model_set_loading (ThunarListModel *store,
                               gboolean         loading);

static void
thunar_list_model_file_count_callback (ThunarJob *job,
                                       gpointer   model);
static ThunarFolder *
thunar_list_model_get_folder (ThunarStandardViewModel *store);
static void
thunar_list_model_set_folder (ThunarStandardViewModel *store,
                              ThunarFolder            *folder,
                              gchar                   *search_query);
static void
thunar_list_model_set_folders_first (ThunarStandardViewModel *store,
                                     gboolean                 folders_first);
static void
thunar_list_model_set_hidden_last (ThunarStandardViewModel *store,
                                   gboolean                 hidden_last);
static gboolean
thunar_list_model_get_show_hidden (ThunarStandardViewModel *store);
static void
thunar_list_model_set_show_hidden (ThunarStandardViewModel *store,
                                   gboolean                 show_hidden);
static gboolean
thunar_list_model_get_file_size_binary (ThunarStandardViewModel *store);
static void
thunar_list_model_set_file_size_binary (ThunarStandardViewModel *store,
                                        gboolean                 file_size_binary);
static ThunarFile *
thunar_list_model_get_file (ThunarStandardViewModel *store,
                            GtkTreeIter             *iter);
static GList *
thunar_list_model_get_paths_for_files (ThunarStandardViewModel *store,
                                       GList                   *files);
static ThunarJob *
thunar_list_model_get_job (ThunarStandardViewModel *store);
static void
thunar_list_model_set_job (ThunarStandardViewModel *store,
                           ThunarJob               *job);

struct _ThunarListModelClass
{
  GObjectClass __parent__;

  /* signals */
  void (*error) (ThunarListModel *store,
                 const GError    *error);
  void (*search_done) (void);
};

struct _ThunarListModel
{
  GObject __parent__;

  /* the model stamp is only used when debugging is
   * enabled, to make sure we don't accept iterators
   * generated by another model.
   */
#ifndef NDEBUG
  gint stamp;
#endif

  GSequence            *rows;
  GSList               *hidden;
  ThunarFolder         *folder;
  gboolean              show_hidden : 1;
  ThunarFolderItemCount folder_item_count;
  gboolean              file_size_binary : 1;
  ThunarDateStyle       date_style;
  char                 *date_custom_style;

  /* Normalized current search terms.
   * NULL if not presenting a search's results.
   * Search job may have finished even if this is non-NULL.
   */
  gchar **search_terms;

  /* ids for the "row-inserted" and "row-deleted" signals
   * of GtkTreeModel to speed up folder changing.
   */
  guint row_inserted_id;
  guint row_deleted_id;

  guint          sort_case_sensitive : 1;
  guint          sort_folders_first : 1;
  guint          sort_hidden_last : 1;
  gint           sort_sign; /* 1 = ascending, -1 descending */
  ThunarSortFunc sort_func;

  /* searching runs in a separate thread which incrementally inserts results (files)
   * in the files_to_add list.
   * Periodically the main thread takes all the files in the files_to_add list
   * and adds them in the model. The list is then emptied.
   */
  ThunarJob  *recursive_search_job;
  GHashTable *files_to_add;
  GMutex      mutex_files_to_add;

  /* used to stop the periodic call to thunar_list_model_add_search_files when the search is finished/canceled */
  guint update_search_results_timeout_id;

  /* Tells if the model is yet loading the set folder */
  gboolean loading;

  /* indicates that file was removed or sorted */
  gboolean file_was_removed;
  gboolean file_was_sorted;

  /* tells is workaround for removed file should be used */
  gboolean check_file_in_model_before_use;
};



static GParamSpec *list_model_props[N_PROPERTIES] = {
  NULL,
};



G_DEFINE_TYPE_WITH_CODE (ThunarListModel, thunar_list_model, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GTK_TYPE_TREE_MODEL, thunar_list_model_tree_model_init)
    G_IMPLEMENT_INTERFACE (GTK_TYPE_TREE_DRAG_DEST, thunar_list_model_drag_dest_init)
    G_IMPLEMENT_INTERFACE (GTK_TYPE_TREE_SORTABLE, thunar_list_model_sortable_init)
    G_IMPLEMENT_INTERFACE (THUNAR_TYPE_STANDARD_VIEW_MODEL, thunar_list_model_standard_view_model_init))



static void
thunar_list_model_class_init (ThunarListModelClass *klass)
{
  GObjectClass *gobject_class;
  gpointer      g_iface;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = thunar_list_model_dispose;
  gobject_class->finalize = thunar_list_model_finalize;
  gobject_class->get_property = thunar_list_model_get_property;
  gobject_class->set_property = thunar_list_model_set_property;

  g_iface = g_type_default_interface_peek (THUNAR_TYPE_STANDARD_VIEW_MODEL);
  /**
   * ThunarListModel:case-sensitive:
   *
   * Tells whether the sorting should be case sensitive.
   **/
  list_model_props[PROP_CASE_SENSITIVE] =
  g_param_spec_override ("case-sensitive",
                         g_object_interface_find_property (g_iface, "case-sensitive"));

  /**
   * ThunarListModel:date-style:
   *
   * The style used to format dates.
   **/
  list_model_props[PROP_DATE_STYLE] =
  g_param_spec_override ("date-style",
                         g_object_interface_find_property (g_iface, "date-style"));

  /**
   * ThunarListModel:date-custom-style:
   *
   * The style used for custom format of dates.
   **/
  list_model_props[PROP_DATE_CUSTOM_STYLE] =
  g_param_spec_override ("date-custom-style",
                         g_object_interface_find_property (g_iface, "date-custom-style"));

  /**
   * ThunarListModel:folder:
   *
   * The folder presented by this #ThunarListModel.
   **/
  list_model_props[PROP_FOLDER] =
  g_param_spec_override ("folder",
                         g_object_interface_find_property (g_iface, "folder"));

  /**
   * ThunarListModel::folders-first:
   *
   * Tells whether to always sort folders before other files.
   **/
  list_model_props[PROP_FOLDERS_FIRST] =
  g_param_spec_override ("folders-first",
                         g_object_interface_find_property (g_iface, "folders-first"));

  /**
   * ThunarListModel::hidden-last:
   *
   * Tells whether to always sort hidden files after other files.
   **/

  list_model_props[PROP_HIDDEN_LAST] =
  g_param_spec_override ("hidden-last",
                         g_object_interface_find_property (g_iface, "hidden-last"));

  /**
   * ThunarListModel::num-files:
   *
   * The number of files in the folder presented by this #ThunarListModel.
   **/
  list_model_props[PROP_NUM_FILES] =
  g_param_spec_override ("num-files",
                         g_object_interface_find_property (g_iface, "num-files"));

  /**
   * ThunarListModel::show-hidden:
   *
   * Tells whether to include hidden (and backup) files.
   **/
  list_model_props[PROP_SHOW_HIDDEN] =
  g_param_spec_override ("show-hidden",
                         g_object_interface_find_property (g_iface, "show-hidden"));

  /**
   * ThunarListModel::misc-file-size-binary:
   *
   * Tells whether to format file size in binary.
   **/
  list_model_props[PROP_FILE_SIZE_BINARY] =
  g_param_spec_override ("file-size-binary",
                         g_object_interface_find_property (g_iface, "file-size-binary"));

  /**
   * ThunarListModel:folder-item-count:
   *
   * Tells when the size column of folders should show the number of containing files
   **/
  list_model_props[PROP_FOLDER_ITEM_COUNT] =
  g_param_spec_override ("folder-item-count",
                         g_object_interface_find_property (g_iface, "folder-item-count"));

  /**
   * ThunarListModel:loading:
   *
   * Tells if the model is yet loading a folder
   **/
  list_model_props[PROP_LOADING] =
  g_param_spec_override ("loading",
                         g_object_interface_find_property (g_iface, "loading"));

  /* install properties */
  g_object_class_install_properties (gobject_class, N_PROPERTIES, list_model_props);

  /* No need to install signals. Already done by the interface */
}



static void
thunar_list_model_standard_view_model_init (ThunarStandardViewModelIface *iface)
{
  iface->get_job = thunar_list_model_get_job;
  iface->set_job = thunar_list_model_set_job;
  iface->get_file = thunar_list_model_get_file;
  iface->get_folder = thunar_list_model_get_folder;
  iface->set_folder = thunar_list_model_set_folder;
  iface->get_show_hidden = thunar_list_model_get_show_hidden;
  iface->set_show_hidden = thunar_list_model_set_show_hidden;
  iface->get_paths_for_files = thunar_list_model_get_paths_for_files;
  iface->get_file_size_binary = thunar_list_model_get_file_size_binary;
  iface->set_file_size_binary = thunar_list_model_set_file_size_binary;
  iface->set_folders_first = thunar_list_model_set_folders_first;
  iface->set_hidden_last = thunar_list_model_set_hidden_last;
  iface->add_search_files = thunar_list_model_add_search_files;
}



static void
thunar_list_model_tree_model_init (GtkTreeModelIface *iface)
{
  iface->get_flags = thunar_list_model_get_flags;
  iface->get_n_columns = thunar_list_model_get_n_columns;
  iface->get_column_type = thunar_list_model_get_column_type;
  iface->get_iter = thunar_list_model_get_iter;
  iface->get_path = thunar_list_model_get_path;
  iface->get_value = thunar_list_model_get_value;
  iface->iter_next = thunar_list_model_iter_next;
  iface->iter_children = thunar_list_model_iter_children;
  iface->iter_has_child = thunar_list_model_iter_has_child;
  iface->iter_n_children = thunar_list_model_iter_n_children;
  iface->iter_nth_child = thunar_list_model_iter_nth_child;
  iface->iter_parent = thunar_list_model_iter_parent;
}



static void
thunar_list_model_drag_dest_init (GtkTreeDragDestIface *iface)
{
  iface->drag_data_received = thunar_list_model_drag_data_received;
  iface->row_drop_possible = thunar_list_model_row_drop_possible;
}



static void
thunar_list_model_sortable_init (GtkTreeSortableIface *iface)
{
  iface->get_sort_column_id = thunar_list_model_get_sort_column_id;
  iface->set_sort_column_id = thunar_list_model_set_sort_column_id;
  iface->set_sort_func = thunar_list_model_set_sort_func;
  iface->set_default_sort_func = thunar_list_model_set_default_sort_func;
  iface->has_default_sort_func = thunar_list_model_has_default_sort_func;
}



static void
thunar_list_model_init (ThunarListModel *store)
{
  /* generate a unique stamp if we're in debug mode */
#ifndef NDEBUG
  store->stamp = g_random_int ();
#endif

  store->row_inserted_id = g_signal_lookup ("row-inserted", GTK_TYPE_TREE_MODEL);
  store->row_deleted_id = g_signal_lookup ("row-deleted", GTK_TYPE_TREE_MODEL);

  store->search_terms = NULL;

  store->sort_case_sensitive = TRUE;
  store->sort_folders_first = TRUE;
  store->sort_hidden_last = FALSE;
  store->sort_sign = 1;
  store->sort_func = thunar_file_compare_by_name;
  store->rows = g_sequence_new (g_object_unref);
  store->files_to_add = g_hash_table_new (g_direct_hash, NULL);
  g_mutex_init (&store->mutex_files_to_add);

  store->loading = FALSE;
}



static void
thunar_list_model_dispose (GObject *object)
{
  /* unlink from the folder (if any) */
  thunar_list_model_set_folder (THUNAR_STANDARD_VIEW_MODEL (object), NULL, NULL);

  (*G_OBJECT_CLASS (thunar_list_model_parent_class)->dispose) (object);
}



static void
thunar_list_model_finalize (GObject *object)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (object);

  thunar_list_model_cancel_search_job (store);

  if (store->update_search_results_timeout_id > 0)
    {
      g_source_remove (store->update_search_results_timeout_id);
      store->update_search_results_timeout_id = 0;
    }
  g_hash_table_destroy (store->files_to_add);
  store->files_to_add = NULL;

  g_sequence_free (store->rows);
  g_mutex_clear (&store->mutex_files_to_add);

  g_free (store->date_custom_style);

  g_strfreev (store->search_terms);

  (*G_OBJECT_CLASS (thunar_list_model_parent_class)->finalize) (object);
}



static void
thunar_list_model_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (object);

  switch (prop_id)
    {
    case PROP_CASE_SENSITIVE:
      g_value_set_boolean (value, thunar_list_model_get_case_sensitive (store));
      break;

    case PROP_DATE_STYLE:
      g_value_set_enum (value, thunar_list_model_get_date_style (store));
      break;

    case PROP_DATE_CUSTOM_STYLE:
      g_value_set_string (value, thunar_list_model_get_date_custom_style (store));
      break;

    case PROP_FOLDER:
      g_value_set_object (value, thunar_list_model_get_folder (THUNAR_STANDARD_VIEW_MODEL (store)));
      break;

    case PROP_FOLDERS_FIRST:
      g_value_set_boolean (value, thunar_list_model_get_folders_first (store));
      break;

    case PROP_HIDDEN_LAST:
      g_value_set_boolean (value, thunar_list_model_get_hidden_last (store));
      break;

    case PROP_NUM_FILES:
      g_value_set_uint (value, thunar_list_model_get_num_files (store));
      break;

    case PROP_SHOW_HIDDEN:
      g_value_set_boolean (value, thunar_list_model_get_show_hidden (THUNAR_STANDARD_VIEW_MODEL (store)));
      break;

    case PROP_FILE_SIZE_BINARY:
      g_value_set_boolean (value, thunar_list_model_get_file_size_binary THUNAR_STANDARD_VIEW_MODEL (store));
      break;

    case PROP_FOLDER_ITEM_COUNT:
      g_value_set_enum (value, thunar_list_model_get_folder_item_count (store));
      break;

    case PROP_LOADING:
      g_value_set_boolean (value, thunar_list_model_get_loading (store));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
thunar_list_model_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (object);

  switch (prop_id)
    {
    case PROP_CASE_SENSITIVE:
      thunar_list_model_set_case_sensitive (store, g_value_get_boolean (value));
      break;

    case PROP_DATE_STYLE:
      thunar_list_model_set_date_style (store, g_value_get_enum (value));
      break;

    case PROP_DATE_CUSTOM_STYLE:
      thunar_list_model_set_date_custom_style (store, g_value_get_string (value));
      break;

    case PROP_FOLDER:
      thunar_list_model_set_folder (THUNAR_STANDARD_VIEW_MODEL (store), g_value_get_object (value), NULL);
      break;

    case PROP_FOLDERS_FIRST:
      thunar_list_model_set_folders_first (THUNAR_STANDARD_VIEW_MODEL (store), g_value_get_boolean (value));
      break;

    case PROP_HIDDEN_LAST:
      thunar_list_model_set_hidden_last (THUNAR_STANDARD_VIEW_MODEL (store), g_value_get_boolean (value));
      break;


    case PROP_SHOW_HIDDEN:
      thunar_list_model_set_show_hidden (THUNAR_STANDARD_VIEW_MODEL (store), g_value_get_boolean (value));
      break;

    case PROP_FILE_SIZE_BINARY:
      thunar_list_model_set_file_size_binary (THUNAR_STANDARD_VIEW_MODEL (store), g_value_get_boolean (value));
      break;

    case PROP_FOLDER_ITEM_COUNT:
      thunar_list_model_set_folder_item_count (store, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static GtkTreeModelFlags
thunar_list_model_get_flags (GtkTreeModel *model)
{
  return GTK_TREE_MODEL_ITERS_PERSIST | GTK_TREE_MODEL_LIST_ONLY;
}



static gint
thunar_list_model_get_n_columns (GtkTreeModel *model)
{
  return THUNAR_N_COLUMNS;
}



static GType
thunar_list_model_get_column_type (GtkTreeModel *model,
                                   gint          idx)
{
  switch (idx)
    {
    case THUNAR_COLUMN_DATE_CREATED:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_DATE_ACCESSED:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_DATE_MODIFIED:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_DATE_DELETED:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_RECENCY:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_LOCATION:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_GROUP:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_MIME_TYPE:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_NAME:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_OWNER:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_PERMISSIONS:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_SIZE:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_SIZE_IN_BYTES:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_TYPE:
      return G_TYPE_STRING;

    case THUNAR_COLUMN_FILE:
      return THUNAR_TYPE_FILE;

    case THUNAR_COLUMN_FILE_NAME:
      return G_TYPE_STRING;
    }

  _thunar_assert_not_reached ();
  return G_TYPE_INVALID;
}



static gboolean
thunar_list_model_get_iter (GtkTreeModel *model,
                            GtkTreeIter  *iter,
                            GtkTreePath  *path)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  GSequenceIter   *row;
  gint             offset;

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  _thunar_return_val_if_fail (gtk_tree_path_get_depth (path) > 0, FALSE);

  /* determine the row for the path */
  offset = gtk_tree_path_get_indices (path)[0];
  row = g_sequence_get_iter_at_pos (store->rows, offset);

  if (!g_sequence_iter_is_end (row))
    {
      GTK_TREE_ITER_INIT (*iter, store->stamp, row);
      return TRUE;
    }

  return FALSE;
}



static GtkTreePath *
thunar_list_model_get_path (GtkTreeModel *model,
                            GtkTreeIter  *iter)
{
  gint idx;

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (model), NULL);
  _thunar_return_val_if_fail (iter->stamp == THUNAR_LIST_MODEL (model)->stamp, NULL);

  idx = g_sequence_iter_get_position (iter->user_data);
  if (G_LIKELY (idx >= 0))
    return gtk_tree_path_new_from_indices (idx, -1);

  return NULL;
}



static void
thunar_list_model_get_value (GtkTreeModel *model,
                             GtkTreeIter  *iter,
                             gint          column,
                             GValue       *value)
{
  ThunarGroup  *group;
  const gchar  *device_type;
  const gchar  *name;
  const gchar  *real_name;
  ThunarUser   *user;
  ThunarFile   *file;
  ThunarFolder *folder;
  gchar        *str;
  gint32        item_count;
  GFile        *g_file;
  GFile        *g_file_parent;

  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (model));
  _thunar_return_if_fail (iter->stamp == (THUNAR_LIST_MODEL (model))->stamp);

  /* WORKAROUND: if file was removed from the completion model and files was sorted in
   * "thunar_list_model_file_changed", sometimes the view is trying to access the removed
   * data, so check if the requested item is in the model. */
  if (THUNAR_LIST_MODEL (model)->check_file_in_model_before_use && THUNAR_LIST_MODEL (model)->file_was_removed && THUNAR_LIST_MODEL (model)->file_was_sorted)
    {
      GSequenceIter *row = g_sequence_get_begin_iter (THUNAR_LIST_MODEL (model)->rows);
      GSequenceIter *end = g_sequence_get_end_iter (THUNAR_LIST_MODEL (model)->rows);
      GSequenceIter *next;
      gboolean       found = FALSE;

      while (row != end)
        {
          next = g_sequence_iter_next (row);
          if (row == iter->user_data)
            {
              found = TRUE;
              break;
            }
          row = next;
        }
      if (!found)
        {
          g_warning ("Requested file doesn't exist in the list model!");
          return;
        }
    }

  file = g_sequence_get (iter->user_data);
  _thunar_return_if_fail (THUNAR_IS_FILE (file));

  switch (column)
    {
    case THUNAR_COLUMN_DATE_CREATED:
      g_value_init (value, G_TYPE_STRING);
      str = thunar_file_get_date_string (file, THUNAR_FILE_DATE_CREATED, THUNAR_LIST_MODEL (model)->date_style, THUNAR_LIST_MODEL (model)->date_custom_style);
      g_value_take_string (value, str);
      break;

    case THUNAR_COLUMN_DATE_ACCESSED:
      g_value_init (value, G_TYPE_STRING);
      str = thunar_file_get_date_string (file, THUNAR_FILE_DATE_ACCESSED, THUNAR_LIST_MODEL (model)->date_style, THUNAR_LIST_MODEL (model)->date_custom_style);
      g_value_take_string (value, str);
      break;

    case THUNAR_COLUMN_DATE_MODIFIED:
      g_value_init (value, G_TYPE_STRING);
      str = thunar_file_get_date_string (file, THUNAR_FILE_DATE_MODIFIED, THUNAR_LIST_MODEL (model)->date_style, THUNAR_LIST_MODEL (model)->date_custom_style);
      g_value_take_string (value, str);
      break;

    case THUNAR_COLUMN_DATE_DELETED:
      g_value_init (value, G_TYPE_STRING);
      str = thunar_file_get_date_string (file, THUNAR_FILE_DATE_DELETED, THUNAR_LIST_MODEL (model)->date_style, THUNAR_LIST_MODEL (model)->date_custom_style);
      g_value_take_string (value, str);
      break;

    case THUNAR_COLUMN_RECENCY:
      g_value_init (value, G_TYPE_STRING);
      str = thunar_file_get_date_string (file, THUNAR_FILE_RECENCY, THUNAR_LIST_MODEL (model)->date_style, THUNAR_LIST_MODEL (model)->date_custom_style);
      g_value_take_string (value, str);
      break;

    case THUNAR_COLUMN_LOCATION:
      g_value_init (value, G_TYPE_STRING);
      g_file_parent = g_file_get_parent (thunar_file_get_file (file));
      str = NULL;

      /* g_file_parent will be NULL only if a search returned the root
       * directory somehow, or "file:///" is in recent:/// somehow.
       * These should be quite rare circumstances. */
      if (G_UNLIKELY (g_file_parent == NULL))
        {
          g_value_take_string (value, NULL);
          break;
        }

      /* Try and show a relative path beginning with the current folder's name to the parent folder.
       * Fall thru with str==NULL if that is not possible. */
      folder = THUNAR_LIST_MODEL (model)->folder;
      if (G_LIKELY (folder != NULL))
        {
          const gchar *folder_basename = thunar_file_get_basename (thunar_folder_get_corresponding_file (folder));
          GFile       *g_folder = thunar_file_get_file (thunar_folder_get_corresponding_file (folder));
          if (g_file_equal (g_folder, g_file_parent))
            {
              /* commonest non-prefix case: item location is directly inside the search folder */
              str = g_strdup (folder_basename);
            }
          else
            {
              str = g_file_get_relative_path (g_folder, g_file_parent);
              /* str can still be NULL if g_folder is not a prefix of g_file_parent */
              if (str != NULL)
                {
                  gchar *tmp = g_build_path (G_DIR_SEPARATOR_S, folder_basename, str, NULL);
                  g_free (str);
                  str = tmp;
                }
            }
        }

      /* catchall for when model->folder is not an ancestor of the parent (e.g. when searching recent:///).
       * In this case, show a prettified absolute URI or local path. */
      if (str == NULL)
        str = g_file_get_parse_name (g_file_parent);

      g_object_unref (g_file_parent);
      g_value_take_string (value, str);
      break;

    case THUNAR_COLUMN_GROUP:
      g_value_init (value, G_TYPE_STRING);
      group = thunar_file_get_group (file);
      if (G_LIKELY (group != NULL))
        {
          g_value_set_string (value, thunar_group_get_name (group));
          g_object_unref (G_OBJECT (group));
        }
      else
        {
          g_value_set_static_string (value, _("Unknown"));
        }
      break;

    case THUNAR_COLUMN_MIME_TYPE:
      g_value_init (value, G_TYPE_STRING);
      g_value_set_static_string (value, thunar_file_get_content_type (file));
      break;

    case THUNAR_COLUMN_NAME:
      g_value_init (value, G_TYPE_STRING);
      g_value_set_static_string (value, thunar_file_get_display_name (file));
      break;

    case THUNAR_COLUMN_OWNER:
      g_value_init (value, G_TYPE_STRING);
      user = thunar_file_get_user (file);
      if (G_LIKELY (user != NULL))
        {
          /* determine sane display name for the owner */
          name = thunar_user_get_name (user);
          real_name = thunar_user_get_real_name (user);
          if (G_LIKELY (real_name != NULL))
            {
              if (strcmp (name, real_name) == 0)
                str = g_strdup (name);
              else
                str = g_strdup_printf ("%s (%s)", real_name, name);
            }
          else
            str = g_strdup (name);
          g_value_take_string (value, str);
          g_object_unref (G_OBJECT (user));
        }
      else
        {
          g_value_set_static_string (value, _("Unknown"));
        }
      break;

    case THUNAR_COLUMN_PERMISSIONS:
      g_value_init (value, G_TYPE_STRING);
      g_value_take_string (value, thunar_file_get_mode_string (file));
      break;

    case THUNAR_COLUMN_SIZE:
      g_value_init (value, G_TYPE_STRING);

      if (thunar_file_is_mountable (file))
        {
          g_file = thunar_file_get_target_location (file);
          if (g_file == NULL)
            break;
          g_value_take_string (value, thunar_g_file_get_free_space_string (g_file, THUNAR_LIST_MODEL (model)->file_size_binary));
          g_object_unref (g_file);
          break;
        }
      else if (thunar_file_is_directory (file))
        {
          /* If the option is set to always show folder sizes as item counts, then give the folder's item count */
          if (THUNAR_LIST_MODEL (model)->folder_item_count == THUNAR_FOLDER_ITEM_COUNT_ALWAYS)
            {
              item_count = thunar_file_get_file_count (file, G_CALLBACK (thunar_list_model_file_count_callback), model);
              if (item_count < 0)
                g_value_take_string (value, g_strdup (_("unknown")));
              else
                g_value_take_string (value, g_strdup_printf (ngettext ("%u item", "%u items", item_count), item_count));
            }

          /* If the option is set to always show folder sizes as item counts only for local files,
           * check if the files is local or not, and act accordingly */
          else if (THUNAR_LIST_MODEL (model)->folder_item_count == THUNAR_FOLDER_ITEM_COUNT_ONLY_LOCAL)
            {
              if (thunar_file_is_local (file))
                {
                  item_count = thunar_file_get_file_count (file, G_CALLBACK (thunar_list_model_file_count_callback), model);
                  if (item_count < 0)
                    g_value_take_string (value, g_strdup (_("unknown")));
                  else
                    g_value_take_string (value, g_strdup_printf (ngettext ("%u item", "%u items", item_count), item_count));
                }
              else
                g_value_take_string (value, thunar_file_get_size_string_formatted (file, THUNAR_LIST_MODEL (model)->file_size_binary));
            }
          else if (THUNAR_LIST_MODEL (model)->folder_item_count != THUNAR_FOLDER_ITEM_COUNT_NEVER)
            g_warning ("Error, unknown enum value for folder_item_count in the list model");
        }
      else
        {
          g_value_take_string (value, thunar_file_get_size_string_formatted (file, THUNAR_LIST_MODEL (model)->file_size_binary));
        }
      break;

    case THUNAR_COLUMN_SIZE_IN_BYTES:
      g_value_init (value, G_TYPE_STRING);
      g_value_take_string (value, thunar_file_get_size_in_bytes_string (file));
      break;

    case THUNAR_COLUMN_TYPE:
      g_value_init (value, G_TYPE_STRING);
      device_type = thunar_file_get_device_type (file);
      if (device_type != NULL)
        {
          g_value_take_string (value, g_strdup (device_type));
          break;
        }
      g_value_take_string (value, thunar_file_get_content_type_desc (file, TRUE));
      break;

    case THUNAR_COLUMN_FILE:
      g_value_init (value, THUNAR_TYPE_FILE);
      g_value_set_object (value, file);
      break;

    case THUNAR_COLUMN_FILE_NAME:
      g_value_init (value, G_TYPE_STRING);
      g_value_set_static_string (value, thunar_file_get_display_name (file));
      break;

    default:
      _thunar_assert_not_reached ();
      break;
    }
}



static gboolean
thunar_list_model_iter_next (GtkTreeModel *model,
                             GtkTreeIter  *iter)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (model), FALSE);
  _thunar_return_val_if_fail (iter->stamp == (THUNAR_LIST_MODEL (model))->stamp, FALSE);

  iter->user_data = g_sequence_iter_next (iter->user_data);
  return !g_sequence_iter_is_end (iter->user_data);
}



static gboolean
thunar_list_model_iter_children (GtkTreeModel *model,
                                 GtkTreeIter  *iter,
                                 GtkTreeIter  *parent)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);

  if (G_LIKELY (parent == NULL
                && g_sequence_get_length (store->rows) > 0))
    {
      GTK_TREE_ITER_INIT (*iter, store->stamp, g_sequence_get_begin_iter (store->rows));
      return TRUE;
    }

  return FALSE;
}



static gboolean
thunar_list_model_iter_has_child (GtkTreeModel *model,
                                  GtkTreeIter  *iter)
{
  return FALSE;
}



static gint
thunar_list_model_iter_n_children (GtkTreeModel *model,
                                   GtkTreeIter  *iter)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), 0);

  return (iter == NULL) ? g_sequence_get_length (store->rows) : 0;
}



static gboolean
thunar_list_model_iter_nth_child (GtkTreeModel *model,
                                  GtkTreeIter  *iter,
                                  GtkTreeIter  *parent,
                                  gint          n)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  GSequenceIter   *row;

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);

  if (G_LIKELY (parent == NULL))
    {
      row = g_sequence_get_iter_at_pos (store->rows, n);
      if (g_sequence_iter_is_end (row))
        return FALSE;

      GTK_TREE_ITER_INIT (*iter, store->stamp, row);
      return TRUE;
    }

  return FALSE;
}



static gboolean
thunar_list_model_iter_parent (GtkTreeModel *model,
                               GtkTreeIter  *iter,
                               GtkTreeIter  *child)
{
  return FALSE;
}



static gboolean
thunar_list_model_drag_data_received (GtkTreeDragDest  *dest,
                                      GtkTreePath      *path,
                                      GtkSelectionData *data)
{
  return FALSE;
}



static gboolean
thunar_list_model_row_drop_possible (GtkTreeDragDest  *dest,
                                     GtkTreePath      *path,
                                     GtkSelectionData *data)
{
  return FALSE;
}



static gboolean
thunar_list_model_get_sort_column_id (GtkTreeSortable *sortable,
                                      gint            *sort_column_id,
                                      GtkSortType     *order)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (sortable);

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);

  if (store->sort_func == thunar_cmp_files_by_mime_type)
    *sort_column_id = THUNAR_COLUMN_MIME_TYPE;
  else if (store->sort_func == thunar_file_compare_by_name)
    *sort_column_id = THUNAR_COLUMN_NAME;
  else if (store->sort_func == thunar_cmp_files_by_permissions)
    *sort_column_id = THUNAR_COLUMN_PERMISSIONS;
  else if (store->sort_func == thunar_cmp_files_by_size || store->sort_func == (ThunarSortFunc) thunar_cmp_files_by_size_and_items_count)
    *sort_column_id = THUNAR_COLUMN_SIZE;
  else if (store->sort_func == thunar_cmp_files_by_size_in_bytes)
    *sort_column_id = THUNAR_COLUMN_SIZE_IN_BYTES;
  else if (store->sort_func == thunar_cmp_files_by_date_created)
    *sort_column_id = THUNAR_COLUMN_DATE_CREATED;
  else if (store->sort_func == thunar_cmp_files_by_date_accessed)
    *sort_column_id = THUNAR_COLUMN_DATE_ACCESSED;
  else if (store->sort_func == thunar_cmp_files_by_date_modified)
    *sort_column_id = THUNAR_COLUMN_DATE_MODIFIED;
  else if (store->sort_func == thunar_cmp_files_by_date_deleted)
    *sort_column_id = THUNAR_COLUMN_DATE_DELETED;
  else if (store->sort_func == thunar_cmp_files_by_recency)
    *sort_column_id = THUNAR_COLUMN_RECENCY;
  else if (store->sort_func == thunar_cmp_files_by_location)
    *sort_column_id = THUNAR_COLUMN_LOCATION;
  else if (store->sort_func == thunar_cmp_files_by_type)
    *sort_column_id = THUNAR_COLUMN_TYPE;
  else if (store->sort_func == thunar_cmp_files_by_owner)
    *sort_column_id = THUNAR_COLUMN_OWNER;
  else if (store->sort_func == thunar_cmp_files_by_group)
    *sort_column_id = THUNAR_COLUMN_GROUP;
  else
    _thunar_assert_not_reached ();

  if (order != NULL)
    {
      if (store->sort_sign > 0)
        *order = GTK_SORT_ASCENDING;
      else
        *order = GTK_SORT_DESCENDING;
    }

  return TRUE;
}



static void
thunar_list_model_set_sort_column_id (GtkTreeSortable *sortable,
                                      gint             sort_column_id,
                                      GtkSortType      order)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (sortable);

  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  switch (sort_column_id)
    {
    case THUNAR_COLUMN_DATE_CREATED:
      store->sort_func = thunar_cmp_files_by_date_created;
      break;

    case THUNAR_COLUMN_DATE_ACCESSED:
      store->sort_func = thunar_cmp_files_by_date_accessed;
      break;

    case THUNAR_COLUMN_DATE_MODIFIED:
      store->sort_func = thunar_cmp_files_by_date_modified;
      break;

    case THUNAR_COLUMN_DATE_DELETED:
      store->sort_func = thunar_cmp_files_by_date_deleted;
      break;

    case THUNAR_COLUMN_RECENCY:
      store->sort_func = thunar_cmp_files_by_recency;
      break;

    case THUNAR_COLUMN_LOCATION:
      store->sort_func = thunar_cmp_files_by_location;
      break;

    case THUNAR_COLUMN_GROUP:
      store->sort_func = thunar_cmp_files_by_group;
      break;

    case THUNAR_COLUMN_MIME_TYPE:
      store->sort_func = thunar_cmp_files_by_mime_type;
      break;

    case THUNAR_COLUMN_FILE_NAME:
    case THUNAR_COLUMN_NAME:
      store->sort_func = thunar_file_compare_by_name;
      break;

    case THUNAR_COLUMN_OWNER:
      store->sort_func = thunar_cmp_files_by_owner;
      break;

    case THUNAR_COLUMN_PERMISSIONS:
      store->sort_func = thunar_cmp_files_by_permissions;
      break;

    case THUNAR_COLUMN_SIZE:
      store->sort_func = (store->folder_item_count != THUNAR_FOLDER_ITEM_COUNT_NEVER) ? (ThunarSortFunc) thunar_cmp_files_by_size_and_items_count : thunar_cmp_files_by_size;
      break;

    case THUNAR_COLUMN_SIZE_IN_BYTES:
      store->sort_func = thunar_cmp_files_by_size_in_bytes;
      break;

    case THUNAR_COLUMN_TYPE:
      store->sort_func = thunar_cmp_files_by_type;
      break;

    default:
      _thunar_assert_not_reached ();
    }

  /* new sort sign */
  store->sort_sign = (order == GTK_SORT_ASCENDING) ? 1 : -1;

  /* re-sort the store */
  thunar_list_model_sort (store);

  /* notify listining parties */
  gtk_tree_sortable_sort_column_changed (sortable);
}



static void
thunar_list_model_set_default_sort_func (GtkTreeSortable       *sortable,
                                         GtkTreeIterCompareFunc func,
                                         gpointer               data,
                                         GDestroyNotify         destroy)
{
  g_critical ("ThunarListModel has sorting facilities built-in!");
}



static void
thunar_list_model_set_sort_func (GtkTreeSortable       *sortable,
                                 gint                   sort_column_id,
                                 GtkTreeIterCompareFunc func,
                                 gpointer               data,
                                 GDestroyNotify         destroy)
{
  g_critical ("ThunarListModel has sorting facilities built-in!");
}



static gboolean
thunar_list_model_has_default_sort_func (GtkTreeSortable *sortable)
{
  return FALSE;
}



static gint
thunar_list_model_cmp_func (gconstpointer a,
                            gconstpointer b,
                            gpointer      user_data)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (user_data);
  gboolean         isdir_a;
  gboolean         isdir_b;
  gboolean         is_hidden_a;
  gboolean         is_hidden_b;

  _thunar_return_val_if_fail (THUNAR_IS_FILE (a), 0);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (b), 0);

  if (G_LIKELY (store->sort_folders_first))
    {
      isdir_a = thunar_file_is_directory (a);
      isdir_b = thunar_file_is_directory (b);
      if (isdir_a != isdir_b)
        return isdir_a ? -1 : 1;
    }

  if (store->sort_hidden_last)
    {
      is_hidden_a = thunar_file_is_hidden (a);
      is_hidden_b = thunar_file_is_hidden (b);
      if (is_hidden_a != is_hidden_b)
        return is_hidden_a ? 1 : -1;
    }

  return (*store->sort_func) (a, b, store->sort_case_sensitive) * store->sort_sign;
}



static void
thunar_list_model_sort (ThunarListModel *store)
{
  GtkTreePath    *path;
  GSequenceIter **old_order;
  gint           *new_order;
  gint            n;
  gint            length;
  GSequenceIter  *row;

  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  length = g_sequence_get_length (store->rows);
  if (G_UNLIKELY (length <= 1))
    return;

  /* be sure to not overuse the stack */
  if (G_LIKELY (length < STACK_ALLOC_LIMIT))
    {
      old_order = g_newa (GSequenceIter *, length);
      new_order = g_newa (gint, length);
    }
  else
    {
      old_order = g_new (GSequenceIter *, length);
      new_order = g_new (gint, length);
    }

  /* store old order */
  row = g_sequence_get_begin_iter (store->rows);
  for (n = 0; n < length; ++n)
    {
      old_order[n] = row;
      row = g_sequence_iter_next (row);
    }

  /* sort */
  g_sequence_sort (store->rows, thunar_list_model_cmp_func, store);

  /* new_order[newpos] = oldpos */
  for (n = 0; n < length; ++n)
    new_order[g_sequence_iter_get_position (old_order[n])] = n;

  /* tell the view about the new item order */
  path = gtk_tree_path_new_first ();
  gtk_tree_model_rows_reordered (GTK_TREE_MODEL (store), path, NULL, new_order);
  gtk_tree_path_free (path);

  /* clean up if we used the heap */
  if (G_UNLIKELY (length >= STACK_ALLOC_LIMIT))
    {
      g_free (old_order);
      g_free (new_order);
    }
}



static void
thunar_list_model_files_changed (ThunarFolder    *folder,
                                 GHashTable      *files,
                                 ThunarListModel *store)
{
  ThunarFile    *file;
  GSequenceIter *row;
  GSequenceIter *end;
  gint           pos_after;
  gint           pos_before = 0;
  gint          *new_order;
  gint           length;
  gint           i, j;
  GtkTreePath   *path;
  GtkTreeIter    iter;
  GSList        *hidden_link = NULL;
  GSList        *lp;

  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  row = g_sequence_get_begin_iter (store->rows);
  end = g_sequence_get_end_iter (store->rows);
  for (; row != end; row = g_sequence_iter_next (row))
    {
      /* check if that file was changed */
      file = g_hash_table_lookup (files, g_sequence_get (row));
      if (file == NULL)
        continue;

      pos_before = g_sequence_iter_get_position (row);

      /* this file is hidden now & show_hidden is FALSE
       * so we should remove this file from the view and store
       * it in the hidden list */
      if (thunar_file_is_hidden (file) && !store->show_hidden)
        {
          GHashTable *hidden_files;

          hidden_link = g_slist_find (store->hidden, file);
          if (hidden_link == NULL)
            store->hidden = g_slist_prepend (store->hidden, g_object_ref (file));
          hidden_files = g_hash_table_new (g_direct_hash, NULL);
          g_hash_table_add (hidden_files, file);
          thunar_list_model_files_removed (store->folder, hidden_files, store);
          g_hash_table_destroy (hidden_files);
          return;
        }

      /* generate the iterator for this row */
      GTK_TREE_ITER_INIT (iter, store->stamp, row);

      /* check if the sorting changed */
      g_sequence_sort_changed (row, thunar_list_model_cmp_func, store);
      pos_after = g_sequence_iter_get_position (row);

      /* if 'g_sequence_sort_changed' changed the sorting, the positions will differ now */
      if (pos_after != pos_before)
        {
          /* do swap sorting here since its much faster than a complete sort */
          length = g_sequence_get_length (store->rows);
          if (G_LIKELY (length < STACK_ALLOC_LIMIT))
            new_order = g_newa (gint, length);
          else
            new_order = g_new (gint, length);

          /* new_order[newpos] = oldpos */
          for (i = 0, j = 0; i < length; ++i)
            {
              if (G_UNLIKELY (i == pos_after))
                {
                  new_order[i] = pos_before;
                }
              else
                {
                  if (G_UNLIKELY (j == pos_before))
                    j++;
                  new_order[i] = j++;
                }
            }

          /* tell the view about the new item order */
          path = gtk_tree_path_new_first ();
          gtk_tree_model_rows_reordered (GTK_TREE_MODEL (store), path, NULL, new_order);
          gtk_tree_path_free (path);

          /* clean up if we used the heap */
          if (G_UNLIKELY (length >= STACK_ALLOC_LIMIT))
            g_free (new_order);

          store->file_was_sorted = TRUE;
        }
      else
        {
          /* just notify the view that it has to redraw the file */
          path = gtk_tree_path_new_from_indices (pos_before, -1);
          gtk_tree_model_row_changed (GTK_TREE_MODEL (store), path, &iter); // this line
          gtk_tree_path_free (path);
        }

    } /* end while loop (for each row) */


  /* maybe this file was a hidden file but now it's not
   * in such a case we need to emit a "files-added" for this file
   * and remove it from the hidden list */
  lp = store->hidden;
  while (lp != NULL)
    {
      GSList     *next = lp->next;
      GHashTable *hidden_files;

      hidden_link = g_hash_table_lookup (files, lp->data);
      if (hidden_link == NULL || thunar_file_is_hidden (THUNAR_FILE (hidden_link)))
        {
          lp = next;
          continue;
        }

      /* remove the file from our internal hidden-list, if it is not hidden any more */
      g_object_unref (lp->data);
      store->hidden = g_slist_delete_link (store->hidden, lp);

      /* emit "files-added" for the file */
      hidden_files = g_hash_table_new (g_direct_hash, NULL);
      g_hash_table_add (hidden_files, hidden_link);
      thunar_list_model_files_added (store->folder, hidden_files, store);
      g_hash_table_destroy (hidden_files);

      lp = next;
    }
}



static void
thunar_list_model_folder_destroy (ThunarFolder    *folder,
                                  ThunarListModel *store)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));
  _thunar_return_if_fail (THUNAR_IS_FOLDER (folder));

  thunar_list_model_set_folder (THUNAR_STANDARD_VIEW_MODEL (store), NULL, NULL);

  /* TODO: What to do when the folder is deleted? */
}



static void
thunar_list_model_folder_error (ThunarFolder    *folder,
                                const GError    *error,
                                ThunarListModel *store)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));
  _thunar_return_if_fail (THUNAR_IS_FOLDER (folder));
  _thunar_return_if_fail (error != NULL);

  /* reset the current folder */
  thunar_list_model_set_folder (THUNAR_STANDARD_VIEW_MODEL (store), NULL, NULL);

  /* forward the error signal */
  g_signal_emit_by_name (G_OBJECT (store), "error", error);
}



static void
thunar_list_model_notify_loading (ThunarFolder    *folder,
                                  GParamSpec      *pspec,
                                  ThunarListModel *model)
{
  _thunar_return_if_fail (THUNAR_IS_FOLDER (folder));
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (model));

  if (!thunar_folder_get_loading (folder))
    thunar_list_model_set_loading (model, FALSE);
}



static void
thunar_list_model_files_added (ThunarFolder    *folder,
                               GHashTable      *files,
                               ThunarListModel *store)
{
  GHashTable    *filtered;
  ThunarFile    *file;
  gboolean       matched;
  gchar         *name_n;
  gpointer       key;
  GHashTableIter iter;

  /* pass the list directly if not currently showing search results */
  if (store->search_terms == NULL)
    {
      thunar_list_model_insert_files (store, files);
      return;
    }

  /* otherwise, filter out files that don't match the current search terms */
  filtered = g_hash_table_new (g_direct_hash, NULL);

  g_hash_table_iter_init (&iter, files);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      file = THUNAR_FILE (key);
      if (THUNAR_IS_FILE (file) == FALSE)
        {
          g_warning ("failed to add file to search results");
          continue;
        }

      name_n = (gchar *) thunar_file_get_display_name (file);
      if (name_n == NULL)
        {
          g_warning ("failed to get display name");
          continue;
        }

      name_n = thunar_g_utf8_normalize_for_search (name_n, TRUE, TRUE);
      matched = thunar_util_search_terms_match (store->search_terms, name_n);
      g_free (name_n);

      if (matched)
        g_hash_table_add (filtered, file);
    }
  thunar_list_model_insert_files (store, filtered);
  g_hash_table_destroy (filtered);
}


static void
thunar_list_model_insert_files (ThunarListModel *store,
                                GHashTable      *files)
{
  GtkTreePath   *path;
  GtkTreeIter    iter;
  ThunarFile    *file;
  gint          *indices;
  GSequenceIter *row;
  gboolean       has_handler;
  gboolean       search_mode;
  gpointer       key;
  GHashTableIter file_iter;

  /* we use a simple trick here to avoid allocating
   * GtkTreePath's again and again, by simply accessing
   * the indices directly and only modifying the first
   * item in the integer array... looks a hack, eh?
   */
  path = gtk_tree_path_new_first ();
  indices = gtk_tree_path_get_indices (path);

  /* check if we have any handlers connected for "row-inserted" */
  has_handler = g_signal_has_handler_pending (G_OBJECT (store), store->row_inserted_id, 0, FALSE);

  /* process all added files */
  search_mode = (store->search_terms != NULL);
  g_hash_table_iter_init (&file_iter, files);
  while (g_hash_table_iter_next (&file_iter, &key, NULL))
    {
      /* take a reference on that file */
      file = THUNAR_FILE (g_object_ref (G_OBJECT (key)));
      _thunar_return_if_fail (THUNAR_IS_FILE (file));

      /* check if the file should be stashed in the hidden list */
      /* The ->hidden list is an optimization used by the model when
       * it is not being used to store search results. In the search
       * case, we simply restart the search, */
      if (!store->show_hidden && thunar_file_is_hidden (file))
        {
          if (search_mode == FALSE)
            store->hidden = g_slist_prepend (store->hidden, file);
          else
            g_object_unref (file);
        }
      else
        {
          /* insert the file */
          row = g_sequence_insert_sorted (store->rows, file,
                                          thunar_list_model_cmp_func, store);

          if (has_handler)
            {
              /* generate an iterator for the new item */
              GTK_TREE_ITER_INIT (iter, store->stamp, row);

              indices[0] = g_sequence_iter_get_position (row);
              gtk_tree_model_row_inserted (GTK_TREE_MODEL (store), path, &iter);
            }
        }
    }

  /* release the path */
  gtk_tree_path_free (path);

  /* number of visible files may have changed */
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_NUM_FILES]);
}



static void
thunar_list_model_files_removed (ThunarFolder    *folder,
                                 GHashTable      *files,
                                 ThunarListModel *store)
{
  GSequenceIter *row;
  GSequenceIter *end;
  GSequenceIter *next;
  GtkTreePath   *path;
  gboolean       found;
  gboolean       search_mode;
  gpointer       key;
  ThunarFile    *file;
  GHashTableIter iter;

  /* drop all the referenced files from the model */
  search_mode = (store->search_terms != NULL);
  g_hash_table_iter_init (&iter, files);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      file = THUNAR_FILE (G_OBJECT (key));

      row = g_sequence_get_begin_iter (store->rows);
      end = g_sequence_get_end_iter (store->rows);

      found = FALSE;

      while (row != end)
        {
          next = g_sequence_iter_next (row);

          if (g_sequence_get (row) == file)
            {
              /* indicate that file was removed from this model */
              store->file_was_removed = TRUE;

              /* setup path for "row-deleted" */
              path = gtk_tree_path_new_from_indices (g_sequence_iter_get_position (row), -1);

              /* remove file from the model */
              g_sequence_remove (row);

              /* notify the view(s) */
              gtk_tree_model_row_deleted (GTK_TREE_MODEL (store), path);
              gtk_tree_path_free (path);

              /* no need to look in the hidden files */
              found = TRUE;

              break;
            }

          row = next;
        }

      /* check if the file was found */
      if (!found)
        {
          if (search_mode == FALSE)
            {
              /* file is hidden */
              /* this only makes sense when not storing search results */
              _thunar_assert (g_slist_find (store->hidden, file) != NULL);
              store->hidden = g_slist_remove (store->hidden, file);
              g_object_unref (G_OBJECT (file));
            }
        }
    }

  /* this probably changed */
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_NUM_FILES]);
}



/**
 * thunar_list_model_new:
 *
 * Allocates a new #ThunarListModel not associated with
 * any #ThunarFolder.
 *
 * Return value: the newly allocated #ThunarListModel.
 **/
ThunarStandardViewModel *
thunar_list_model_new (void)
{
  return g_object_new (THUNAR_TYPE_LIST_MODEL, NULL);
}



/**
 * thunar_list_model_check_file_in_model_before_use:
 *
 * Enables file removed workaround to prevent crash.
 **/
void
thunar_list_model_check_file_in_model_before_use (ThunarListModel *model)
{
  model->check_file_in_model_before_use = TRUE;
}



/**
 * thunar_list_model_get_case_sensitive:
 * @store : a valid #ThunarListModel object.
 *
 * Returns %TRUE if the sorting is done in a case-sensitive
 * manner for @store.
 *
 * Return value: %TRUE if sorting is case-sensitive.
 **/
static gboolean
thunar_list_model_get_case_sensitive (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->sort_case_sensitive;
}



/**
 * thunar_list_model_set_case_sensitive:
 * @store          : a valid #ThunarListModel object.
 * @case_sensitive : %TRUE to use case-sensitive sort for @store.
 *
 * If @case_sensitive is %TRUE the sorting in @store will be done
 * in a case-sensitive manner.
 **/
static void
thunar_list_model_set_case_sensitive (ThunarListModel *store,
                                      gboolean         case_sensitive)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* normalize the setting */
  case_sensitive = !!case_sensitive;

  /* check if we have a new setting */
  if (store->sort_case_sensitive != case_sensitive)
    {
      /* apply the new setting */
      store->sort_case_sensitive = case_sensitive;

      /* resort the model with the new setting */
      thunar_list_model_sort (store);

      /* notify listeners */
      g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_CASE_SENSITIVE]);

      /* emit a "changed" signal for each row, so the display is
         reloaded with the new case-sensitive setting */
      gtk_tree_model_foreach (GTK_TREE_MODEL (store),
                              (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed,
                              NULL);
    }
}



/**
 * thunar_list_model_get_date_style:
 * @store : a valid #ThunarListModel object.
 *
 * Return value: the #ThunarDateStyle used to format dates in
 *               the given @store.
 **/
static ThunarDateStyle
thunar_list_model_get_date_style (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), THUNAR_DATE_STYLE_SIMPLE);
  return store->date_style;
}



/**
 * thunar_list_model_set_date_style:
 * @store      : a valid #ThunarListModel object.
 * @date_style : the #ThunarDateStyle that should be used to format
 *               dates in the @store.
 *
 * Chances the style used to format dates in @store to the specified
 * @date_style.
 **/
static void
thunar_list_model_set_date_style (ThunarListModel *store,
                                  ThunarDateStyle  date_style)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* check if we have a new setting */
  if (store->date_style != date_style)
    {
      /* apply the new setting */
      store->date_style = date_style;

      /* notify listeners */
      g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_DATE_STYLE]);

      /* emit a "changed" signal for each row, so the display is reloaded with the new date style */
      gtk_tree_model_foreach (GTK_TREE_MODEL (store),
                              (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed,
                              NULL);
    }
}



/**
 * thunar_list_model_get_date_custom_style:
 * @store : a valid #ThunarListModel object.
 *
 * Return value: the style used to format customdates in the given @store.
 **/
static const char *
thunar_list_model_get_date_custom_style (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), NULL);
  return store->date_custom_style;
}



/**
 * thunar_list_model_set_date_custom_style:
 * @store             : a valid #ThunarListModel object.
 * @date_custom_style : the style that should be used to format
 *                      custom dates in the @store.
 *
 * Changes the style used to format custom dates in @store to the specified @date_custom_style.
 **/
static void
thunar_list_model_set_date_custom_style (ThunarListModel *store,
                                         const char      *date_custom_style)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* check if we have a new setting */
  if (g_strcmp0 (store->date_custom_style, date_custom_style) != 0)
    {
      /* apply the new setting */
      g_free (store->date_custom_style);
      store->date_custom_style = g_strdup (date_custom_style);

      /* notify listeners */
      g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_DATE_CUSTOM_STYLE]);

      /* emit a "changed" signal for each row, so the display is reloaded with the new date style */
      gtk_tree_model_foreach (GTK_TREE_MODEL (store),
                              (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed,
                              NULL);
    }
}



static ThunarJob *
thunar_list_model_get_job (ThunarStandardViewModel *model)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  return store->recursive_search_job;
}



static void
thunar_list_model_set_job (ThunarStandardViewModel *model,
                           ThunarJob               *job)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  store->recursive_search_job = job;
}



static void
thunar_list_model_add_search_files (ThunarStandardViewModel *model,
                                    GList                   *files)
{
  ThunarListModel *_model = THUNAR_LIST_MODEL (model);

  g_mutex_lock (&_model->mutex_files_to_add);

  for (GList *lp = files; lp != NULL; lp = g_list_next (lp))
    g_hash_table_add (_model->files_to_add, lp->data);

  g_mutex_unlock (&_model->mutex_files_to_add);
}



static gboolean
thunar_list_model_update_search_files (ThunarListModel *model)
{
  g_mutex_lock (&model->mutex_files_to_add);

  if (model->files_to_add != NULL)
    {
      thunar_list_model_insert_files (model, model->files_to_add);
      g_hash_table_remove_all (model->files_to_add);
    }

  g_mutex_unlock (&model->mutex_files_to_add);

  return TRUE;
}



static void
thunar_list_model_cancel_search_job (ThunarListModel *model)
{
  /* cancel the ongoing search if there is one */
  if (model->recursive_search_job)
    {
      thunar_job_cancel (THUNAR_JOB (model->recursive_search_job));

      g_signal_handlers_disconnect_by_data (model->recursive_search_job, model);
      g_object_unref (model->recursive_search_job);
      model->recursive_search_job = NULL;
    }
}



G_NORETURN
static void
thunar_list_model_search_error (ThunarJob *job)
{
  g_error ("Error while searching recursively");
}



static void
thunar_list_model_search_finished (ThunarJob       *job,
                                   ThunarListModel *store)
{
  if (store->recursive_search_job)
    {
      g_signal_handlers_disconnect_by_data (store->recursive_search_job, store);
      g_object_unref (store->recursive_search_job);
      store->recursive_search_job = NULL;
    }

  if (store->update_search_results_timeout_id > 0)
    {
      thunar_list_model_update_search_files (store);
      g_source_remove (store->update_search_results_timeout_id);
      store->update_search_results_timeout_id = 0;
    }

  g_hash_table_remove_all (store->files_to_add);

  g_signal_emit_by_name (store, "search-done");
}



/**
 * thunar_list_model_get_folder:
 * @store : a valid #ThunarListModel object.
 *
 * Return value: the #ThunarFolder @store is associated with
 *               or %NULL if @store has no folder.
 **/
static ThunarFolder *
thunar_list_model_get_folder (ThunarStandardViewModel *model)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), NULL);
  return store->folder;
}



/**
 * thunar_list_model_set_folder:
 * @store                       : a valid #ThunarListModel.
 * @folder                      : a #ThunarFolder or %NULL.
 * @search_query                : a #string or %NULL.
 **/
static void
thunar_list_model_set_folder (ThunarStandardViewModel *model,
                              ThunarFolder            *folder,
                              gchar                   *search_query)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  GtkTreePath     *path;
  gboolean         has_handler;
  GHashTable      *files;
  GSequenceIter   *row;
  GSequenceIter   *end;
  GSequenceIter   *next;

  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));
  _thunar_return_if_fail (folder == NULL || THUNAR_IS_FOLDER (folder));

  /* unlink from the previously active folder (if any) */
  if (G_LIKELY (store->folder != NULL))
    {
      thunar_list_model_cancel_search_job (store);

      if (store->update_search_results_timeout_id > 0)
        {
          g_source_remove (store->update_search_results_timeout_id);
          store->update_search_results_timeout_id = 0;
        }
      g_hash_table_remove_all (store->files_to_add);

      /* check if we have any handlers connected for "row-deleted" */
      has_handler = g_signal_has_handler_pending (G_OBJECT (store), store->row_deleted_id, 0, FALSE);

      row = g_sequence_get_begin_iter (store->rows);
      end = g_sequence_get_end_iter (store->rows);

      /* remove existing entries */
      path = gtk_tree_path_new_first ();
      while (row != end)
        {
          /* remove the row from the list */
          next = g_sequence_iter_next (row);
          g_sequence_remove (row);
          row = next;

          /* notify the view(s) if they're actually
           * interested in the "row-deleted" signal.
           */
          if (G_LIKELY (has_handler))
            gtk_tree_model_row_deleted (GTK_TREE_MODEL (store), path);
        }
      gtk_tree_path_free (path);

      /* remove hidden entries */
      g_slist_free_full (store->hidden, g_object_unref);
      store->hidden = NULL;

      /* reset the information that file was removed or sorted */
      store->file_was_removed = FALSE;
      store->file_was_sorted = FALSE;

      /* unregister signals and drop the reference */
      g_signal_handlers_disconnect_by_data (G_OBJECT (store->folder), store);
      g_object_unref (G_OBJECT (store->folder));
    }

  /* ... just to be sure! */
  _thunar_assert (g_sequence_get_length (store->rows) == 0);

#ifndef NDEBUG
  /* new stamp since the model changed */
  store->stamp = g_random_int ();
#endif

  /* activate the new folder */
  store->folder = folder;

  /* freeze */
  g_object_freeze_notify (G_OBJECT (store));

  /* connect to the new folder (if any) */
  if (folder != NULL)
    {
      g_object_ref (G_OBJECT (folder));

      /* get the already loaded files or search for files matching the search_query
       * don't start searching if the query is empty, that would be a waste of resources
       */
      if (search_query == NULL || strlen (g_strstrip (search_query)) == 0)
        {
          files = thunar_folder_get_files (folder);
          thunar_list_model_set_loading (store, TRUE);

          if (store->search_terms != NULL)
            {
              g_strfreev (store->search_terms);
              store->search_terms = NULL;
            }
        }
      else
        {
          gchar *search_query_c; /* normalized */

          search_query_c = thunar_g_utf8_normalize_for_search (search_query, TRUE, TRUE);
          g_strfreev (store->search_terms);
          store->search_terms = thunar_util_split_search_query (search_query_c, NULL);
          if (store->search_terms != NULL)
            {
              /* search the current folder
               * start a new recursive_search_job */
              store->recursive_search_job = thunar_io_jobs_search_directory (THUNAR_STANDARD_VIEW_MODEL (store), search_query_c, thunar_folder_get_corresponding_file (folder));
              thunar_job_launch (THUNAR_JOB (store->recursive_search_job));

              g_signal_connect (store->recursive_search_job, "error", G_CALLBACK (thunar_list_model_search_error), NULL);
              g_signal_connect (store->recursive_search_job, "finished", G_CALLBACK (thunar_list_model_search_finished), store);

              /* add new results to the model every X ms */
              store->update_search_results_timeout_id = g_timeout_add (500, G_SOURCE_FUNC (thunar_list_model_update_search_files), store);
            }
          g_free (search_query_c);
          files = NULL;
        }

      /* insert the files */
      if (files != NULL)
        thunar_list_model_insert_files (store, files);

      /* connect signals to the new folder */
      g_signal_connect (G_OBJECT (store->folder), "destroy", G_CALLBACK (thunar_list_model_folder_destroy), store);
      g_signal_connect (G_OBJECT (store->folder), "error", G_CALLBACK (thunar_list_model_folder_error), store);
      g_signal_connect (G_OBJECT (store->folder), "files-added", G_CALLBACK (thunar_list_model_files_added), store);
      g_signal_connect (G_OBJECT (store->folder), "files-removed", G_CALLBACK (thunar_list_model_files_removed), store);
      g_signal_connect (G_OBJECT (store->folder), "files-changed", G_CALLBACK (thunar_list_model_files_changed), store);
      g_signal_connect (G_OBJECT (store->folder), "notify::loading", G_CALLBACK (thunar_list_model_notify_loading), store);

      /* If the folder is already loaded, directly update the loading state */
      if (!thunar_folder_get_loading (store->folder))
        thunar_list_model_notify_loading (store->folder, NULL, store);
    }

  /* notify listeners that we have a new folder */
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_FOLDER]);
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_NUM_FILES]);
  g_object_thaw_notify (G_OBJECT (store));
}



/**
 * thunar_list_model_get_folders_first:
 * @store : a #ThunarListModel.
 *
 * Determines whether @store lists folders first.
 *
 * Return value: %TRUE if @store lists folders first.
 **/
static gboolean
thunar_list_model_get_folders_first (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->sort_folders_first;
}



/**
 * thunar_list_model_set_folders_first:
 * @store         : a #ThunarListModel.
 * @folders_first : %TRUE to let @store list folders first.
 **/
static void
thunar_list_model_set_folders_first (ThunarStandardViewModel *model,
                                     gboolean                 folders_first)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* check if the new setting differs */
  if ((store->sort_folders_first && folders_first)
      || (!store->sort_folders_first && !folders_first))
    return;

  /* apply the new setting (re-sorting the store) */
  store->sort_folders_first = folders_first;
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_FOLDERS_FIRST]);
  thunar_list_model_sort (store);

  /* emit a "changed" signal for each row, so the display is
     reloaded with the new folders first setting */
  gtk_tree_model_foreach (GTK_TREE_MODEL (store),
                          (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed,
                          NULL);
}


/**
 * thunar_list_model_get_hidden_last:
 * @store : a #ThunarListModel.
 *
 * Determines whether @store lists hidden files last.
 *
 * Return value: %TRUE if @store lists hidden files last.
 **/
static gboolean
thunar_list_model_get_hidden_last (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->sort_hidden_last;
}



/**
 * thunar_list_model_set_hidden_last:
 * @store       : a #ThunarListModel.
 * @hidden_last : %TRUE to let @store list hidden files last.
 **/
static void
thunar_list_model_set_hidden_last (ThunarStandardViewModel *model,
                                   gboolean                 hidden_last)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* check if the new setting differs */
  if ((store->sort_hidden_last && hidden_last)
      || (!store->sort_hidden_last && !hidden_last))
    return;

  /* apply the new setting (re-sorting the store) */
  store->sort_hidden_last = hidden_last;
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_HIDDEN_LAST]);
  thunar_list_model_sort (store);

  /* emit a "changed" signal for each row, so the display is
     reloaded with the new hidden last setting */
  gtk_tree_model_foreach (GTK_TREE_MODEL (store),
                          (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed,
                          NULL);
}



/**
 * thunar_list_model_get_show_hidden:
 * @store : a #ThunarListModel.
 *
 * Return value: %TRUE if hidden files will be shown, else %FALSE.
 **/
static gboolean
thunar_list_model_get_show_hidden (ThunarStandardViewModel *model)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->show_hidden;
}



/**
 * thunar_list_model_set_show_hidden:
 * @store       : a #ThunarListModel.
 * @show_hidden : %TRUE if hidden files should be shown, else %FALSE.
 **/
static void
thunar_list_model_set_show_hidden (ThunarStandardViewModel *model,
                                   gboolean                 show_hidden)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  GtkTreePath     *path;
  GtkTreeIter      iter;
  ThunarFile      *file;
  GSList          *lp;
  GSequenceIter   *row;
  GSequenceIter   *next;
  GSequenceIter   *end;

  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* check if the settings differ */
  if (store->show_hidden == show_hidden)
    return;

  store->show_hidden = show_hidden;

  if (store->show_hidden)
    {
      for (lp = store->hidden; lp != NULL; lp = lp->next)
        {
          file = THUNAR_FILE (lp->data);

          /* insert file in the sorted position */
          row = g_sequence_insert_sorted (store->rows, file,
                                          thunar_list_model_cmp_func, store);

          GTK_TREE_ITER_INIT (iter, store->stamp, row);

          /* tell the view about the new row */
          path = gtk_tree_path_new_from_indices (g_sequence_iter_get_position (row), -1);
          gtk_tree_model_row_inserted (GTK_TREE_MODEL (store), path, &iter);
          gtk_tree_path_free (path);
        }
      g_slist_free (store->hidden);
      store->hidden = NULL;
    }
  else
    {
      _thunar_assert (store->hidden == NULL);

      /* remove all hidden files */
      row = g_sequence_get_begin_iter (store->rows);
      end = g_sequence_get_end_iter (store->rows);

      while (row != end)
        {
          next = g_sequence_iter_next (row);

          file = g_sequence_get (row);
          if (thunar_file_is_hidden (file))
            {
              /* store file in the list */
              store->hidden = g_slist_prepend (store->hidden, g_object_ref (file));

              /* setup path for "row-deleted" */
              path = gtk_tree_path_new_from_indices (g_sequence_iter_get_position (row), -1);

              /* remove file from the model */
              g_sequence_remove (row);

              /* notify the view(s) */
              gtk_tree_model_row_deleted (GTK_TREE_MODEL (store), path);
              gtk_tree_path_free (path);
            }

          row = next;
          _thunar_assert (end == g_sequence_get_end_iter (store->rows));
        }
    }

  /* notify listeners about the new setting */
  g_object_freeze_notify (G_OBJECT (store));
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_NUM_FILES]);
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_SHOW_HIDDEN]);
  g_object_thaw_notify (G_OBJECT (store));
}



/**
 * thunar_list_model_get_file_size_binary:
 * @store : a valid #ThunarListModel object.
 *
 * Returns %TRUE if the file size should be formatted
 * as binary.
 *
 * Return value: %TRUE if file size format is binary.
 **/
static gboolean
thunar_list_model_get_file_size_binary (ThunarStandardViewModel *model)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->file_size_binary;
}



/**
 * thunar_list_model_set_file_size_binary:
 * @store            : a valid #ThunarListModel object.
 * @file_size_binary : %TRUE to format file size as binary.
 *
 * If @file_size_binary is %TRUE the file size should be
 * formatted as binary.
 **/
static void
thunar_list_model_set_file_size_binary (ThunarStandardViewModel *model,
                                        gboolean                 file_size_binary)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* normalize the setting */
  file_size_binary = !!file_size_binary;

  /* check if we have a new setting */
  if (store->file_size_binary != file_size_binary)
    {
      /* apply the new setting */
      store->file_size_binary = file_size_binary;

      /* resort the model with the new setting */
      thunar_list_model_sort (store);

      /* notify listeners */
      g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_FILE_SIZE_BINARY]);

      /* emit a "changed" signal for each row, so the display is
         reloaded with the new binary file size setting */
      gtk_tree_model_foreach (GTK_TREE_MODEL (store),
                              (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed,
                              NULL);
    }
}



/**
 * thunar_list_model_get_folder_item_count:
 * @store : a #ThunarListModel.
 *
 * Return value: A value of the enum #ThunarFolderItemCount
 **/
static gint
thunar_list_model_get_folder_item_count (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->folder_item_count;
}



/**
 * thunar_list_model_get_loading:
 * @store : a #ThunarListModel.
 *
 * Return value: %TRUE if @store is yet loading a folder.
 **/
static gboolean
thunar_list_model_get_loading (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), FALSE);
  return store->loading;
}



/**
 * thunar_list_model_set_loading:
 * @store : a #ThunarListModel.
 *
 * sets model's loading property to @loading.
 *
 * Return value: void
 **/
static void
thunar_list_model_set_loading (ThunarListModel *store,
                               gboolean         loading)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  if (store->loading == loading)
    return;

  store->loading = loading;

  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_LOADING]);
}



/**
 * thunar_list_model_set_folder_item_count:
 * @store             : a #ThunarListModel.
 * @count_as_dir_size : a value of the enum #ThunarFolderItemCount
 **/
static void
thunar_list_model_set_folder_item_count (ThunarListModel      *store,
                                         ThunarFolderItemCount count_as_dir_size)
{
  _thunar_return_if_fail (THUNAR_IS_LIST_MODEL (store));

  /* check if the new setting differs */
  if (store->folder_item_count == count_as_dir_size)
    return;

  store->folder_item_count = count_as_dir_size;
  g_object_notify_by_pspec (G_OBJECT (store), list_model_props[PROP_FOLDER_ITEM_COUNT]);

  gtk_tree_model_foreach (GTK_TREE_MODEL (store), (GtkTreeModelForeachFunc) (void (*) (void)) gtk_tree_model_row_changed, NULL);

  /* re-sorting the store if needed */
  if (store->sort_func == thunar_cmp_files_by_size || store->sort_func == (ThunarSortFunc) thunar_cmp_files_by_size_and_items_count)
    {
      store->sort_func = (store->folder_item_count != THUNAR_FOLDER_ITEM_COUNT_NEVER) ? (ThunarSortFunc) thunar_cmp_files_by_size_and_items_count : thunar_cmp_files_by_size;
      thunar_list_model_sort (store);
    }
}



/**
 * thunar_list_model_get_file:
 * @store : a #ThunarListModel.
 * @iter  : a valid #GtkTreeIter for @store.
 *
 * Returns the #ThunarFile referred to by @iter. Free
 * the returned object using #g_object_unref() when
 * you are done with it.
 *
 * Returns : (transfer full) (nullable): The #ThunarFile, or NULL if no file is set for @iter
 **/
static ThunarFile *
thunar_list_model_get_file (ThunarStandardViewModel *model,
                            GtkTreeIter             *iter)
{
  ThunarFile *file;

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (model), NULL);
  _thunar_return_val_if_fail (iter->stamp == THUNAR_LIST_MODEL (model)->stamp, NULL);

  file = g_sequence_get (iter->user_data);

  if (file != NULL)
    g_object_ref (file);

  return file;
}



/**
 * thunar_list_model_get_num_files:
 * @store : a #ThunarListModel.
 *
 * Counts the number of visible files for @store, taking into account the
 * current setting of "show-hidden".
 *
 * Return value: ahe number of visible files in @store.
 **/
static gint
thunar_list_model_get_num_files (ThunarListModel *store)
{
  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), 0);
  return g_sequence_get_length (store->rows);
}



/**
 * thunar_list_model_get_paths_for_files:
 * @store : a #ThunarListModel instance.
 * @files : a list of #ThunarFile<!---->s.
 *
 * Determines the list of #GtkTreePath<!---->s for the #ThunarFile<!---->s
 * found in the @files list. If a #ThunarFile from the @files list is not
 * available in @store, no #GtkTreePath will be returned for it. So, in effect,
 * only #GtkTreePath<!---->s for the subset of @files available in @store will
 * be returned.
 *
 * The caller is responsible to free the returned list using:
 * <informalexample><programlisting>
 * g_list_free_full (list, (GDestroyNotify) gtk_tree_path_free);
 * </programlisting></informalexample>
 *
 * Return value: the list of #GtkTreePath<!---->s for @files.
 **/
static GList *
thunar_list_model_get_paths_for_files (ThunarStandardViewModel *model,
                                       GList                   *files)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);
  GList           *paths = NULL;
  GSequenceIter   *row;
  GSequenceIter   *end;
  gint             i = 0;

  _thunar_return_val_if_fail (THUNAR_IS_LIST_MODEL (store), NULL);

  row = g_sequence_get_begin_iter (store->rows);
  end = g_sequence_get_end_iter (store->rows);

  /* find the rows for the given files */
  while (row != end)
    {
      if (g_list_find (files, g_sequence_get (row)) != NULL)
        {
          _thunar_assert (i == g_sequence_iter_get_position (row));
          paths = g_list_prepend (paths, gtk_tree_path_new_from_indices (i, -1));
        }

      row = g_sequence_iter_next (row);
      i++;
    }

  return paths;
}



static void
thunar_list_model_file_count_callback (ThunarJob *job,
                                       gpointer   model)
{
  ThunarListModel *store = THUNAR_LIST_MODEL (model);

  GArray     *param_values;
  ThunarFile *file;
  GHashTable *files;

  if (job == NULL)
    return;

  param_values = thunar_simple_job_get_param_values (THUNAR_SIMPLE_JOB (job));
  file = THUNAR_FILE (g_value_get_object (&g_array_index (param_values, GValue, 0)));

  if (file == NULL)
    return;

  files = g_hash_table_new (g_direct_hash, NULL);
  g_hash_table_add (files, file);
  thunar_list_model_files_changed (store->folder, files, store);
  g_hash_table_destroy (files);
}
