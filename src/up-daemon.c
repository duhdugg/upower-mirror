/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 David Zeuthen <davidz@redhat.com>
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>

#include "up-config.h"
#include "up-constants.h"
#include "up-device-list.h"
#include "up-device.h"
#include "up-backend.h"
#include "up-daemon.h"

struct UpDaemonPrivate
{
	UpConfig		*config;
	UpBackend		*backend;
	UpDeviceList		*power_devices;
	guint			 action_timeout_id;
	GHashTable		*poll_timeouts;
	gboolean                 poll_paused;
	GHashTable		*idle_signals;
	int			 critical_action_lock_fd;

	/* Properties */
	UpDeviceLevel		 warning_level;

	/* Display battery properties */
	UpDevice		*display_device;
	UpDeviceKind		 kind;
	UpDeviceState		 state;
	gdouble			 percentage;
	gdouble			 energy;
	gdouble			 energy_full;
	gdouble			 energy_rate;
	gint64			 time_to_empty;
	gint64			 time_to_full;

	/* WarningLevel configuration */
	gboolean		 use_percentage_for_policy;
	guint			 low_percentage;
	guint			 critical_percentage;
	guint			 action_percentage;
	guint			 low_time;
	guint			 critical_time;
	guint			 action_time;
};

static void	up_daemon_finalize		(GObject	*object);
static gboolean	up_daemon_get_on_battery_local	(UpDaemon	*daemon);
static gboolean	up_daemon_get_warning_level_local(UpDaemon	*daemon);
static gboolean	up_daemon_get_on_ac_local 	(UpDaemon	*daemon);

G_DEFINE_TYPE_WITH_PRIVATE (UpDaemon, up_daemon, UP_TYPE_EXPORTED_DAEMON_SKELETON)

#define UP_DAEMON_ACTION_DELAY				20 /* seconds */
#define UP_INTERFACE_PREFIX				"org.freedesktop.UPower."

/**
 * up_daemon_get_on_battery_local:
 *
 * As soon as _any_ battery goes discharging, this is true
 **/
static gboolean
up_daemon_get_on_battery_local (UpDaemon *daemon)
{
	guint i;
	gboolean ret;
	gboolean result = FALSE;
	gboolean on_battery;
	UpDevice *device;
	GPtrArray *array;

	/* ask each device */
	array = up_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (UpDevice *) g_ptr_array_index (array, i);
		ret = up_device_get_on_battery (device, &on_battery);
		if (ret && on_battery) {
			result = TRUE;
			break;
		}
	}
	g_ptr_array_unref (array);
	return result;
}

/**
 * up_daemon_get_number_devices_of_type:
 **/
guint
up_daemon_get_number_devices_of_type (UpDaemon *daemon, UpDeviceKind type)
{
	guint i;
	UpDevice *device;
	GPtrArray *array;
	UpDeviceKind type_tmp;
	guint count = 0;

	/* ask each device */
	array = up_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (UpDevice *) g_ptr_array_index (array, i);
		g_object_get (device,
			      "type", &type_tmp,
			      NULL);
		if (type == type_tmp)
			count++;
	}
	g_ptr_array_unref (array);
	return count;
}

/**
 * up_daemon_update_display_battery:
 *
 * Update our internal state.
 *
 * Returns: %TRUE if the state changed.
 **/
