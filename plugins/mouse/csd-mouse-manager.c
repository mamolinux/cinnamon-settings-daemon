/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street - Suite 500, Boston, MA 02110-1335, USA.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#ifdef __linux
#include <sys/prctl.h>
#endif

#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include <X11/extensions/XInput.h>
#include <X11/extensions/XIproto.h>

#include "cinnamon-settings-profile.h"
#include "csd-mouse-manager.h"
#include "csd-input-helper.h"
#include "csd-enums.h"

#define CSD_MOUSE_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CSD_TYPE_MOUSE_MANAGER, CsdMouseManagerPrivate))

#define SETTINGS_MOUSE_DIR         "org.cinnamon.settings-daemon.peripherals.mouse"
#define SETTINGS_TOUCHPAD_DIR      "org.cinnamon.settings-daemon.peripherals.touchpad"
#define SETTINGS_TRACKBALL_DIR     "org.cinnamon.settings-daemon.peripherals.trackball"

/* Keys for both touchpad and mouse */
#define KEY_LEFT_HANDED         "left-handed"                /* a boolean for mouse, an enum for touchpad */
#define KEY_CUSTOM_ACCELERATION "custom-acceleration"
#define KEY_MOTION_ACCELERATION "motion-acceleration"
#define KEY_CUSTOM_THRESHOLD    "custom-threshold"
#define KEY_MOTION_THRESHOLD    "motion-threshold"

/* Touchpad settings */
#define KEY_TOUCHPAD_DISABLE_W_TYPING    "disable-while-typing"
#define KEY_TAP_TO_CLICK                 "tap-to-click"
#define KEY_CLICKPAD_CLICK               "clickpad-click"

#define KEY_SCROLL_METHOD                "scrolling-method"
#define KEY_HORIZ_SCROLL                 "horizontal-scrolling"

#define KEY_TOUCHPAD_ENABLED             "touchpad-enabled"
#define KEY_NATURAL_SCROLL_ENABLED       "natural-scroll"
#define KEY_TOUCHPAD_DISABLE_WITH_MOUSE  "disable-with-external-mouse"

/* Mouse settings */
#define KEY_LOCATE_POINTER               "locate-pointer"
#define KEY_DWELL_CLICK_ENABLED          "dwell-click-enabled"
#define KEY_SECONDARY_CLICK_ENABLED      "secondary-click-enabled"
#define KEY_MIDDLE_BUTTON_EMULATION      "middle-button-enabled"

/* Trackball settings */
#define KEY_SCROLL_WHEEL_BUTTON          "scroll-wheel-emulation-button"

struct CsdMouseManagerPrivate
{
        guint start_idle_id;
        GSettings *touchpad_settings;
        GSettings *mouse_settings;
        GSettings *mouse_a11y_settings;
        GSettings *trackball_settings;
        GdkDeviceManager *device_manager;
        guint device_added_id;
        guint device_removed_id;
        GHashTable *blacklist;

        gboolean mousetweaks_daemon_running;
        gboolean syndaemon_spawned;
        GPid syndaemon_pid;
        gboolean locate_pointer_spawned;
        GPid locate_pointer_pid;
};

static void     csd_mouse_manager_finalize    (GObject             *object);
static void     set_tap_to_click              (GdkDevice           *device,
                                               gboolean             state,
                                               gboolean             left_handed);
static void     set_click_actions             (GdkDevice           *device,
                                               gint                 clickpad_click,
                                               gboolean             left_handed);
static void     set_natural_scroll            (CsdMouseManager *manager,
                                               GdkDevice       *device,
                                               gboolean         natural_scroll);

G_DEFINE_TYPE (CsdMouseManager, csd_mouse_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

static void
csd_mouse_manager_class_init (CsdMouseManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = csd_mouse_manager_finalize;

        g_type_class_add_private (klass, sizeof (CsdMouseManagerPrivate));
}

static XDevice *
open_gdk_device (GdkDevice *device)
{
        XDevice *xdevice;
        int id;

        g_object_get (G_OBJECT (device), "device-id", &id, NULL);

        gdk_x11_display_error_trap_push (gdk_display_get_default ());

        xdevice = XOpenDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), id);

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()) != 0)
                return NULL;

        return xdevice;
}

static gboolean
gdkdevice_is_touchpad (GdkDevice *device)
{
        XDevice *xdevice;
        gboolean ret = FALSE;

        xdevice = open_gdk_device (device);

        if (xdevice == NULL)
                return ret;

        ret = device_is_touchpad (xdevice);

        xdevice_close (xdevice);

        return ret;
}

static gboolean
device_is_trackball (GdkDevice *device)
{
        XDeviceInfo *device_info;
        gboolean retval = FALSE;
        gint n_devices;
        guint i;
        int id;

        g_object_get (G_OBJECT (device), "device-id", &id, NULL);

        gdk_error_trap_push ();

        device_info = XListInputDevices (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), &n_devices);
        if (device_info == NULL)
                return retval;

        for (i = 0; i < n_devices; i++) {
                if (device_info[i].id != id)
                        continue;

                retval = device_info_is_trackball (&device_info[i]);
                break;
        }
        XFreeDeviceList (device_info);

        if (gdk_error_trap_pop () != 0)
                return FALSE;

        return retval;
}

static gboolean
device_is_blacklisted (CsdMouseManager *manager,
                       GdkDevice       *device)
{
        int id;
        g_object_get (G_OBJECT (device), "device-id", &id, NULL);
        if (g_hash_table_lookup (manager->priv->blacklist, GINT_TO_POINTER (id)) != NULL) {
                g_debug ("device %s (%d) is blacklisted", gdk_device_get_name (device), id);
                return TRUE;
        }
        return FALSE;
}

static gboolean
device_is_ignored (CsdMouseManager *manager,
		   GdkDevice       *device)
{
	GdkInputSource source;
	const char *name;

	if (device_is_blacklisted (manager, device))
		return TRUE;

	source = gdk_device_get_source (device);
	if (source != GDK_SOURCE_MOUSE &&
	    source != GDK_SOURCE_TOUCHPAD &&
	    source != GDK_SOURCE_CURSOR)
		return TRUE;

	name = gdk_device_get_name (device);
	if (name != NULL && g_str_equal ("Virtual core XTEST pointer", name))
		return TRUE;

	return FALSE;
}

static void
configure_button_layout (guchar   *buttons,
                         gint      n_buttons,
                         gboolean  left_handed)
{
        const gint left_button = 1;
        gint right_button;
        gint i;

        /* if the button is higher than 2 (3rd button) then it's
         * probably one direction of a scroll wheel or something else
         * uninteresting
         */
        right_button = MIN (n_buttons, 3);

        /* If we change things we need to make sure we only swap buttons.
         * If we end up with multiple physical buttons assigned to the same
         * logical button the server will complain. This code assumes physical
         * button 0 is the physical left mouse button, and that the physical
         * button other than 0 currently assigned left_button or right_button
         * is the physical right mouse button.
         */

        /* check if the current mapping satisfies the above assumptions */
        if (buttons[left_button - 1] != left_button &&
            buttons[left_button - 1] != right_button)
                /* The current mapping is weird. Swapping buttons is probably not a
                 * good idea.
                 */
                return;

        /* check if we are left_handed and currently not swapped */
        if (left_handed && buttons[left_button - 1] == left_button) {
                /* find the right button */
                for (i = 0; i < n_buttons; i++) {
                        if (buttons[i] == right_button) {
                                buttons[i] = left_button;
                                break;
                        }
                }
                /* swap the buttons */
                buttons[left_button - 1] = right_button;
        }
        /* check if we are not left_handed but are swapped */
        else if (!left_handed && buttons[left_button - 1] == right_button) {
                /* find the right button */
                for (i = 0; i < n_buttons; i++) {
                        if (buttons[i] == left_button) {
                                buttons[i] = right_button;
                                break;
                        }
                }
                /* swap the buttons */
                buttons[left_button - 1] = left_button;
        }
}

