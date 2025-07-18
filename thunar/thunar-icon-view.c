/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2005-2006 Benedikt Meurer <benny@xfce.org>
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

#include "thunar/thunar-icon-view.h"
#include "thunar/thunar-private.h"

#include <libxfce4ui/libxfce4ui.h>



/* Property identifiers */
enum
{
  PROP_0,
  PROP_TEXT_BESIDE_ICONS,
};



static void
thunar_icon_view_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec);
static AtkObject *
thunar_icon_view_get_accessible (GtkWidget *widget);
static void
thunar_icon_view_style_set (GtkWidget *widget,
                            GtkStyle  *previous_style);
static void
thunar_icon_view_set_consistent_horizontal_spacing (ThunarIconView *icon_view);
static void
thunar_icon_view_zoom_level_changed (ThunarStandardView *standard_view);



struct _ThunarIconViewClass
{
  ThunarAbstractIconViewClass __parent__;
};

struct _ThunarIconView
{
  ThunarAbstractIconView __parent__;
};



G_DEFINE_TYPE (ThunarIconView, thunar_icon_view, THUNAR_TYPE_ABSTRACT_ICON_VIEW)



static void
thunar_icon_view_class_init (ThunarIconViewClass *klass)
{
  ThunarStandardViewClass *thunarstandard_view_class;
  GtkWidgetClass          *gtkwidget_class;
  GObjectClass            *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->set_property = thunar_icon_view_set_property;

  gtkwidget_class = GTK_WIDGET_CLASS (klass);
  gtkwidget_class->style_set = thunar_icon_view_style_set;
  gtkwidget_class->get_accessible = thunar_icon_view_get_accessible;

  thunarstandard_view_class = THUNAR_STANDARD_VIEW_CLASS (klass);
  thunarstandard_view_class->zoom_level_property_name = "last-icon-view-zoom-level";

  /**
   * ThunarIconView::text-beside-icons:
   *
   * Write-only property to specify whether text should be
   * display besides the icon rather than below.
   **/
  g_object_class_install_property (gobject_class,
                                   PROP_TEXT_BESIDE_ICONS,
                                   g_param_spec_boolean ("text-beside-icons",
                                                         "text-beside-icons",
                                                         "text-beside-icons",
                                                         FALSE,
                                                         G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
}



static void
thunar_icon_view_init (ThunarIconView *icon_view)
{
  /* setup the icon renderer */
  g_object_set (G_OBJECT (THUNAR_STANDARD_VIEW (icon_view)->icon_renderer),
                "ypad", 1u,
                NULL);

  /* setup the name renderer */
  g_object_set (G_OBJECT (THUNAR_STANDARD_VIEW (icon_view)->name_renderer),
                "wrap-mode", PANGO_WRAP_WORD_CHAR,
                NULL);

  /* synchronize the "text-beside-icons" property with the global preference */
  g_object_bind_property (G_OBJECT (THUNAR_STANDARD_VIEW (icon_view)->preferences),
                          "misc-text-beside-icons",
                          G_OBJECT (icon_view),
                          "text-beside-icons",
                          G_BINDING_SYNC_CREATE);
}



static void
thunar_icon_view_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ThunarStandardView *standard_view = THUNAR_STANDARD_VIEW (object);

  switch (prop_id)
    {
    case PROP_TEXT_BESIDE_ICONS:
      if (G_UNLIKELY (g_value_get_boolean (value)))
        {
          xfce_icon_view_set_orientation (XFCE_ICON_VIEW (gtk_bin_get_child (GTK_BIN (standard_view))), GTK_ORIENTATION_HORIZONTAL);
          g_object_set (G_OBJECT (standard_view->name_renderer), "wrap-width", 128, "yalign", 0.5f, "xalign", 0.0f, "alignment", PANGO_ALIGN_LEFT, NULL);

          /* disconnect the "zoom-level" signal handler, since we're using a fixed wrap-width here */
          g_signal_handlers_disconnect_by_func (object, thunar_icon_view_zoom_level_changed, NULL);

          /* reset consistent horizontal spacing */
          thunar_icon_view_set_consistent_horizontal_spacing (THUNAR_ICON_VIEW (standard_view));
        }
      else
        {
          xfce_icon_view_set_orientation (XFCE_ICON_VIEW (gtk_bin_get_child (GTK_BIN (standard_view))), GTK_ORIENTATION_VERTICAL);
          g_object_set (G_OBJECT (standard_view->name_renderer), "yalign", 0.0f, "xalign", 0.5f, "alignment", PANGO_ALIGN_CENTER, NULL);

          /* connect the "zoom-level" signal handler as the wrap-width is now synced with the "zoom-level" */
          g_signal_connect (object, "notify::zoom-level", G_CALLBACK (thunar_icon_view_zoom_level_changed), NULL);
          thunar_icon_view_zoom_level_changed (standard_view);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static AtkObject *
thunar_icon_view_get_accessible (GtkWidget *widget)
{
  AtkObject *object;

  /* query the atk object for the icon view class */
  object = (*GTK_WIDGET_CLASS (thunar_icon_view_parent_class)->get_accessible) (widget);

  /* set custom Atk properties for the icon view */
  if (G_LIKELY (object != NULL))
    {
      atk_object_set_description (object, _("Icon based directory listing"));
      atk_object_set_name (object, _("Icon view"));
      atk_object_set_role (object, ATK_ROLE_DIRECTORY_PANE);
    }

  return object;
}



static void
thunar_icon_view_style_set (GtkWidget *widget,
                            GtkStyle  *previous_style)
{
  /* call the parent handler */
  (*GTK_WIDGET_CLASS (thunar_icon_view_parent_class)->style_set) (widget, previous_style);

  /* (re)set consistent horizontal spacing */
  thunar_icon_view_set_consistent_horizontal_spacing (THUNAR_ICON_VIEW (widget));
}



static void
thunar_icon_view_set_consistent_horizontal_spacing (ThunarIconView *icon_view)
{
  ThunarStandardView *standard_view = THUNAR_STANDARD_VIEW (icon_view);
  XfceIconView       *xfce_icon_view = XFCE_ICON_VIEW (gtk_bin_get_child (GTK_BIN (standard_view)));
  gint                wrap_width;
  gint                xpad;
  gint                column_spacing;

  if (xfce_icon_view_get_orientation (xfce_icon_view) == GTK_ORIENTATION_HORIZONTAL)
    {
      /* reset consistent horizontal spacing if text is beside icon */
      xfce_icon_view_set_item_width (xfce_icon_view, -1);
      return;
    }

  g_object_get (G_OBJECT (standard_view->name_renderer), "wrap-width", &wrap_width, NULL);
  gtk_cell_renderer_get_padding (standard_view->name_renderer, &xpad, NULL);

  column_spacing = xfce_icon_view_get_column_spacing (xfce_icon_view);

  /* set consistent horizontal spacing */
  xfce_icon_view_set_item_width (xfce_icon_view, wrap_width + MAX (0, xpad * 2 - column_spacing));
}



static void
thunar_icon_view_zoom_level_changed (ThunarStandardView *standard_view)
{
  gint            wrap_width;
  XfceIconView   *xfce_icon_view;
  ThunarZoomLevel zoom_level;

  _thunar_return_if_fail (THUNAR_IS_STANDARD_VIEW (standard_view));

  xfce_icon_view = XFCE_ICON_VIEW (gtk_bin_get_child (GTK_BIN (standard_view)));
  zoom_level = thunar_view_get_zoom_level (THUNAR_VIEW (standard_view));

  /* determine the "wrap-width" depending on the "zoom-level" */
  switch (zoom_level)
    {
    case THUNAR_ZOOM_LEVEL_25_PERCENT:
      wrap_width = 48;
      break;

    case THUNAR_ZOOM_LEVEL_38_PERCENT:
      wrap_width = 64;
      break;

    case THUNAR_ZOOM_LEVEL_50_PERCENT:
      wrap_width = 72;
      break;

    case THUNAR_ZOOM_LEVEL_75_PERCENT:
      wrap_width = 112;
      break;

    default:
      wrap_width = 128;
      break;
    }

  /* set the new "wrap-width" for the text renderer */
  g_object_set (G_OBJECT (standard_view->name_renderer), "wrap-width", wrap_width, NULL);

  /* set consistent horizontal spacing */
  thunar_icon_view_set_consistent_horizontal_spacing (THUNAR_ICON_VIEW (standard_view));

  /* Like that rubber band selection can be done properly on high zoom levels */
  /* Without margin adjustment it would be almost impossible to start the selection on the left */
  xfce_icon_view_set_margin (xfce_icon_view, thunar_zoom_level_to_view_margin (zoom_level));
}