static gboolean
up_daemon_update_display_battery (UpDaemon *daemon)
{
	guint i;
	GPtrArray *array;

	UpDeviceKind kind_total = UP_DEVICE_KIND_UNKNOWN;
	UpDeviceState state_total = UP_DEVICE_STATE_UNKNOWN;
	gdouble percentage_total = 0.0;
	gdouble energy_total = 0.0;
	gdouble energy_full_total = 0.0;
	gdouble energy_rate_total = 0.0;
	gint64 time_to_empty_total = 0;
	gint64 time_to_full_total = 0;
	gboolean is_present_total = FALSE;
	guint num_batteries = 0;

	/* Gather state from each device */
	array = up_device_list_get_array (daemon->priv->power_devices);
	for (i = 0; i < array->len; i++) {
		UpDevice *device;

		UpDeviceState state = UP_DEVICE_STATE_UNKNOWN;
		UpDeviceKind kind = UP_DEVICE_KIND_UNKNOWN;
		gdouble percentage = 0.0;
		gdouble energy = 0.0;
		gdouble energy_full = 0.0;
		gdouble energy_rate = 0.0;
		gint64 time_to_empty = 0;
		gint64 time_to_full = 0;
		gboolean power_supply = FALSE;

		device = g_ptr_array_index (array, i);
		g_object_get (device,
			      "type", &kind,
			      "state", &state,
			      "percentage", &percentage,
			      "energy", &energy,
			      "energy-full", &energy_full,
			      "energy-rate", &energy_rate,
			      "time-to-empty", &time_to_empty,
			      "time-to-full", &time_to_full,
			      "power-supply", &power_supply,
			      NULL);

		/* When we have a UPS, it's either a desktop, and
		 * has no batteries, or a laptop, in which case we
		 * ignore the batteries */
		if (kind == UP_DEVICE_KIND_UPS) {
			kind_total = kind;
			state_total = state;
			energy_total = energy;
			energy_full_total = energy_full;
			energy_rate_total = energy_rate;
			time_to_empty_total = time_to_empty;
			time_to_full_total = time_to_full;
			percentage_total = percentage;
			is_present_total = TRUE;
			break;
		}
		if (kind != UP_DEVICE_KIND_BATTERY ||
		    power_supply == FALSE)
			continue;

		/* If one battery is charging, the composite is charging
		 * If all batteries are discharging or pending-charge, the composite is discharging
		 * If all batteries are fully charged, the composite is fully charged
		 * If one battery is pending-charge and no other is charging or discharging, then the composite is pending-charge
		 * Everything else is unknown */
		if (state == UP_DEVICE_STATE_CHARGING)
			state_total = UP_DEVICE_STATE_CHARGING;
		else if (state == UP_DEVICE_STATE_DISCHARGING &&
			 state_total != UP_DEVICE_STATE_CHARGING)
			state_total = UP_DEVICE_STATE_DISCHARGING;
		else if (state == UP_DEVICE_STATE_PENDING_CHARGE &&
			 (state_total == UP_DEVICE_STATE_UNKNOWN || state_total == UP_DEVICE_STATE_PENDING_CHARGE))
			state_total = UP_DEVICE_STATE_PENDING_CHARGE;
		else if (state == UP_DEVICE_STATE_FULLY_CHARGED &&
			 state_total == UP_DEVICE_STATE_UNKNOWN)
			state_total = UP_DEVICE_STATE_FULLY_CHARGED;

		/* sum up composite */
		kind_total = UP_DEVICE_KIND_BATTERY;
		is_present_total = TRUE;
		energy_total += energy;
		energy_full_total += energy_full;
		energy_rate_total += energy_rate;
		time_to_empty_total += time_to_empty;
		time_to_full_total += time_to_full;
		/* Will be recalculated for multiple batteries, no worries */
		percentage_total += percentage;
		num_batteries++;
	}

	/* Handle multiple batteries */
	if (num_batteries <= 1)
		goto out;

	g_debug ("Calculating percentage and time to full/to empty for %i batteries", num_batteries);

	/* use percentage weighted for each battery capacity */
	if (energy_full_total > 0.0)
		percentage_total = 100.0 * energy_total / energy_full_total;

	/* calculate a quick and dirty time remaining value */
	if (energy_rate_total > 0) {
		if (state_total == UP_DEVICE_STATE_DISCHARGING)
			time_to_empty_total = SECONDS_PER_HOUR * (energy_total / energy_rate_total);
		else if (state_total == UP_DEVICE_STATE_CHARGING)
			time_to_full_total = SECONDS_PER_HOUR * ((energy_full_total - energy_total) / energy_rate_total);
	}

out:
	g_ptr_array_unref (array);

	/* Did anything change? */
	if (daemon->priv->kind == kind_total &&
	    daemon->priv->state == state_total &&
	    daemon->priv->energy == energy_total &&
	    daemon->priv->energy_full == energy_full_total &&
	    daemon->priv->energy_rate == energy_rate_total &&
	    daemon->priv->time_to_empty == time_to_empty_total &&
	    daemon->priv->time_to_full == time_to_full_total &&
	    daemon->priv->percentage == percentage_total)
		return FALSE;

	daemon->priv->kind = kind_total;
	daemon->priv->state = state_total;
	daemon->priv->energy = energy_total;
	daemon->priv->energy_full = energy_full_total;
	daemon->priv->energy_rate = energy_rate_total;
	daemon->priv->time_to_empty = time_to_empty_total;
	daemon->priv->time_to_full = time_to_full_total;

	daemon->priv->percentage = percentage_total;

	g_object_set (daemon->priv->display_device,
		      "type", kind_total,
		      "state", state_total,
		      "energy", energy_total,
		      "energy-full", energy_full_total,
		      "energy-rate", energy_rate_total,
		      "time-to-empty", time_to_empty_total,
		      "time-to-full", time_to_full_total,
		      "percentage", percentage_total,
		      "is-present", is_present_total,
		      "power-supply", TRUE,
		      "update-time", (guint64) g_get_real_time () / G_USEC_PER_SEC,
		      NULL);

	return TRUE;
}

/**
 * up_daemon_get_warning_level_local:
 *
 * As soon as _all_ batteries are low, this is true
 **/