static gboolean
xinput_device_has_buttons (GdkDevice *device)
{
        int i;
        XAnyClassInfo *class_info;

        /* FIXME can we use the XDevice's classes here instead? */
        XDeviceInfo *device_info, *info;
        gint n_devices;
        int id;

        /* Find the XDeviceInfo for the GdkDevice */
        g_object_get (G_OBJECT (device), "device-id", &id, NULL);

        device_info = XListInputDevices (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), &n_devices);
        if (device_info == NULL)
                return FALSE;

        info = NULL;
        for (i = 0; i < n_devices; i++) {
                if (device_info[i].id == id) {
                        info = &device_info[i];
                        break;
                }
        }
        if (info == NULL)
                goto bail;

        class_info = info->inputclassinfo;
        for (i = 0; i < info->num_classes; i++) {
                if (class_info->class == ButtonClass) {
                        XButtonInfo *button_info;

                        button_info = (XButtonInfo *) class_info;
                        if (button_info->num_buttons > 0) {
                                XFreeDeviceList (device_info);
                                return TRUE;
                        }
                }

                class_info = (XAnyClassInfo *) (((guchar *) class_info) +
                                                class_info->length);
        }

bail:
        XFreeDeviceList (device_info);

        return FALSE;
}

static Atom
property_from_name (const char *property_name)
{
        Atom prop;

        prop = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), property_name, True);

        return prop;
}

static gboolean
touchpad_has_single_button (XDevice *device)
{
        Atom type, prop;
        int format;
        unsigned long nitems, bytes_after;
        unsigned char *data;
        gboolean is_single_button = FALSE;
        int rc;

        prop = property_from_name ("Synaptics Capabilities");
        if (!prop)
                return FALSE;

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), device, prop, 0, 1, False,
                                XA_INTEGER, &type, &format, &nitems,
                                &bytes_after, &data);
        if (rc == Success && type == XA_INTEGER && format == 8 && nitems >= 3)
                is_single_button = (data[0] == 1 && data[1] == 0 && data[2] == 0);

        if (rc == Success)
                XFree (data);

        gdk_x11_display_error_trap_pop_ignored (gdk_display_get_default ());

        return is_single_button;
}

static gboolean
property_exists_on_device (GdkDevice *device, const char * property_name)
{
        int rc;
        Atom act_type, property;
        int act_format;
        unsigned long nitems, bytes_after;
        unsigned char *data;
        XDevice *xdevice;

        property = property_from_name (property_name);
        if (!property)
                return FALSE;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return FALSE;

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                 property, 0, 1, False,
                                 XA_INTEGER, &act_type, &act_format, &nitems,
                                 &bytes_after, &data);
        if (rc == Success)
            XFree(data);

        gdk_x11_display_error_trap_pop_ignored (gdk_display_get_default ());

        xdevice_close (xdevice);
        return rc == Success;
}

static void
property_set_bool (GdkDevice *device,
                   XDevice *xdevice,
                   const char * property_name,
                   int property_index,
                   gboolean enable)
{
        int rc;
        Atom act_type, property;
        int act_format;
        unsigned long nitems, bytes_after;
        unsigned char *data;

        property = property_from_name (property_name);
        if (!property) {
                return;
        }

        int value = 0;
        if (enable) {
                value = 1;
        }

        g_debug ("Setting %s on %s to %d", property_name, gdk_device_get_name (device), value);

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                 property, 0, 1, False,
                                 XA_INTEGER, &act_type, &act_format, &nitems,
                                 &bytes_after, &data);
        if (rc == Success && act_type == XA_INTEGER && act_format == 8 && nitems > property_index) {
                data[property_index] = value;
                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                       property, XA_INTEGER, 8,
                                       PropModeReplace, data, nitems);
        }

        if (rc == Success) {
                XFree (data);
        }

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ())) {
                g_warning ("Error while setting %s on \"%s\"", property_name, gdk_device_get_name (device));
        }
}

static gboolean
property_get_bool (GdkDevice *device,
                   XDevice *xdevice,
                   const char * property_name,
                   int property_index)
{
        int rc;
        Atom act_type, property;
        int act_format;
        unsigned long nitems, bytes_after;
        unsigned char *data;

        gboolean ret = FALSE;

        property = property_from_name (property_name);
        if (!property) {
                return FALSE;
        }

        g_debug ("Getting %s on %s", property_name, gdk_device_get_name (device));

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                 property, 0, 1, False,
                                 XA_INTEGER, &act_type, &act_format, &nitems,
                                 &bytes_after, &data);
        if (rc == Success && act_type == XA_INTEGER && act_format == 8 && nitems > property_index) {
                ret = data[property_index];
        }

        if (rc == Success) {
                XFree (data);
        }

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ())) {
                g_warning ("Error while getting %s on \"%s\"", property_name, gdk_device_get_name (device));
        }

        return ret;
}

static void
touchpad_set_bool (GdkDevice *device, const char * property_name, int property_index, gboolean enable)
{
        XDevice *xdevice;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (device_is_touchpad (xdevice))
                property_set_bool (device, xdevice, property_name, property_index, enable);

        xdevice_close (xdevice);
}

static gboolean
touchpad_get_bool (GdkDevice *device, const char * property_name, int property_index)
{
        XDevice *xdevice;
        gboolean ret = FALSE;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return ret;

        if (device_is_touchpad (xdevice))
                ret = property_get_bool (device, xdevice, property_name, property_index);

        xdevice_close (xdevice);

        return ret;
}

static void
set_left_handed_legacy_driver (CsdMouseManager *manager,
                               GdkDevice       *device,
                               gboolean mouse_left_handed,
                               gboolean touchpad_left_handed)
{
        XDevice *xdevice;
        guchar *buttons;
        gsize buttons_capacity = 16;
        gboolean left_handed;
        gint n_buttons;

        if (!xinput_device_has_buttons (device))
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        g_debug ("setting handedness on %s", gdk_device_get_name (device));

        buttons = g_new (guchar, buttons_capacity);

        /* If the device is a touchpad, swap tap buttons
         * around too, otherwise a tap would be a right-click */
        if (device_is_touchpad (xdevice)) {
                gboolean tap = g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TAP_TO_CLICK);
                gboolean single_button = touchpad_has_single_button (xdevice);

                left_handed = touchpad_left_handed;

                if (tap && !single_button)
                        set_tap_to_click (device, tap, left_handed);

                if (single_button)
                        goto out;
        } else {
                left_handed = mouse_left_handed;
        }

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        n_buttons = XGetDeviceButtonMapping (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                             buttons,
                                             buttons_capacity);

        while (n_buttons > buttons_capacity) {
                buttons_capacity = n_buttons;
                buttons = (guchar *) g_realloc (buttons,
                                                buttons_capacity * sizeof (guchar));

                n_buttons = XGetDeviceButtonMapping (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                                     buttons,
                                                     buttons_capacity);
        }

        configure_button_layout (buttons, n_buttons, left_handed);

        XSetDeviceButtonMapping (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, buttons, n_buttons);
        gdk_x11_display_error_trap_pop_ignored (gdk_display_get_default ());

