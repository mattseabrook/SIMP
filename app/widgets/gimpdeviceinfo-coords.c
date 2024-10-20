/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gegl.h>
#include <gtk/gtk.h>

#include "widgets-types.h"

#include "gimpdeviceinfo.h"
#include "gimpdeviceinfo-coords.h"


static const GimpCoords default_coords = GIMP_COORDS_DEFAULT_VALUES;


/*  public functions  */

gboolean
gimp_device_info_get_event_coords (GimpDeviceInfo *info,
                                   GdkWindow      *window,
                                   const GdkEvent *event,
                                   GimpCoords     *coords)
{
  gdouble x;

  if (event && gdk_event_get_axis (event, GDK_AXIS_X, &x))
    {
      *coords = default_coords;

      coords->x = x;
      gdk_event_get_axis (event, GDK_AXIS_Y, &coords->y);

      /* translate event coordinates to window coordinates, only
       * happens if we drag a guide from a ruler
       */
      if (event->any.window &&
          event->any.window != window)
        {
          GtkWidget *src_widget;
          GtkWidget *dest_widget;

          src_widget = gtk_get_event_widget ((GdkEvent *) event);
          gdk_window_get_user_data (window, (gpointer) &dest_widget);

          if (src_widget && dest_widget)
            {
              gint offset_x;
              gint offset_y;

              if (gtk_widget_translate_coordinates (src_widget, dest_widget,
                                                    0, 0,
                                                    &offset_x, &offset_y))
                {
                  coords->x += offset_x;
                  coords->y += offset_y;
                }
            }
        }

      if (gdk_event_get_axis (event, GDK_AXIS_PRESSURE, &coords->pressure))
        {
          coords->pressure = gimp_device_info_map_axis (info,
                                                        GDK_AXIS_PRESSURE,
                                                        coords->pressure);
        }

      if (gdk_event_get_axis (event, GDK_AXIS_XTILT, &coords->xtilt))
        {
          coords->xtilt = gimp_device_info_map_axis (info,
                                                     GDK_AXIS_XTILT,
                                                     coords->xtilt);
        }

      if (gdk_event_get_axis (event, GDK_AXIS_YTILT, &coords->ytilt))
        {
          coords->ytilt = gimp_device_info_map_axis (info,
                                                     GDK_AXIS_YTILT,
                                                     coords->ytilt);
        }

      if (gdk_event_get_axis (event, GDK_AXIS_WHEEL, &coords->wheel))
        {
          coords->wheel = gimp_device_info_map_axis (info,
                                                     GDK_AXIS_WHEEL,
                                                     coords->wheel);
        }

      if (gimp_device_info_get_mode (info)     != GDK_MODE_DISABLED &&
          gdk_device_get_source (info->device) != GDK_SOURCE_MOUSE)
        {
          /* The event was generated by an enabled extended non-mouse device */
          coords->extended = TRUE;
        }
      else
        {
          /* The event was generated by a not extended enabled device */
          coords->extended = FALSE;
        }

      return TRUE;
    }

  gimp_device_info_get_device_coords (info, window, coords);

  return FALSE;
}

#define GIMP_AXIS_HOPEFULLY_LAST 36
/* gtk2 has a bug where gdk_device_get_state may write
   more coordinates than GDK_AXIS_LAST in a caller-provided
   array, resulting in a buffer overflow.

   This happens when the underlying X device supports more axes -- as
   of 2023, the Wacom tablet driver 1.20 supports 8 axes when
   GDK_AXIS_LAST is 7.

   To be safe we over-estimate the maximum number of axes that any
   driver may provide to gtk, to avoid buffer overflows. It would be nicer
   to fix gdk upstream (to never write more than GDK_AXIS_LAST - GDK_AXIS_X
   coordinates), but the proper upstream fix was rejected as gtk2 is EOL:
    https://gitlab.gnome.org/GNOME/gtk/-/merge_requests/6045

   Our constant might in theory become wrong if new device drivers
   start returning much more axes than they do today -- more than 32,
   when the current maximum is 8. Hopefully, by the time this happens,
   users have migrated to gtk3 versions of gimp that use a different
   API and do not suffer from this bug.

   We chose 36 as it is also the internal limit MAX_VALUATORS on the
   number of 'valuators' used by current X server implementations.
*/