static gboolean
up_daemon_get_warning_level_local (UpDaemon *daemon)
{
	up_daemon_update_display_battery (daemon);
	if (daemon->priv->kind != UP_DEVICE_KIND_UPS &&
	    daemon->priv->kind != UP_DEVICE_KIND_BATTERY)
		return UP_DEVICE_LEVEL_NONE;

	if (daemon->priv->kind == UP_DEVICE_KIND_UPS &&
	    daemon->priv->state != UP_DEVICE_STATE_DISCHARGING)
		return UP_DEVICE_LEVEL_NONE;

	/* Check to see if the batteries have not noticed we are on AC */
	if (daemon->priv->kind == UP_DEVICE_KIND_BATTERY &&
	    up_daemon_get_on_ac_local (daemon))
		return UP_DEVICE_LEVEL_NONE;

	return up_daemon_compute_warning_level (daemon,
						daemon->priv->state,
						daemon->priv->kind,
						TRUE, /* power_supply */
						daemon->priv->percentage,
						daemon->priv->time_to_empty);
}

/**
 * up_daemon_get_on_ac_local:
 *
 * As soon as _any_ ac supply goes online, this is true
 **/
static gboolean
up_daemon_get_on_ac_local (UpDaemon *daemon)
{
	guint i;
	gboolean ret;
	gboolean result = FALSE;
	gboolean online;
	UpDevice *device;
	GPtrArray *array;

	/* ask each device */
	array = up_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (UpDevice *) g_ptr_array_index (array, i);
		ret = up_device_get_online (device, &online);
		if (ret && online) {
			result = TRUE;
			break;
		}
	}
	g_ptr_array_unref (array);
	return result;
}

/**
 * up_daemon_refresh_battery_devices:
 **/
static gboolean
up_daemon_refresh_battery_devices (UpDaemon *daemon)
{
	guint i;
	GPtrArray *array;
	UpDevice *device;

	/* refresh all devices in array */
	array = up_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		UpDeviceKind type;
		gboolean power_supply;

		device = (UpDevice *) g_ptr_array_index (array, i);
		/* only refresh battery devices */
		g_object_get (device,
			      "type", &type,
			      "power-supply", &power_supply,
			      NULL);
		if (type == UP_DEVICE_KIND_BATTERY &&
		    power_supply)
			up_device_refresh_internal (device);
	}
	g_ptr_array_unref (array);

	return TRUE;
}

/**
 * up_daemon_enumerate_devices:
 **/
static gboolean
up_daemon_enumerate_devices (UpExportedDaemon *skeleton,
			     GDBusMethodInvocation *invocation,
			     UpDaemon *daemon)
{
	guint i;
	GPtrArray *array;
	GPtrArray *object_paths;
	UpDevice *device;

	/* build a pointer array of the object paths */
	object_paths = g_ptr_array_new_with_free_func (g_free);
	array = up_device_list_get_array (daemon->priv->power_devices);
	for (i = 0; i < array->len; i++) {
		device = (UpDevice *) g_ptr_array_index (array, i);
		g_ptr_array_add (object_paths, g_strdup (up_device_get_object_path (device)));
	}
	g_ptr_array_unref (array);
	g_ptr_array_add (object_paths, NULL);

	/* return it on the bus */
	up_exported_daemon_complete_enumerate_devices (skeleton, invocation,
						       (const gchar **) object_paths->pdata);

	/* free */
	g_ptr_array_unref (object_paths);
	return TRUE;
}

/**
 * up_daemon_get_display_device:
 **/
static gboolean
up_daemon_get_display_device (UpExportedDaemon *skeleton,
			      GDBusMethodInvocation *invocation,
			      UpDaemon *daemon)
{
	up_exported_daemon_complete_get_display_device (skeleton, invocation,
							up_device_get_object_path (daemon->priv->display_device));
	return TRUE;
}

/**
 * up_daemon_get_critical_action:
 **/
static gboolean
up_daemon_get_critical_action (UpExportedDaemon *skeleton,
			       GDBusMethodInvocation *invocation,
			       UpDaemon *daemon)
{
	up_exported_daemon_complete_get_critical_action (skeleton, invocation,
							 up_backend_get_critical_action (daemon->priv->backend));
	return TRUE;
}

/**
 * up_daemon_register_power_daemon:
 **/
static gboolean
up_daemon_register_power_daemon (UpDaemon *daemon,
				 GDBusConnection *connection)
{
	GError *error = NULL;

	/* export our interface on the bus */
	g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (daemon),
					  connection,
					  "/org/freedesktop/UPower",
					  &error);

	if (error != NULL) {
		g_critical ("error registering daemon on system bus: %s", error->message);
		g_error_free (error);
		return FALSE;
	}

	/* Register the display device */
	up_device_register_display_device (daemon->priv->display_device, daemon);

	return TRUE;
}