out:
        xdevice_close (xdevice);
        g_free (buttons);
}

static void
set_left_handed_libinput (GdkDevice       *device,
                          gboolean mouse_left_handed,
                          gboolean touchpad_left_handed)
{
        XDevice *xdevice;
        gboolean want_lefthanded;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        want_lefthanded = device_is_touchpad (xdevice) ? touchpad_left_handed : mouse_left_handed;

        property_set_bool (device, xdevice, "libinput Left Handed Enabled", 0, want_lefthanded);

        xdevice_close (xdevice);
}

static void
set_left_handed (CsdMouseManager *manager,
                 GdkDevice       *device,
                 gboolean mouse_left_handed,
                 gboolean touchpad_left_handed)
{
        if (property_exists_on_device (device, "libinput Left Handed Enabled"))
                set_left_handed_libinput (device, mouse_left_handed, touchpad_left_handed);
        else
                set_left_handed_legacy_driver (manager, device, mouse_left_handed, touchpad_left_handed);
}

static void
set_motion_legacy_driver (CsdMouseManager *manager,
                          GdkDevice       *device)
{
        XDevice *xdevice;
        XPtrFeedbackControl feedback;
        XFeedbackState *states, *state;
        int num_feedbacks;
        int numerator, denominator;
        gfloat motion_acceleration;
        gboolean custom_acceleration;
        int motion_threshold;
        gboolean custom_threshold;
        GSettings *settings;
        guint i;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        g_debug ("setting motion on %s", gdk_device_get_name (device));

        if (device_is_touchpad (xdevice))
                settings = manager->priv->touchpad_settings;
        else
                settings = manager->priv->mouse_settings;

        /* Calculate acceleration */
        motion_acceleration = g_settings_get_double (settings, KEY_MOTION_ACCELERATION);
        custom_acceleration = g_settings_get_boolean (settings, KEY_CUSTOM_ACCELERATION);

        if (custom_acceleration) {
                if (motion_acceleration >= 1.0) {
                        /* we want to get the acceleration, with a resolution of 0.5
                         */
                        if ((motion_acceleration - floor (motion_acceleration)) < 0.25) {
                                numerator = floor (motion_acceleration);
                                denominator = 1;
                        } else if ((motion_acceleration - floor (motion_acceleration)) < 0.5) {
                                numerator = ceil (2.0 * motion_acceleration);
                                denominator = 2;
                        } else if ((motion_acceleration - floor (motion_acceleration)) < 0.75) {
                                numerator = floor (2.0 *motion_acceleration);
                                denominator = 2;
                        } else {
                                numerator = ceil (motion_acceleration);
                                denominator = 1;
                        }
                } else if (motion_acceleration < 1.0 && motion_acceleration > 0) {
                        /* This we do to 1/10ths */
                        numerator = floor (motion_acceleration * 10) + 1;
                        denominator= 10;
                } else {
                        numerator = -1;
                        denominator = -1;
                }
        } else {
                numerator = -1;
                denominator = -1;
        }

        /* And threshold */
        custom_threshold = g_settings_get_boolean (settings, KEY_CUSTOM_THRESHOLD);

        if (custom_threshold) {
                motion_threshold = g_settings_get_int (settings, KEY_MOTION_THRESHOLD);
        } else {
                motion_threshold = -1;
        }

        gdk_x11_display_error_trap_push (gdk_display_get_default ());

        /* Get the list of feedbacks for the device */
        states = XGetFeedbackControl (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, &num_feedbacks);
        if (states == NULL)
                goto out;
        state = (XFeedbackState *) states;
        for (i = 0; i < num_feedbacks; i++) {
                if (state->class == PtrFeedbackClass) {
                        /* And tell the device */
                        feedback.class      = PtrFeedbackClass;
                        feedback.length     = sizeof (XPtrFeedbackControl);
                        feedback.id         = state->id;
                        feedback.threshold  = motion_threshold;
                        feedback.accelNum   = numerator;
                        feedback.accelDenom = denominator;

                        g_debug ("Setting accel %d/%d, threshold %d for device '%s'",
                                 numerator, denominator, motion_threshold, gdk_device_get_name (device));

                        XChangeFeedbackControl (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                                xdevice,
                                                DvAccelNum | DvAccelDenom | DvThreshold,
                                                (XFeedbackControl *) &feedback);

                        break;
                }
                state = (XFeedbackState *) ((char *) state + state->length);
        }

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error setting acceleration on \"%s\"",
                           gdk_device_get_name (device));

        XFreeFeedbackList (states);

    out:

        xdevice_close (xdevice);
}

static void
set_motion_libinput (CsdMouseManager *manager,
                     GdkDevice       *device)
{
        int rc;
        XDevice *xdevice;
        GSettings *settings;
        Atom act_type, property, float_type;
        int act_format;
        unsigned long nitems, bytes_after;
        union {
                unsigned char *c;
                long *l;
        } data;
        gfloat accel;
        gfloat motion_acceleration;
        gboolean custom_acceleration;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        g_debug ("setting motion on %s", gdk_device_get_name (device));

        if (device_is_touchpad (xdevice))
                settings = manager->priv->touchpad_settings;
        else
                settings = manager->priv->mouse_settings;


        /* Calculate acceleration */
        motion_acceleration = g_settings_get_double (settings, KEY_MOTION_ACCELERATION);
        custom_acceleration = g_settings_get_boolean (settings, KEY_CUSTOM_ACCELERATION);

        /* panel gives us a range of 1.0-10.0, map to libinput's -1, 1

           oldrange = (oldmax - oldmin)
           newrange = (newmax - newmin)

           mapped = (value - oldmin) * newrange / oldrange + oldmin
         */

        if (motion_acceleration == -1.0 || !custom_acceleration) /* unset */
                accel = 0.0;
        else
                accel = (motion_acceleration - 1.0) * 2.0 / 9.0 - 1;


        float_type = property_from_name ("FLOAT");
        if (!float_type)
                return;

        property = property_from_name ("libinput Accel Speed");
        if (!property) {
                return;
        }

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                 property, 0, 1, False,
                                 float_type, &act_type, &act_format, &nitems,
                                 &bytes_after, &data.c);
        if (rc == Success && act_type == float_type && act_format == 32 && nitems >= 1) {
                *(float*)data.l = accel;
                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                       property, float_type, 32,
                                       PropModeReplace, data.c, nitems);
        }

        if (rc == Success) {
                XFree (data.c);
        }

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ())) {
                g_warning ("Error while setting accel speed on \"%s\"", gdk_device_get_name (device));
        }

        xdevice_close (xdevice);
}

static void
set_motion (CsdMouseManager *manager,
            GdkDevice       *device)
{
        if (property_exists_on_device (device, "libinput Accel Speed"))
                set_motion_libinput (manager, device);
        else
                set_motion_legacy_driver (manager, device);
}

static void
set_middle_button_evdev (CsdMouseManager *manager,
                         GdkDevice       *device,
                         gboolean         middle_button)
{
        Atom prop;
        XDevice *xdevice;
        Atom type;
        int format;
        unsigned long nitems, bytes_after;
        unsigned char *data;
        int rc;

        prop = property_from_name ("Evdev Middle Button Emulation");

        if (!prop) /* no evdev devices */
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        g_debug ("setting middle button on %s", gdk_device_get_name (device));

        gdk_x11_display_error_trap_push (gdk_display_get_default ());

        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                            xdevice, prop, 0, 1, False, XA_INTEGER, &type, &format,
                            &nitems, &bytes_after, &data);

        if (rc == Success && format == 8 && type == XA_INTEGER && nitems == 1) {
                data[0] = middle_button ? 1 : 0;

                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                       xdevice, prop, type, format, PropModeReplace, data, nitems);
        }

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error in setting middle button emulation on \"%s\"", gdk_device_get_name (device));

        if (rc == Success)
                XFree (data);

        xdevice_close (xdevice);
}