void
gimp_device_info_get_device_coords (GimpDeviceInfo *info,
                                    GdkWindow      *window,
                                    GimpCoords     *coords)
{
  gdouble axes[GIMP_AXIS_HOPEFULLY_LAST] = { 0, };

  *coords = default_coords;

  gdk_device_get_state (info->device, window, axes, NULL);

  gdk_device_get_axis (info->device, axes, GDK_AXIS_X, &coords->x);
  gdk_device_get_axis (info->device, axes, GDK_AXIS_Y, &coords->y);

  if (gdk_device_get_axis (info->device,
                           axes, GDK_AXIS_PRESSURE, &coords->pressure))
    {
      coords->pressure = gimp_device_info_map_axis (info,
                                                    GDK_AXIS_PRESSURE,
                                                    coords->pressure);
    }

  if (gdk_device_get_axis (info->device,
                           axes, GDK_AXIS_XTILT, &coords->xtilt))
    {
      coords->xtilt = gimp_device_info_map_axis (info,
                                                 GDK_AXIS_XTILT,
                                                 coords->xtilt);
    }

  if (gdk_device_get_axis (info->device,
                           axes, GDK_AXIS_YTILT, &coords->ytilt))
    {
      coords->ytilt = gimp_device_info_map_axis (info,
                                                 GDK_AXIS_YTILT,
                                                 coords->ytilt);
    }

  if (gdk_device_get_axis (info->device,
                           axes, GDK_AXIS_WHEEL, &coords->wheel))
    {
      coords->wheel = gimp_device_info_map_axis (info,
                                                 GDK_AXIS_WHEEL,
                                                 coords->wheel);
    }

  if (gimp_device_info_get_mode (info)     != GDK_MODE_DISABLED &&
      gdk_device_get_source (info->device) != GDK_SOURCE_MOUSE)
    {
      /* The event was generated by an enabled extended non-mouse device */
      coords->extended = TRUE;
    }
  else
    {
      /* The event was generated by a not extended enabled device */
      coords->extended = FALSE;
    }
}

void
gimp_device_info_get_time_coords (GimpDeviceInfo *info,
                                  GdkTimeCoord   *event,
                                  GimpCoords     *coords)
{
  *coords = default_coords;

  gdk_device_get_axis (info->device, event->axes, GDK_AXIS_X, &coords->x);
  gdk_device_get_axis (info->device, event->axes, GDK_AXIS_Y, &coords->y);

  /*  CLAMP() the return value of each *_get_axis() call to be safe
   *  against buggy XInput drivers.
   */

  if (gdk_device_get_axis (info->device,
                           event->axes, GDK_AXIS_PRESSURE, &coords->pressure))
    {
      coords->pressure = gimp_device_info_map_axis (info,
                                                    GDK_AXIS_PRESSURE,
                                                    coords->pressure);
    }

  if (gdk_device_get_axis (info->device,
                           event->axes, GDK_AXIS_XTILT, &coords->xtilt))
    {
      coords->xtilt = gimp_device_info_map_axis (info,
                                                 GDK_AXIS_XTILT,
                                                 coords->xtilt);
    }

  if (gdk_device_get_axis (info->device,
                           event->axes, GDK_AXIS_YTILT, &coords->ytilt))
    {
      coords->ytilt = gimp_device_info_map_axis (info,
                                                 GDK_AXIS_YTILT,
                                                 coords->ytilt);
    }

  if (gdk_device_get_axis (info->device,
                           event->axes, GDK_AXIS_WHEEL, &coords->wheel))
    {
      coords->wheel = gimp_device_info_map_axis (info,
                                                 GDK_AXIS_WHEEL,
                                                 coords->wheel);
    }

  if (gimp_device_info_get_mode (info)     != GDK_MODE_DISABLED &&
      gdk_device_get_source (info->device) != GDK_SOURCE_MOUSE)
    {
      /* The event was generated by an enabled extended non-mouse device */
      coords->extended = TRUE;
    }
  else
    {
      /* The event was generated by a not extended enabled device */
      coords->extended = FALSE;
    }
}

gboolean
gimp_device_info_get_event_state (GimpDeviceInfo  *info,
                                  GdkWindow       *window,
                                  const GdkEvent  *event,
                                  GdkModifierType *state)
{
  if (gdk_event_get_state (event, state))
    return TRUE;

  gimp_device_info_get_device_state (info, window, state);

  return FALSE;
}

void
gimp_device_info_get_device_state (GimpDeviceInfo  *info,
                                   GdkWindow       *window,
                                   GdkModifierType *state)
{
  gdk_device_get_state (info->device, window, NULL, state);
}