/**
 * up_daemon_startup:
 **/
gboolean
up_daemon_startup (UpDaemon *daemon,
		   GDBusConnection *connection)
{
	gboolean ret;
	gboolean on_battery;
	UpDeviceLevel warning_level;
	UpDaemonPrivate *priv = daemon->priv;

	/* register on bus */
	ret = up_daemon_register_power_daemon (daemon, connection);
	if (!ret) {
		g_warning ("failed to register");
		goto out;
	}

	g_debug ("daemon now coldplug");

	/* coldplug backend backend */
	ret = up_backend_coldplug (priv->backend, daemon);
	if (!ret) {
		g_warning ("failed to coldplug backend");
		goto out;
	}

	/* get battery state */
	on_battery = (up_daemon_get_on_battery_local (daemon) &&
		      !up_daemon_get_on_ac_local (daemon));
	warning_level = up_daemon_get_warning_level_local (daemon);
	up_daemon_set_on_battery (daemon, on_battery);
	up_daemon_set_warning_level (daemon, warning_level);

	g_debug ("daemon now not coldplug");

out:
	return ret;
}

/**
 * up_daemon_shutdown:
 *
 * Stop the daemon, release all devices and resources.
 **/
void
up_daemon_shutdown (UpDaemon *daemon)
{
	/* stop accepting new devices and clear backend state */
	up_backend_unplug (daemon->priv->backend);

	/* forget about discovered devices and release UpDaemon reference */
	up_device_list_clear (daemon->priv->power_devices, TRUE);

	/* release UpDaemon reference */
	up_device_unplug (daemon->priv->display_device);
}

/**
 * up_daemon_get_device_list:
 **/
UpDeviceList *
up_daemon_get_device_list (UpDaemon *daemon)
{
	return g_object_ref (daemon->priv->power_devices);
}

/**
 * up_daemon_set_on_battery:
 **/
void
up_daemon_set_on_battery (UpDaemon *daemon, gboolean on_battery)
{
	g_debug ("on_battery = %s", on_battery ? "yes" : "no");
	up_exported_daemon_set_on_battery (UP_EXPORTED_DAEMON (daemon), on_battery);
}

static gboolean
take_action_timeout_cb (UpDaemon *daemon)
{
	/* Release the inhibitor lock first, otherwise our action may be canceled */
	if (daemon->priv->critical_action_lock_fd >= 0) {
		close (daemon->priv->critical_action_lock_fd);
		daemon->priv->critical_action_lock_fd = -1;
	}

	up_backend_take_action (daemon->priv->backend);

	g_debug ("Backend was notified to take action. The timeout will be removed.");
	daemon->priv->action_timeout_id = 0;

	return G_SOURCE_REMOVE;
}

/**
 * up_daemon_set_warning_level:
 **/
void
up_daemon_set_warning_level (UpDaemon *daemon, UpDeviceLevel warning_level)
{
	UpDaemonPrivate *priv = daemon->priv;

	if (priv->warning_level == warning_level)
		return;

	g_debug ("warning_level = %s", up_device_level_to_string (warning_level));
	priv->warning_level = warning_level;

	g_object_set (G_OBJECT (daemon->priv->display_device),
		      "warning-level", warning_level,
		      "update-time", (guint64) g_get_real_time () / G_USEC_PER_SEC,
		      NULL);

	if (daemon->priv->warning_level == UP_DEVICE_LEVEL_ACTION) {
		if (daemon->priv->action_timeout_id == 0) {
			g_debug ("About to take action in %d seconds", UP_DAEMON_ACTION_DELAY);
			daemon->priv->critical_action_lock_fd = up_backend_inhibitor_lock_take (daemon->priv->backend, "Execute critical action", "block");
			daemon->priv->action_timeout_id = g_timeout_add_seconds (UP_DAEMON_ACTION_DELAY,
										 (GSourceFunc) take_action_timeout_cb,
										 daemon);
			g_source_set_name_by_id (daemon->priv->action_timeout_id, "[upower] take_action_timeout_cb");
		} else {
			g_debug ("Not taking action, timeout id already set");
		}
	} else {
		if (daemon->priv->action_timeout_id > 0) {
			g_debug ("Removing timeout as action level changed");
			g_source_remove (daemon->priv->action_timeout_id);
		}
	}
}