static void
set_middle_button_libinput (CsdMouseManager *manager,
                            GdkDevice       *device,
                            gboolean         middle_button)
{
        XDevice *xdevice;

        g_debug ("setting middle button on %s", gdk_device_get_name (device));

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        /* we didn't set it for synaptics, so bail out for touchpads */
        if (device_is_touchpad (xdevice))
                return;

        property_set_bool (device, xdevice, "libinput Middle Emulation Enabled",
                           0, middle_button);

        xdevice_close (xdevice);
}

static void
set_middle_button (CsdMouseManager *manager,
                   GdkDevice       *device,
                   gboolean         middle_button)
{
        if (property_from_name ("Evdev Middle Button Emulation"))
                set_middle_button_evdev (manager, device, middle_button);

        if (property_from_name ("libinput Middle Emulation Enabled"))
                set_middle_button_libinput (manager, device, middle_button);
}

/* Ensure that syndaemon dies together with us, to avoid running several of
 * them */
static void
setup_syndaemon (gpointer user_data)
{
#ifdef __linux
        prctl (PR_SET_PDEATHSIG, SIGHUP);
#endif
}

static gboolean
have_program_in_path (const char *name)
{
        gchar *path;
        gboolean result;

        path = g_find_program_in_path (name);
        result = (path != NULL);
        g_free (path);
        return result;
}

static void
syndaemon_died (GPid pid, gint status, gpointer user_data)
{
        CsdMouseManager *manager = CSD_MOUSE_MANAGER (user_data);

        g_debug ("syndaemon stopped with status %i", status);
        g_spawn_close_pid (pid);
        manager->priv->syndaemon_spawned = FALSE;
}

static int
set_disable_w_typing_synaptics (CsdMouseManager *manager, gboolean state)
{
        if (state && touchpad_is_present ()) {
                GError *error = NULL;
                GPtrArray *args;

                if (manager->priv->syndaemon_spawned)
                        return 0;

                if (!have_program_in_path ("syndaemon"))
                        return 0;

                args = g_ptr_array_new ();

                g_ptr_array_add (args, "syndaemon");
                g_ptr_array_add (args, "-i");
                g_ptr_array_add (args, "1.0");
                g_ptr_array_add (args, "-t");
                g_ptr_array_add (args, "-K");
                g_ptr_array_add (args, "-R");
                g_ptr_array_add (args, NULL);

                /* we must use G_SPAWN_DO_NOT_REAP_CHILD to avoid
                 * double-forking, otherwise syndaemon will immediately get
                 * killed again through (PR_SET_PDEATHSIG when the intermediate
                 * process dies */
                g_spawn_async (g_get_home_dir (), (char **) args->pdata, NULL,
                                G_SPAWN_SEARCH_PATH|G_SPAWN_DO_NOT_REAP_CHILD, setup_syndaemon, NULL,
                                &manager->priv->syndaemon_pid, &error);

                manager->priv->syndaemon_spawned = (error == NULL);
                g_ptr_array_free (args, FALSE);

                if (error) {
                        g_warning ("Failed to launch syndaemon: %s", error->message);
                        g_settings_set_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_DISABLE_W_TYPING, FALSE);
                        g_error_free (error);
                } else {
                        g_child_watch_add (manager->priv->syndaemon_pid, syndaemon_died, manager);
                        g_debug ("Launched syndaemon");
                }
        } else if (manager->priv->syndaemon_spawned) {
                kill (manager->priv->syndaemon_pid, SIGHUP);
                g_spawn_close_pid (manager->priv->syndaemon_pid);
                manager->priv->syndaemon_spawned = FALSE;
                g_debug ("Killed syndaemon");
        }

        return 0;
}

static int
set_disable_w_typing_libinput (CsdMouseManager *manager, gboolean state)
{
        GList *devices, *l;

        /* This is only called once for synaptics but for libinput we need
         * to loop through the list of devices
         */
        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);

        for (l = devices; l != NULL; l = l->next) {
                GdkDevice *device = l->data;

                if (device_is_ignored (manager, device))
                        continue;

                touchpad_set_bool (device, "libinput Disable While Typing Enabled", 0, state);
        }
        g_list_free (devices);

        return 0;
}

static int
set_disable_w_typing (CsdMouseManager *manager, gboolean state)
{
        if (property_from_name ("Synaptics Off"))
                set_disable_w_typing_synaptics (manager, state);

        if (property_from_name ("libinput Disable While Typing Enabled"))
                set_disable_w_typing_libinput (manager, state);

        return 0;
}

static int
set_disable_w_mouse_attached_libinput (CsdMouseManager *manager, gboolean state)
{
        GList *devices, *l;

        /* This is only called once for synaptics but for libinput we need
         * to loop through the list of devices
         */
        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);

        for (l = devices; l != NULL; l = l->next) {
                GdkDevice *device = l->data;

                if (device_is_ignored (manager, device))
                        continue;

                if (touchpad_get_bool (device, "libinput Send Events Modes Available", 1)) {
                    touchpad_set_bool (device, "libinput Send Events Mode Enabled", 1, state);
                }

        }
        g_list_free (devices);

        return 0;
}

static void
set_disable_w_mouse_attached (CsdMouseManager *manager, gboolean state)
{

        set_disable_w_mouse_attached_libinput (manager, state);
}

static void
set_tap_to_click_synaptics (GdkDevice *device,
                            gboolean   state,
                            gboolean   left_handed)
{
        int format, rc;
        unsigned long nitems, bytes_after;
        XDevice *xdevice;
        unsigned char* data;
        Atom prop, type;

        prop = property_from_name ("Synaptics Tap Action");
        if (!prop)
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        g_debug ("setting tap to click on %s", gdk_device_get_name (device));

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop, 0, 2,
                                 False, XA_INTEGER, &type, &format, &nitems,
                                 &bytes_after, &data);

        if (rc == Success && type == XA_INTEGER && format == 8 && nitems >= 7) {
                /* Set MR mapping for corner tapping on the right side*/
                data[0] = (state) ? 2 : 0;
                data[1] = (state) ? 3 : 0;

                /* Set RLM mapping for 1/2/3 fingers*/
                data[4] = (state) ? ((left_handed) ? 3 : 1) : 0;
                data[5] = (state) ? ((left_handed) ? 1 : 3) : 0;
                data[6] = (state) ? 2 : 0;
                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop, XA_INTEGER, 8,
                                       PropModeReplace, data, nitems);
        }

        if (rc == Success)
                XFree (data);

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error in setting tap to click on \"%s\"", gdk_device_get_name (device));

        xdevice_close (xdevice);
}

static void
set_tap_to_click_libinput (GdkDevice *device, gboolean state)
{
        g_debug ("setting tap to click on %s", gdk_device_get_name (device));

        touchpad_set_bool (device, "libinput Tapping Enabled", 0, state);
}

static void
set_tap_to_click (GdkDevice *device,
                  gboolean   state,
                  gboolean   left_handed)
{
        if (property_from_name ("Synaptics Tap Action"))
                set_tap_to_click_synaptics (device, state, left_handed);

        if (property_from_name ("libinput Tapping Enabled"))
                set_tap_to_click_libinput (device, state);
}