UpDeviceLevel
up_daemon_compute_warning_level (UpDaemon      *daemon,
				 UpDeviceState  state,
				 UpDeviceKind   kind,
				 gboolean       power_supply,
				 gdouble        percentage,
				 gint64         time_to_empty)
{
	gboolean use_percentage = TRUE;
	UpDeviceLevel default_level = UP_DEVICE_LEVEL_NONE;

	if (state != UP_DEVICE_STATE_DISCHARGING)
		return UP_DEVICE_LEVEL_NONE;

	/* Keyboard and mice usually have a coarser
	 * battery level, so this avoids falling directly
	 * into critical (or off) before any warnings */
	if (kind == UP_DEVICE_KIND_MOUSE ||
	    kind == UP_DEVICE_KIND_KEYBOARD ||
	    kind == UP_DEVICE_KIND_TOUCHPAD) {
		if (percentage <= 5.0f)
			return UP_DEVICE_LEVEL_CRITICAL;
		else if (percentage <= 10.0f)
			return  UP_DEVICE_LEVEL_LOW;
		else
			return UP_DEVICE_LEVEL_NONE;
	} else if (kind == UP_DEVICE_KIND_UPS) {
		default_level = UP_DEVICE_LEVEL_DISCHARGING;
	}

	if (power_supply &&
	    !daemon->priv->use_percentage_for_policy &&
	    time_to_empty > 0.0)
		use_percentage = FALSE;

	if (use_percentage) {
		if (percentage > daemon->priv->low_percentage)
			return default_level;
		if (percentage > daemon->priv->critical_percentage)
			return UP_DEVICE_LEVEL_LOW;
		if (percentage > daemon->priv->action_percentage)
			return UP_DEVICE_LEVEL_CRITICAL;
		return UP_DEVICE_LEVEL_ACTION;
	} else {
		if (time_to_empty > daemon->priv->low_time)
			return default_level;
		if (time_to_empty > daemon->priv->critical_time)
			return UP_DEVICE_LEVEL_LOW;
		if (time_to_empty > daemon->priv->action_time)
			return UP_DEVICE_LEVEL_CRITICAL;
		return UP_DEVICE_LEVEL_ACTION;
	}
	g_assert_not_reached ();
}

static void
up_daemon_update_warning_level (UpDaemon *daemon)
{
	gboolean ret;
	UpDaemonPrivate *priv = daemon->priv;
	UpDeviceLevel warning_level;

	/* Check if the on_battery and warning_level state has changed */
	ret = (up_daemon_get_on_battery_local (daemon) && !up_daemon_get_on_ac_local (daemon));
	up_daemon_set_on_battery (daemon, ret);

	warning_level = up_daemon_get_warning_level_local (daemon);
	if (warning_level != priv->warning_level)
		up_daemon_set_warning_level (daemon, warning_level);
}

const gchar *
up_daemon_get_charge_icon (UpDaemon     *daemon,
			   gdouble       percentage,
			   UpDeviceLevel battery_level,
			   gboolean      charging)
{
	if (battery_level == UP_DEVICE_LEVEL_NONE && daemon != NULL) {
		if (percentage <= daemon->priv->low_percentage)
			return charging ? "battery-caution-charging-symbolic" : "battery-caution-symbolic";
		else if (percentage < 30)
			return charging ? "battery-low-charging-symbolic" : "battery-low-symbolic";
		else if (percentage < 60)
			return charging ? "battery-good-charging-symbolic" : "battery-good-symbolic";
		return charging ? "battery-full-charging-symbolic" : "battery-full-symbolic";
	} else {
		switch (battery_level) {
		case UP_DEVICE_LEVEL_UNKNOWN:
			/* The lack of symmetry is on purpose */
			return charging ? "battery-good-charging-symbolic" : "battery-caution-symbolic";
		case UP_DEVICE_LEVEL_LOW:
		case UP_DEVICE_LEVEL_CRITICAL:
			return charging ? "battery-caution-charging-symbolic" : "battery-caution-symbolic";
		case UP_DEVICE_LEVEL_NORMAL:
			return charging ? "battery-low-charging-symbolic" : "battery-low-symbolic";
		case UP_DEVICE_LEVEL_HIGH:
			return charging ? "battery-good-charging-symbolic" : "battery-good-symbolic";
		case UP_DEVICE_LEVEL_FULL:
			return charging ? "battery-full-charging-symbolic" : "battery-full-symbolic";
		default:
			g_assert_not_reached ();
		}
	}
}

/**
 * up_daemon_device_changed_cb:
 **/
static void
up_daemon_device_changed_cb (UpDevice *device, GParamSpec *pspec, UpDaemon *daemon)
{
	UpDeviceKind type;

	g_return_if_fail (UP_IS_DAEMON (daemon));
	g_return_if_fail (UP_IS_DEVICE (device));

	/* refresh battery devices when AC state changes */
	g_object_get (device,
		      "type", &type,
		      NULL);
	if (type == UP_DEVICE_KIND_LINE_POWER) {
		/* refresh now */
		up_daemon_refresh_battery_devices (daemon);
	}

	up_daemon_update_warning_level (daemon);
}

typedef struct {
	guint id;
	guint timeout;
	GSourceFunc callback;
} TimeoutData;

static void
change_idle_timeout (UpDevice   *device,
		     GParamSpec *pspec,
		     gpointer    user_data)
{
	TimeoutData *data;
	GSourceFunc callback;
	UpDaemon *daemon;

	daemon = up_device_get_daemon (device);

	data = g_hash_table_lookup (daemon->priv->poll_timeouts, device);
	callback = data->callback;

	up_daemon_stop_poll (G_OBJECT (device));
	up_daemon_start_poll (G_OBJECT (device), callback);
	g_object_unref (daemon);
}

static void
device_destroyed (gpointer  user_data,
		  GObject  *where_the_object_was)
{
	UpDaemon *daemon = user_data;
	TimeoutData *data;

	data = g_hash_table_lookup (daemon->priv->poll_timeouts, where_the_object_was);
	if (data == NULL)
		return;
	g_source_remove (data->id);
	g_hash_table_remove (daemon->priv->poll_timeouts, where_the_object_was);
}

static gboolean
fire_timeout_callback (gpointer user_data)
{
	UpDevice *device = user_data;
	TimeoutData *data;
	UpDaemon *daemon;

	daemon = up_device_get_daemon (device);

	data = g_hash_table_lookup (daemon->priv->poll_timeouts, device);
	g_assert (data);

	g_debug ("Firing timeout for '%s' after %u seconds",
		 up_exported_device_get_native_path (UP_EXPORTED_DEVICE (device)),
		 data->timeout);

	/* Fire the actual callback */
	(data->callback) (device);
	g_object_unref (daemon);

	return G_SOURCE_CONTINUE;
}

static guint
calculate_timeout (UpDevice *device)
{
	UpDeviceLevel warning_level;

	g_object_get (G_OBJECT (device), "warning-level", &warning_level, NULL);
	if (warning_level >= UP_DEVICE_LEVEL_DISCHARGING)
		return UP_DAEMON_SHORT_TIMEOUT;
	return UP_DAEMON_LONG_TIMEOUT;
}

static void
enable_poll_for_device (UpDevice *device, TimeoutData *data)
{
	const char *path;
	guint timeout;
	char *name;

	path = up_exported_device_get_native_path (UP_EXPORTED_DEVICE (device));

	timeout = calculate_timeout (device);
	data->timeout = timeout;

	data->id = g_timeout_add_seconds (timeout, fire_timeout_callback, device);
	name = g_strdup_printf ("[upower] UpDevice::poll for %s (%u secs)",
				path, timeout);
	g_source_set_name_by_id (data->id, name);
	g_free (name);
}

static void
enable_warning_level_notifications (UpDaemon *daemon, UpDevice *device)
{
	gulong handler_id;

	handler_id = g_signal_connect (device, "notify::warning-level",
				       G_CALLBACK (change_idle_timeout), NULL);
	g_hash_table_insert (daemon->priv->idle_signals, device,
			     GUINT_TO_POINTER (handler_id));
	g_object_weak_ref (G_OBJECT (device), device_destroyed, daemon);
}

static void
disable_warning_level_notifications (UpDaemon *daemon, UpDevice *device)
{
	gulong handler_id;
	gpointer value;

	value = g_hash_table_lookup (daemon->priv->idle_signals, device);
	if (value == NULL)
		return;

	handler_id = GPOINTER_TO_UINT (value);
	if (g_signal_handler_is_connected (device, handler_id))
		g_signal_handler_disconnect (device, handler_id);

	g_hash_table_remove (daemon->priv->idle_signals, device);
}

void
up_daemon_start_poll (GObject     *object,
		      GSourceFunc  callback)
{
	UpDaemon *daemon;
	UpDevice *device;
	TimeoutData *data;
	const char *path;

	device = UP_DEVICE (object);
	daemon = up_device_get_daemon (device);

	path = up_exported_device_get_native_path (UP_EXPORTED_DEVICE (device));

	if (g_hash_table_lookup (daemon->priv->poll_timeouts, device) != NULL) {
		g_warning ("Poll already started for device '%s'", path);
		goto out;
	}

	data = g_new0 (TimeoutData, 1);
	data->callback = callback;

	g_hash_table_insert (daemon->priv->poll_timeouts, device, data);

	if (daemon->priv->poll_paused)
		goto out;

	enable_warning_level_notifications (daemon, device);
	enable_poll_for_device (device, data);

	g_debug ("Setup poll for '%s' every %u seconds", path, data->timeout);
out:
	g_object_unref (daemon);
}

void
up_daemon_stop_poll (GObject *object)
{
	UpDevice *device;
	TimeoutData *data;
	UpDaemon *daemon;

	device = UP_DEVICE (object);
	daemon = up_device_get_daemon (device);

	disable_warning_level_notifications (daemon, device);

	data = g_hash_table_lookup (daemon->priv->poll_timeouts, device);
	if (data == NULL)
		goto out;

	g_source_remove (data->id);
	g_object_weak_unref (object, device_destroyed, daemon);
	g_hash_table_remove (daemon->priv->poll_timeouts, device);
out:
	g_object_unref (daemon);
}