static void
set_click_actions_synaptics (GdkDevice *device,
                             gint       clickpad_click,
                             gboolean   left_handed)

{
        int format, rc;
        unsigned long nitems, bytes_after;
        XDevice *xdevice;
        unsigned char* data;
        Atom prop, type;
        gboolean state;

        prop = property_from_name ("Synaptics Click Action");
        if (!prop)
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        g_debug ("setting click action to click on %s", gdk_device_get_name (device));

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop, 0, 2,
                                 False, XA_INTEGER, &type, &format, &nitems,
                                 &bytes_after, &data);

        state = (clickpad_click == 2) || (clickpad_click == 3);
        if (rc == Success && type == XA_INTEGER && format == 8 && nitems >= 3) {
            data[0] = 1;
            data[1] = (state)? ((left_handed) ? 1 : 3) : 0;
            data[2] = (state)? 2 : 0;
            XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                   prop, XA_INTEGER, 8, PropModeReplace, data, nitems);
        }

        if (rc == Success)
                XFree (data);

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error in setting click actions on \"%s\"", gdk_device_get_name (device));

        xdevice_close (xdevice);
}

static void
set_click_actions_libinput (GdkDevice *device,
                            gint   clickpad_click)
{
        int format, rc, rc_default;
        unsigned long nitems, bytes_after;
        XDevice *xdevice;
        unsigned char* data, * data_default;
        Atom prop, prop_default, type;

        prop = property_from_name ("libinput Click Method Enabled");
        if (!prop)
                return;
        prop_default = property_from_name("libinput Click Method Enabled Default");
        if (!prop_default && clickpad_click == 3)
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        g_debug ("setting click action to click on %s", gdk_device_get_name (device));

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop, 0, 2,
                                 False, XA_INTEGER, &type, &format, &nitems,
                                 &bytes_after, &data);
        if (clickpad_click == 3) {
                rc_default = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop_default, 0, 2,
                                                False, XA_INTEGER, &type, &format, &nitems,
                                                &bytes_after, &data_default);
        }

        if (rc == Success && type == XA_INTEGER && format == 8 && nitems >= 2
                        && (rc_default == Success || clickpad_click != 3)) {
                data[0] = (clickpad_click == 1) || (clickpad_click == 3 && data_default[0]);
                data[1] = (clickpad_click == 2) || (clickpad_click == 3 && data_default[1]);
                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop, XA_INTEGER, 8, PropModeReplace, data, nitems);
        }

        if (rc == Success)
                XFree (data);
        if (clickpad_click == 3 && rc_default == Success)
                XFree (data_default);

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error in setting click actions on \"%s\"", gdk_device_get_name (device));

        xdevice_close (xdevice);
}

static void
set_click_actions (GdkDevice *device,
                   gint       clickpad_click,
                   gboolean   left_handed)
{
        if (property_from_name ("Synaptics Click Action"))
                set_click_actions_synaptics (device, clickpad_click, left_handed);

        if (property_from_name ("libinput Click Method Enabled"))
                set_click_actions_libinput (device, clickpad_click);
}

static void
set_scrolling_synaptics (GdkDevice *device, gint scrolling_method, gboolean horizontal_scroll)
{
        gboolean want_edge, want_2fg;
        want_2fg = (scrolling_method == 1 || scrolling_method == 3);
        want_edge = (scrolling_method == 2 || scrolling_method == 3);
        touchpad_set_bool (device, "Synaptics Edge Scrolling", 0, want_edge);
        touchpad_set_bool (device, "Synaptics Edge Scrolling", 1, want_edge && horizontal_scroll);
        touchpad_set_bool (device, "Synaptics Two-Finger Scrolling", 0, want_2fg);
        touchpad_set_bool (device, "Synaptics Two-Finger Scrolling", 1, want_2fg && horizontal_scroll);
}

static void
set_scrolling_libinput (GdkDevice *device, gint scrolling_method, gboolean horizontal_scroll)
{
        int format, rc, rc_default;
        unsigned long nitems, bytes_after;
        XDevice *xdevice;
        unsigned char* data, * data_default;
        Atom prop, prop_default, type;

        prop = property_from_name ("libinput Scroll Method Enabled");
        if (!prop)
                return;
        prop_default = property_from_name("libinput Scroll Method Enabled Default");
        if (!prop_default && scrolling_method == 3)
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        g_debug ("setting scroll method on %s", gdk_device_get_name (device));

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop, 0, 2,
                                 False, XA_INTEGER, &type, &format, &nitems,
                                 &bytes_after, &data);

        if (scrolling_method == 3) {
                rc_default = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice, prop_default, 0, 2,
                                                False, XA_INTEGER, &type, &format, &nitems,
                                                &bytes_after, &data_default);
        }

        if (rc == Success && type == XA_INTEGER && format == 8 && nitems >= 3
                        && (rc_default == Success || scrolling_method != 3)) {
                data[0] = (scrolling_method == 1) || (scrolling_method == 3 && data_default[0]);
                data[1] = (scrolling_method == 2) || (scrolling_method == 3 && data_default[1]);
                data[2] = 0 || (scrolling_method == 3 && data_default[2]);
                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                       prop, XA_INTEGER, 8, PropModeReplace, data, nitems);
        }

        if (rc == Success)
                XFree (data);
        if (scrolling_method == 3 && rc_default == Success)
                XFree (data_default);

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error in setting scroll method on \"%s\"", gdk_device_get_name (device));

        xdevice_close (xdevice);

        /* there are versions of libinput around with an undocumented missing T in Horizontal */
        if (property_from_name ("libinput Horizonal Scroll Enabled"))
                touchpad_set_bool (device, "libinput Horizonal Scroll Enabled", 0, horizontal_scroll);
        else
                touchpad_set_bool (device, "libinput Horizontal Scroll Enabled", 0, horizontal_scroll);
}

static void
set_scrolling (GdkDevice *device, gint scrolling_method, gboolean horizontal_scroll)
{
        if (property_from_name ("Synaptics Edge Scrolling"))
                set_scrolling_synaptics (device, scrolling_method, horizontal_scroll);

        if (property_from_name ("libinput Scroll Method Enabled"))
                set_scrolling_libinput (device, scrolling_method, horizontal_scroll);
}

static void
set_touchpad_disabled (GdkDevice *device)
{
        int id;
        XDevice *xdevice;

        g_object_get (G_OBJECT (device), "device-id", &id, NULL);

        g_debug ("Trying to set device disabled for \"%s\" (%d)", gdk_device_get_name (device), id);

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        if (set_device_enabled (id, FALSE) == FALSE)
                g_warning ("Error disabling device \"%s\" (%d)", gdk_device_get_name (device), id);
        else
                g_debug ("Disabled device \"%s\" (%d)", gdk_device_get_name (device), id);

        xdevice_close (xdevice);
}

static void
set_touchpad_enabled (int id)
{
        XDevice *xdevice;

        g_debug ("Trying to set device enabled for %d", id);

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        xdevice = XOpenDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), id);
        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()) != 0)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        if (set_device_enabled (id, TRUE) == FALSE)
                g_warning ("Error enabling device \"%d\"", id);
        else
                g_debug ("Enabled device %d", id);

        xdevice_close (xdevice);
}

static void
set_locate_pointer (CsdMouseManager *manager,
                    gboolean         state)
{
        if (state) {
                GError *error = NULL;
                char *args[2];

                if (manager->priv->locate_pointer_spawned)
                        return;

                args[0] = LIBEXECDIR "/csd-locate-pointer";
                args[1] = NULL;

                g_spawn_async (NULL, args, NULL,
                               0, NULL, NULL,
                               &manager->priv->locate_pointer_pid, &error);

                manager->priv->locate_pointer_spawned = (error == NULL);

                if (error) {
                        g_settings_set_boolean (manager->priv->mouse_settings, KEY_LOCATE_POINTER, FALSE);
                        g_error_free (error);
                }

        } else if (manager->priv->locate_pointer_spawned) {
                kill (manager->priv->locate_pointer_pid, SIGHUP);
                g_spawn_close_pid (manager->priv->locate_pointer_pid);
                manager->priv->locate_pointer_spawned = FALSE;
        }
}

static void
set_mousetweaks_daemon (CsdMouseManager *manager,
                        gboolean         dwell_click_enabled,
                        gboolean         secondary_click_enabled)
{
        GError *error = NULL;
        gchar *comm;
        gboolean run_daemon = dwell_click_enabled || secondary_click_enabled;

        if (run_daemon || manager->priv->mousetweaks_daemon_running)
                comm = g_strdup_printf ("mousetweaks %s",
                                        run_daemon ? "" : "-s");
        else
                return;

        if (run_daemon)
                manager->priv->mousetweaks_daemon_running = TRUE;

        if (! g_spawn_command_line_async (comm, &error)) {
                if (error->code == G_SPAWN_ERROR_NOENT && run_daemon) {
                        if (dwell_click_enabled) {
                                g_settings_set_boolean (manager->priv->mouse_a11y_settings,
                                                        KEY_DWELL_CLICK_ENABLED, FALSE);
                        } else if (secondary_click_enabled) {
                                g_settings_set_boolean (manager->priv->mouse_a11y_settings,
                                                        KEY_SECONDARY_CLICK_ENABLED, FALSE);
                        }
                        g_warning("Error enabling mouse accessibility features (mousetweaks is not installed)");
                }
                g_error_free (error);
        }
        g_free (comm);
}

static gboolean
get_touchpad_handedness (CsdMouseManager *manager, gboolean mouse_left_handed)
{
        switch (g_settings_get_enum (manager->priv->touchpad_settings, KEY_LEFT_HANDED)) {
        case CSD_TOUCHPAD_HANDEDNESS_RIGHT:
                return FALSE;
        case CSD_TOUCHPAD_HANDEDNESS_LEFT:
                return TRUE;
        case CSD_TOUCHPAD_HANDEDNESS_MOUSE:
                return mouse_left_handed;
        default:
                g_assert_not_reached ();
        }
}

static void
set_scroll_wheel_button (CsdMouseManager *manager,
                         GdkDevice       *device)
{
        Atom wheel_prop, button_prop;
        XDevice *xdevice;
        Atom type;
        int format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        int button;
        int rc;

        if (!device_is_trackball (device))
                return;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        wheel_prop = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                  "Evdev Wheel Emulation", True);
        button_prop = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                   "Evdev Wheel Emulation Button", True);

        if (!wheel_prop || !button_prop) {
                xdevice_close (xdevice);
                return;
        }

        g_debug ("setting scroll wheel emulation on %s", gdk_device_get_name (device));

        gdk_error_trap_push ();

        button = g_settings_get_int (manager->priv->trackball_settings, KEY_SCROLL_WHEEL_BUTTON);

        /* Whether scroll wheel emulation is enabled */
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                 xdevice, wheel_prop, 0, 1, False, XA_INTEGER, &type, &format,
                                 &nitems, &bytes_after, &data);

        if (rc == Success && format == 8 && type == XA_INTEGER && nitems == 1) {
                data[0] = button > 0 ? 1 : 0;

                gdk_error_trap_push ();
                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                       xdevice, wheel_prop, type, format, PropModeReplace, data, nitems);
        }

        if (data) {
                XFree (data);
                data = NULL;
        }

        /* Which button is used for the emulation */
        if (button > 0) {
                rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                         xdevice, button_prop, 0, 1, False, XA_INTEGER, &type, &format,
                                         &nitems, &bytes_after, &data);

                if (rc == Success && format == 8 && type == XA_INTEGER && nitems == 1) {
                        data[0] = button;

                        XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                               xdevice, button_prop, type, format, PropModeReplace, data, nitems);
                }

                if (data)
                        XFree (data);
        }

        if (gdk_error_trap_pop ())
                g_warning ("Error in setting scroll wheel emulation on \"%s\"", gdk_device_get_name (device));

        xdevice_close (xdevice);
}

static void
set_mouse_settings (CsdMouseManager *manager,
                    GdkDevice       *device)
{
        gboolean mouse_left_handed, touchpad_left_handed;

        mouse_left_handed = g_settings_get_boolean (manager->priv->mouse_settings, KEY_LEFT_HANDED);
        touchpad_left_handed = get_touchpad_handedness (manager, mouse_left_handed);
        set_left_handed (manager, device, mouse_left_handed, touchpad_left_handed);

        set_motion (manager, device);
        set_middle_button (manager, device, g_settings_get_boolean (manager->priv->mouse_settings, KEY_MIDDLE_BUTTON_EMULATION));

        set_tap_to_click (device, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TAP_TO_CLICK), touchpad_left_handed);
        set_click_actions( device, g_settings_get_int (manager->priv->touchpad_settings, KEY_CLICKPAD_CLICK), touchpad_left_handed);
        set_scrolling (device, g_settings_get_int (manager->priv->touchpad_settings, KEY_SCROLL_METHOD),
                g_settings_get_boolean (manager->priv->touchpad_settings, KEY_HORIZ_SCROLL));
        if (gdkdevice_is_touchpad (device)) {
                set_natural_scroll (manager, device, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_NATURAL_SCROLL_ENABLED));
        } else {
                set_natural_scroll (manager, device, g_settings_get_boolean (manager->priv->mouse_settings, KEY_NATURAL_SCROLL_ENABLED));
        }
        if (g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_ENABLED) == FALSE)
                set_touchpad_disabled (device);

        set_scroll_wheel_button (manager, device);
}

static void
set_natural_scroll_synaptics (CsdMouseManager *manager,
                              GdkDevice       *device,
                              gboolean         natural_scroll)
{
        XDevice *xdevice;
        Atom scrolling_distance, act_type;
        int rc, act_format;
        unsigned long nitems, bytes_after;
        unsigned char *data;
        glong *ptr;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        if (!device_is_touchpad (xdevice)) {
                xdevice_close (xdevice);
                return;
        }

        g_debug ("Trying to set %s for \"%s\"",
                 natural_scroll ? "natural (reverse) scroll" : "normal scroll",
                 gdk_device_get_name (device));

        scrolling_distance = property_from_name ("Synaptics Scrolling Distance");

        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        rc = XGetDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                 scrolling_distance, 0, 2, False,
                                 XA_INTEGER, &act_type, &act_format, &nitems,
                                 &bytes_after, &data);

        if (rc == Success && act_type == XA_INTEGER && act_format == 32 && nitems >= 2) {
                ptr = (glong *) data;

                if (natural_scroll) {
                        ptr[0] = -abs(ptr[0]);
                        ptr[1] = -abs(ptr[1]);
                } else {
                        ptr[0] = abs(ptr[0]);
                        ptr[1] = abs(ptr[1]);
                }

                XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdevice,
                                       scrolling_distance, XA_INTEGER, act_format,
                                       PropModeReplace, data, nitems);
        }

        if (gdk_x11_display_error_trap_pop (gdk_display_get_default ()))
                g_warning ("Error setting %s for \"%s\"",
                           natural_scroll ? "natural (reverse) scroll" : "normal scroll",
                           gdk_device_get_name (device));

        if (rc == Success)
                XFree (data);

        xdevice_close (xdevice);
}