/**
 * up_daemon_pause_poll:
 *
 * Pause, i.e. stop, all registered poll sources. They can be
 * restarted via up_daemon_resume_poll().
 **/
void
up_daemon_pause_poll (UpDaemon *daemon)
{
	GHashTableIter iter;
	gpointer key, value;

	g_debug ("Polling will be paused");

	daemon->priv->poll_paused = TRUE;

	g_hash_table_iter_init (&iter, daemon->priv->poll_timeouts);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		UpDevice *device = key;
		TimeoutData *data = value;

		if (data->id != 0) {
			g_source_remove (data->id);
			data->id = 0;
		}

		disable_warning_level_notifications (daemon, device);

		g_debug ("Poll paused '%s'", up_device_get_object_path (device));
	}
}

/**
 * up_daemon_resume_poll:
 *
 * Resume all poll sources; the timeout will be recalculated.
 **/
void
up_daemon_resume_poll (UpDaemon *daemon)
{
	GHashTableIter iter;
	gpointer key, value;

	g_debug ("Polling will be resumed");

	g_hash_table_iter_init (&iter, daemon->priv->poll_timeouts);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		UpDevice *device = key;
		TimeoutData *data = value;

		enable_poll_for_device (device, data);
		enable_warning_level_notifications (daemon, device);

		g_debug ("Poll resumed for '%s' every %u seconds",
			 up_device_get_object_path (device), data->timeout);
	}

	daemon->priv->poll_paused = FALSE;
}

/**
 * up_daemon_device_added_cb:
 **/
static void
up_daemon_device_added_cb (UpBackend *backend, GObject *native, UpDevice *device, UpDaemon *daemon)
{
	const gchar *object_path;
	UpDaemonPrivate *priv = daemon->priv;

	g_return_if_fail (UP_IS_DAEMON (daemon));
	g_return_if_fail (UP_IS_DEVICE (device));
	g_return_if_fail (G_IS_OBJECT (native));

	/* add to device list */
	up_device_list_insert (priv->power_devices, native, G_OBJECT (device));

	/* connect, so we get changes */
	g_signal_connect (device, "notify",
			  G_CALLBACK (up_daemon_device_changed_cb), daemon);

	/* emit */
	object_path = up_device_get_object_path (device);
	g_debug ("emitting added: %s", object_path);

	/* don't crash the session */
	if (object_path == NULL) {
		g_warning ("INTERNAL STATE CORRUPT (device-added): not sending NULL, native:%p, device:%p", native, device);
		return;
	}
	up_exported_daemon_emit_device_added (UP_EXPORTED_DAEMON (daemon), object_path);
}

/**
 * up_daemon_device_removed_cb:
 **/
static void
up_daemon_device_removed_cb (UpBackend *backend, GObject *native, UpDevice *device, UpDaemon *daemon)
{
	const gchar *object_path;
	UpDaemonPrivate *priv = daemon->priv;

	g_return_if_fail (UP_IS_DAEMON (daemon));
	g_return_if_fail (UP_IS_DEVICE (device));
	g_return_if_fail (G_IS_OBJECT (native));

	/* remove from list */
	up_device_list_remove (priv->power_devices, G_OBJECT(device));

	/* emit */
	object_path = up_device_get_object_path (device);
	g_debug ("emitting device-removed: %s", object_path);

	/* don't crash the session */
	if (object_path == NULL) {
		g_warning ("INTERNAL STATE CORRUPT (device-removed): not sending NULL, native:%p, device:%p", native, device);
		return;
	}
	up_exported_daemon_emit_device_removed (UP_EXPORTED_DAEMON (daemon), object_path);

	/* finalise the object */
	g_object_unref (device);

	/* In case a battery was removed */
	up_daemon_refresh_battery_devices (daemon);
	up_daemon_update_warning_level (daemon);
}

#define LOAD_OR_DEFAULT(val, str, def) val = (load_default ? def : up_config_get_uint (daemon->priv->config, str))

static void
load_percentage_policy (UpDaemon    *daemon,
			gboolean     load_default)
{
	LOAD_OR_DEFAULT (daemon->priv->low_percentage, "PercentageLow", 20);
	LOAD_OR_DEFAULT (daemon->priv->critical_percentage, "PercentageCritical", 5);
	LOAD_OR_DEFAULT (daemon->priv->action_percentage, "PercentageAction", 2);
}