static void
set_natural_scroll_libinput (CsdMouseManager *manager,
                             GdkDevice       *device,
                             gboolean         natural_scroll)
{
        g_debug ("Trying to set %s for \"%s\"",
                 natural_scroll ? "natural (reverse) scroll" : "normal scroll",
                 gdk_device_get_name (device));

        XDevice *xdevice;

        xdevice = open_gdk_device (device);
        if (xdevice == NULL)
                return;

        property_set_bool (device, xdevice, "libinput Natural Scrolling Enabled", 0, natural_scroll);

        xdevice_close (xdevice);
}

static void
set_natural_scroll (CsdMouseManager *manager,
                    GdkDevice       *device,
                    gboolean         natural_scroll)
{
        if (property_from_name ("Synaptics Scrolling Distance"))
                set_natural_scroll_synaptics (manager, device, natural_scroll);

        if (property_from_name ("libinput Natural Scrolling Enabled"))
                set_natural_scroll_libinput (manager, device, natural_scroll);
}

static void
mouse_callback (GSettings       *settings,
                const gchar     *key,
                CsdMouseManager *manager)
{
        GList *devices, *l;

        if (g_str_equal (key, KEY_DWELL_CLICK_ENABLED) ||
            g_str_equal (key, KEY_SECONDARY_CLICK_ENABLED)) {
                set_mousetweaks_daemon (manager,
                                        g_settings_get_boolean (settings, KEY_DWELL_CLICK_ENABLED),
                                        g_settings_get_boolean (settings, KEY_SECONDARY_CLICK_ENABLED));
                return;
        } else if (g_str_equal (key, KEY_LOCATE_POINTER)) {
                set_locate_pointer (manager, g_settings_get_boolean (settings, KEY_LOCATE_POINTER));
                return;
        }

        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);

        for (l = devices; l != NULL; l = l->next) {
                GdkDevice *device = l->data;

                if (device_is_ignored (manager, device))
                        continue;

                if (gdkdevice_is_touchpad (device))
                        continue;

                if (g_str_equal (key, KEY_LEFT_HANDED)) {
                        gboolean mouse_left_handed;
                        mouse_left_handed = g_settings_get_boolean (settings, KEY_LEFT_HANDED);
                        set_left_handed (manager, device, mouse_left_handed, get_touchpad_handedness (manager, mouse_left_handed));
                } else if (g_str_equal (key, KEY_MOTION_ACCELERATION) ||
                           g_str_equal (key, KEY_CUSTOM_ACCELERATION) ||
                           g_str_equal (key, KEY_MOTION_THRESHOLD) ||
                           g_str_equal (key, KEY_CUSTOM_THRESHOLD)) {
                        set_motion (manager, device);
                } else if (g_str_equal (key, KEY_MIDDLE_BUTTON_EMULATION)) {
                        set_middle_button (manager, device, g_settings_get_boolean (settings, KEY_MIDDLE_BUTTON_EMULATION));
                } else if (g_str_equal (key, KEY_NATURAL_SCROLL_ENABLED)) {
                        set_natural_scroll (manager, device, g_settings_get_boolean (settings, key));
                }
        }
        g_list_free (devices);
}

static void
trackball_callback (GSettings       *settings,
                    const gchar     *key,
                    CsdMouseManager *manager)
{
        GList *devices, *l;

        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);

        for (l = devices; l != NULL; l = l->next) {
                GdkDevice *device = l->data;

                if (device_is_ignored (manager, device))
                        continue;

                set_scroll_wheel_button (manager, device);
        }
        g_list_free (devices);
}

/* Re-enable touchpad when any other pointing device isn't present. */
static void
ensure_touchpad_active (CsdMouseManager *manager)
{
        if (mouse_is_present () == FALSE && touchscreen_is_present () == FALSE && touchpad_is_present ())
                g_settings_set_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_ENABLED, TRUE);
}

static void
touchpad_callback (GSettings       *settings,
                   const gchar     *key,
                   CsdMouseManager *manager)
{
        GList *devices, *l;

        if (g_str_equal (key, KEY_TOUCHPAD_DISABLE_W_TYPING)) {
                set_disable_w_typing (manager, g_settings_get_boolean (manager->priv->touchpad_settings, key));
                return;
        }

        if (g_str_equal (key, KEY_TOUCHPAD_DISABLE_WITH_MOUSE)) {
                set_disable_w_mouse_attached (manager, g_settings_get_boolean (manager->priv->touchpad_settings, key));
                return;
        }

        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);

        for (l = devices; l != NULL; l = l->next) {
                GdkDevice *device = l->data;

                if (device_is_ignored (manager, device))
                        continue;

                if (!gdkdevice_is_touchpad (device))
                        continue;

                if (g_str_equal (key, KEY_TAP_TO_CLICK)) {
                        gboolean mouse_left_handed;
                        mouse_left_handed = g_settings_get_boolean (manager->priv->mouse_settings, KEY_LEFT_HANDED);
                        set_tap_to_click (device, g_settings_get_boolean (settings, key),
                                          get_touchpad_handedness (manager, mouse_left_handed));
                }
                else if (g_str_equal(key, KEY_CLICKPAD_CLICK)) {
                        gboolean mouse_left_handed;
                        mouse_left_handed = g_settings_get_boolean (manager->priv->mouse_settings, KEY_LEFT_HANDED);
                        set_click_actions( device, g_settings_get_int (manager->priv->touchpad_settings, KEY_CLICKPAD_CLICK), get_touchpad_handedness(manager, mouse_left_handed));
                } else if (g_str_equal (key, KEY_SCROLL_METHOD) || g_str_equal (key, KEY_HORIZ_SCROLL)) {
                        set_scrolling (device, g_settings_get_int (manager->priv->touchpad_settings, KEY_SCROLL_METHOD),
                                g_settings_get_boolean (manager->priv->touchpad_settings, KEY_HORIZ_SCROLL));
                } else if (g_str_equal (key, KEY_TOUCHPAD_ENABLED)) {
                        if (g_settings_get_boolean (settings, key) == FALSE)
                                set_touchpad_disabled (device);
                        else
                                set_touchpad_enabled (gdk_x11_device_get_id (device));
                } else if (g_str_equal (key, KEY_MOTION_ACCELERATION) ||
                           g_str_equal (key, KEY_CUSTOM_ACCELERATION) ||
                           g_str_equal (key, KEY_MOTION_THRESHOLD) ||
                           g_str_equal (key, KEY_CUSTOM_THRESHOLD)) {
                        set_motion (manager, device);
                } else if (g_str_equal (key, KEY_LEFT_HANDED)) {
                        gboolean mouse_left_handed;
                        mouse_left_handed = g_settings_get_boolean (manager->priv->mouse_settings, KEY_LEFT_HANDED);
                        set_left_handed (manager, device, mouse_left_handed, get_touchpad_handedness (manager, mouse_left_handed));
                } else if (g_str_equal (key, KEY_NATURAL_SCROLL_ENABLED)) {
                        set_natural_scroll (manager, device, g_settings_get_boolean (settings, key));
                }
        }
        g_list_free (devices);

        if (g_str_equal (key, KEY_TOUCHPAD_ENABLED) &&
            g_settings_get_boolean (settings, key)) {
                devices = get_disabled_devices (manager->priv->device_manager);
                for (l = devices; l != NULL; l = l->next) {
                        int device_id;

                        device_id = GPOINTER_TO_INT (l->data);
                        set_touchpad_enabled (device_id);
                }
                g_list_free (devices);
        }
}

static void
device_added_cb (GdkDeviceManager *device_manager,
                 GdkDevice        *device,
                 CsdMouseManager  *manager)
{
        if (device_is_ignored (manager, device) == FALSE) {
                if (run_custom_command (device, COMMAND_DEVICE_ADDED) == FALSE) {
                        set_mouse_settings (manager, device);
                } else {
                        int id;
                        g_object_get (G_OBJECT (device), "device-id", &id, NULL);
                        g_hash_table_insert (manager->priv->blacklist,
                                             GINT_TO_POINTER (id), GINT_TO_POINTER (1));
                }

                /* If a touchpad was to appear... */
                set_disable_w_typing (manager, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_DISABLE_W_TYPING));
                set_disable_w_mouse_attached (manager, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_DISABLE_WITH_MOUSE));
        }
}

static void
device_removed_cb (GdkDeviceManager *device_manager,
                   GdkDevice        *device,
                   CsdMouseManager  *manager)
{
	int id;

	/* Remove the device from the hash table so that
	 * device_is_ignored () doesn't check for blacklisted devices */
	g_object_get (G_OBJECT (device), "device-id", &id, NULL);
	g_hash_table_remove (manager->priv->blacklist,
			     GINT_TO_POINTER (id));

        if (device_is_ignored (manager, device) == FALSE) {
                run_custom_command (device, COMMAND_DEVICE_REMOVED);

                /* If a touchpad was to disappear... */
                set_disable_w_typing (manager, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_DISABLE_W_TYPING));
                ensure_touchpad_active (manager);
        }
}

static void
set_devicepresence_handler (CsdMouseManager *manager)
{
        GdkDeviceManager *device_manager;

        device_manager = gdk_display_get_device_manager (gdk_display_get_default ());

        manager->priv->device_added_id = g_signal_connect (G_OBJECT (device_manager), "device-added",
                                                           G_CALLBACK (device_added_cb), manager);
        manager->priv->device_removed_id = g_signal_connect (G_OBJECT (device_manager), "device-removed",
                                                             G_CALLBACK (device_removed_cb), manager);
        manager->priv->device_manager = device_manager;
}

static void
csd_mouse_manager_init (CsdMouseManager *manager)
{
        manager->priv = CSD_MOUSE_MANAGER_GET_PRIVATE (manager);
        manager->priv->blacklist = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static gboolean
csd_mouse_manager_idle_cb (CsdMouseManager *manager)
{
        GList *devices, *l;

        cinnamon_settings_profile_start (NULL);

        set_devicepresence_handler (manager);

        manager->priv->mouse_settings = g_settings_new (SETTINGS_MOUSE_DIR);
        g_signal_connect (manager->priv->mouse_settings, "changed",
                          G_CALLBACK (mouse_callback), manager);

        manager->priv->mouse_a11y_settings = g_settings_new ("org.cinnamon.desktop.a11y.mouse");
        g_signal_connect (manager->priv->mouse_a11y_settings, "changed",
                          G_CALLBACK (mouse_callback), manager);

        manager->priv->touchpad_settings = g_settings_new (SETTINGS_TOUCHPAD_DIR);
        g_signal_connect (manager->priv->touchpad_settings, "changed",
                          G_CALLBACK (touchpad_callback), manager);

        manager->priv->trackball_settings = g_settings_new (SETTINGS_TRACKBALL_DIR);
        g_signal_connect (manager->priv->trackball_settings, "changed",
                          G_CALLBACK (trackball_callback), manager);

        manager->priv->syndaemon_spawned = FALSE;

        set_locate_pointer (manager, g_settings_get_boolean (manager->priv->mouse_settings, KEY_LOCATE_POINTER));
        set_mousetweaks_daemon (manager,
                                g_settings_get_boolean (manager->priv->mouse_a11y_settings, KEY_DWELL_CLICK_ENABLED),
                                g_settings_get_boolean (manager->priv->mouse_a11y_settings, KEY_SECONDARY_CLICK_ENABLED));
        set_disable_w_typing (manager, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_DISABLE_W_TYPING));
        set_disable_w_mouse_attached (manager, g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_DISABLE_WITH_MOUSE));


        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);
        for (l = devices; l != NULL; l = l->next) {
                GdkDevice *device = l->data;

                if (device_is_ignored (manager, device))
                        continue;

                if (run_custom_command (device, COMMAND_DEVICE_PRESENT) == FALSE) {
                        set_mouse_settings (manager, device);
                } else {
                        int id;
                        g_object_get (G_OBJECT (device), "device-id", &id, NULL);
                        g_hash_table_insert (manager->priv->blacklist,
                                             GINT_TO_POINTER (id), GINT_TO_POINTER (1));
                }
        }
        g_list_free (devices);

        ensure_touchpad_active (manager);

        if (g_settings_get_boolean (manager->priv->touchpad_settings, KEY_TOUCHPAD_ENABLED)) {
                devices = get_disabled_devices (manager->priv->device_manager);
               for (l = devices; l != NULL; l = l->next) {
                        int device_id;

                        device_id = GPOINTER_TO_INT (l->data);
                        set_touchpad_enabled (device_id);
                }
                g_list_free (devices);
        }

        cinnamon_settings_profile_end (NULL);

        manager->priv->start_idle_id = 0;

        return FALSE;
}

gboolean
csd_mouse_manager_start (CsdMouseManager *manager,
                         GError         **error)
{
        cinnamon_settings_profile_start (NULL);

        if (!supports_xinput_devices ()) {
                g_debug ("XInput is not supported, not applying any settings");
                return TRUE;
        }

        manager->priv->start_idle_id = g_idle_add ((GSourceFunc) csd_mouse_manager_idle_cb, manager);

        cinnamon_settings_profile_end (NULL);

        return TRUE;
}

void
csd_mouse_manager_stop (CsdMouseManager *manager)
{
        CsdMouseManagerPrivate *p = manager->priv;

        g_debug ("Stopping mouse manager");

        if (manager->priv->start_idle_id != 0) {
                g_source_remove (manager->priv->start_idle_id);
                manager->priv->start_idle_id = 0;
        }

        if (p->device_manager != NULL) {
                g_signal_handler_disconnect (p->device_manager, p->device_added_id);
                g_signal_handler_disconnect (p->device_manager, p->device_removed_id);
                p->device_manager = NULL;
        }

        g_clear_object (&p->mouse_a11y_settings);
        g_clear_object (&p->mouse_settings);
        g_clear_object (&p->touchpad_settings);
        g_clear_object (&p->trackball_settings);

        set_locate_pointer (manager, FALSE);
}

static void
csd_mouse_manager_finalize (GObject *object)
{
        CsdMouseManager *mouse_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CSD_IS_MOUSE_MANAGER (object));

        mouse_manager = CSD_MOUSE_MANAGER (object);

        g_return_if_fail (mouse_manager->priv != NULL);

        csd_mouse_manager_stop (mouse_manager);

        G_OBJECT_CLASS (csd_mouse_manager_parent_class)->finalize (object);
}

CsdMouseManager *
csd_mouse_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (CSD_TYPE_MOUSE_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return CSD_MOUSE_MANAGER (manager_object);
}