static void
load_time_policy (UpDaemon    *daemon,
		  gboolean     load_default)
{
	LOAD_OR_DEFAULT (daemon->priv->low_time, "TimeLow", 1200);
	LOAD_OR_DEFAULT (daemon->priv->critical_time, "TimeCritical", 300);
	LOAD_OR_DEFAULT (daemon->priv->action_time, "TimeAction", 120);
}

#define IS_DESCENDING(x, y, z) (x > y && y > z)

static void
policy_config_validate (UpDaemon *daemon)
{
	if (daemon->priv->low_percentage >= 100 ||
	    daemon->priv->critical_percentage >= 100 ||
	    daemon->priv->action_percentage >= 100) {
		load_percentage_policy (daemon, TRUE);
	} else if (!IS_DESCENDING (daemon->priv->low_percentage,
				   daemon->priv->critical_percentage,
				   daemon->priv->action_percentage)) {
		load_percentage_policy (daemon, TRUE);
	}

	if (!IS_DESCENDING (daemon->priv->low_time,
			    daemon->priv->critical_time,
			    daemon->priv->action_time)) {
		load_time_policy (daemon, TRUE);
	}
}

/**
 * up_daemon_init:
 **/
static void
up_daemon_init (UpDaemon *daemon)
{
	daemon->priv = up_daemon_get_instance_private (daemon);
	daemon->priv->config = up_config_new ();
	daemon->priv->power_devices = up_device_list_new ();
	daemon->priv->display_device = up_device_new ();

	daemon->priv->use_percentage_for_policy = up_config_get_boolean (daemon->priv->config, "UsePercentageForPolicy");
	load_percentage_policy (daemon, FALSE);
	load_time_policy (daemon, FALSE);
	policy_config_validate (daemon);

	daemon->priv->backend = up_backend_new ();
	g_signal_connect (daemon->priv->backend, "device-added",
			  G_CALLBACK (up_daemon_device_added_cb), daemon);
	g_signal_connect (daemon->priv->backend, "device-removed",
			  G_CALLBACK (up_daemon_device_removed_cb), daemon);

	daemon->priv->poll_timeouts = g_hash_table_new_full (g_direct_hash, g_direct_equal,
							     NULL, g_free);
	daemon->priv->idle_signals = g_hash_table_new (g_direct_hash, g_direct_equal);

	up_exported_daemon_set_daemon_version (UP_EXPORTED_DAEMON (daemon), PACKAGE_VERSION);

	g_signal_connect (daemon, "handle-enumerate-devices",
			  G_CALLBACK (up_daemon_enumerate_devices), daemon);
	g_signal_connect (daemon, "handle-get-critical-action",
			  G_CALLBACK (up_daemon_get_critical_action), daemon);
	g_signal_connect (daemon, "handle-get-display-device",
			  G_CALLBACK (up_daemon_get_display_device), daemon);
}

static const GDBusErrorEntry up_daemon_error_entries[] = {
	{ UP_DAEMON_ERROR_GENERAL, UP_INTERFACE_PREFIX "GeneralError" },
	{ UP_DAEMON_ERROR_NOT_SUPPORTED, UP_INTERFACE_PREFIX "NotSupported" },
	{ UP_DAEMON_ERROR_NO_SUCH_DEVICE, UP_INTERFACE_PREFIX "NoSuchDevice" },
};

/**
 * up_daemon_error_quark:
 **/
GQuark
up_daemon_error_quark (void)
{
	static volatile gsize quark_volatile = 0;

	g_dbus_error_register_error_domain ("up_daemon_error",
					    &quark_volatile,
					    up_daemon_error_entries,
					    G_N_ELEMENTS (up_daemon_error_entries));
	return quark_volatile;
}

/**
 * up_daemon_class_init:
 **/
static void
up_daemon_class_init (UpDaemonClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_daemon_finalize;
}

/**
 * up_daemon_finalize:
 **/
static void
up_daemon_finalize (GObject *object)
{
	UpDaemon *daemon = UP_DAEMON (object);
	UpDaemonPrivate *priv = daemon->priv;

	if (priv->action_timeout_id != 0)
		g_source_remove (priv->action_timeout_id);

	if (priv->critical_action_lock_fd >= 0) {
		close (priv->critical_action_lock_fd);
		priv->critical_action_lock_fd = -1;
	}

	g_clear_pointer (&priv->poll_timeouts, g_hash_table_destroy);
	g_clear_pointer (&priv->idle_signals, g_hash_table_destroy);

	g_object_unref (priv->power_devices);
	g_object_unref (priv->display_device);
	g_object_unref (priv->config);
	g_object_unref (priv->backend);

	G_OBJECT_CLASS (up_daemon_parent_class)->finalize (object);
}

/**
 * up_daemon_new:
 **/
UpDaemon *
up_daemon_new (void)
{
	return UP_DAEMON (g_object_new (UP_TYPE_DAEMON, NULL));
}
