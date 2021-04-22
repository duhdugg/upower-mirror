/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <sys/wait.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gudev/gudev.h>

#include "up-backend.h"
#include "up-backend-linux-private.h"
#include "up-daemon.h"
#include "up-device.h"

#include "up-device-supply.h"
#include "up-device-wup.h"
#include "up-device-hid.h"
#include "up-device-bluez.h"
#include "up-config.h"
#ifdef HAVE_IDEVICE
#include "up-device-idevice.h"
#endif /* HAVE_IDEVICE */

static void	up_backend_class_init	(UpBackendClass	*klass);
static void	up_backend_init	(UpBackend		*backend);
static void	up_backend_finalize	(GObject		*object);

#define LOGIND_DBUS_NAME                       "org.freedesktop.login1"
#define LOGIND_DBUS_PATH                       "/org/freedesktop/login1"
#define LOGIND_DBUS_INTERFACE                  "org.freedesktop.login1.Manager"

struct UpBackendPrivate
{
	UpDaemon		*daemon;
	UpDeviceList		*device_list;
	GUdevClient		*gudev_client;
	UpDeviceList		*managed_devices;
	UpConfig		*config;
	GDBusProxy		*logind_proxy;
	guint                    logind_sleep_id;
	int                      logind_delay_inhibitor_fd;

	/* BlueZ */
	guint			 bluez_watch_id;
	GDBusObjectManager	*bluez_client;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE_WITH_PRIVATE (UpBackend, up_backend, G_TYPE_OBJECT)

static gboolean up_backend_device_add (UpBackend *backend, GUdevDevice *native);
static void up_backend_device_remove (UpBackend *backend, GUdevDevice *native);

static gpointer
is_macbook (gpointer data)
{
	g_autofree char *product = NULL;

	if (!g_file_get_contents ("/sys/devices/virtual/dmi/id/product_name", &product, NULL, NULL) ||
	    product == NULL)
		return GINT_TO_POINTER(FALSE);
	return GINT_TO_POINTER(g_str_has_prefix (product, "MacBook"));
}

gboolean
up_backend_needs_poll_after_uevent (void)
{
	static GOnce dmi_once = G_ONCE_INIT;
	g_once (&dmi_once, is_macbook, NULL);
	return GPOINTER_TO_INT(dmi_once.retval);
}

static UpDevice *
up_backend_device_new (UpBackend *backend, GUdevDevice *native)
{
	const gchar *subsys;
	const gchar *native_path;
	UpDevice *device = NULL;
	gboolean ret;

	subsys = g_udev_device_get_subsystem (native);
	if (g_strcmp0 (subsys, "power_supply") == 0) {

		/* are we a valid power supply */
		device = UP_DEVICE (up_device_supply_new ());
		g_object_set (G_OBJECT(device),
			      "ignore-system-percentage", GPOINTER_TO_INT (is_macbook (NULL)),
			      NULL);
		ret = up_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;

		/* no valid power supply object */
		g_clear_object (&device);

	} else if (g_strcmp0 (subsys, "tty") == 0) {

		/* see if this is a Watts Up Pro device */
		device = UP_DEVICE (up_device_wup_new ());
		ret = up_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;

		/* no valid TTY object */
		g_clear_object (&device);

	} else if (g_strcmp0 (subsys, "usb") == 0 || g_strcmp0 (subsys, "usbmisc") == 0) {

#ifdef HAVE_IDEVICE
		/* see if this is an iDevice */
		device = UP_DEVICE (up_device_idevice_new ());
		ret = up_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;
		g_object_unref (device);
#endif /* HAVE_IDEVICE */

		/* try to detect a HID UPS */
		device = UP_DEVICE (up_device_hid_new ());
		ret = up_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;

		/* no valid USB object */
		g_clear_object (&device);
	} else {
		native_path = g_udev_device_get_sysfs_path (native);
		g_warning ("native path %s (%s) ignoring", native_path, subsys);
	}
out:
	return device;
}

static void
up_backend_device_changed (UpBackend *backend, GUdevDevice *native)
{
	GObject *object;
	UpDevice *device;
	gboolean ret;

	/* first, check the device and add it if it doesn't exist */
	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	if (object == NULL) {
		g_warning ("treating change event as add on %s", g_udev_device_get_sysfs_path (native));
		up_backend_device_add (backend, native);
		goto out;
	}

	/* need to refresh device */
	device = UP_DEVICE (object);
	ret = up_device_refresh_internal (device);
	if (!ret) {
		g_debug ("no changes on %s", up_device_get_object_path (device));
		goto out;
	}
out:
	g_clear_object (&object);
}

static gboolean
up_backend_device_add (UpBackend *backend, GUdevDevice *native)
{
	GObject *object;
	UpDevice *device;
	gboolean ret = TRUE;

	/* does device exist in db? */
	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	if (object != NULL) {
		device = UP_DEVICE (object);
		/* we already have the device; treat as change event */
		g_warning ("treating add event as change event on %s", up_device_get_object_path (device));
		up_backend_device_changed (backend, native);
		goto out;
	}

	/* get the right sort of device */
	device = up_backend_device_new (backend, native);
	if (device == NULL) {
		ret = FALSE;
		goto out;
	}

	/* emit */
	g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, native, device);
out:
	g_clear_object (&object);
	return ret;
}

static void
up_backend_device_remove (UpBackend *backend, GUdevDevice *native)
{
	GObject *object;
	UpDevice *device;

	/* does device exist in db? */
	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	if (object == NULL) {
		g_debug ("ignoring remove event on %s", g_udev_device_get_sysfs_path (native));
		goto out;
	}

	device = UP_DEVICE (object);
	/* emit */
	g_debug ("emitting device-removed: %s", g_udev_device_get_sysfs_path (native));
	g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, native, device);

out:
	g_clear_object (&object);
}

static void
up_backend_uevent_signal_handler_cb (GUdevClient *client, const gchar *action,
				      GUdevDevice *device, gpointer user_data)
{
	UpBackend *backend = UP_BACKEND (user_data);

	if (g_strcmp0 (action, "add") == 0) {
		g_debug ("SYSFS add %s", g_udev_device_get_sysfs_path (device));
		up_backend_device_add (backend, device);
	} else if (g_strcmp0 (action, "remove") == 0) {
		g_debug ("SYSFS remove %s", g_udev_device_get_sysfs_path (device));
		up_backend_device_remove (backend, device);
	} else if (g_strcmp0 (action, "change") == 0) {
		g_debug ("SYSFS change %s", g_udev_device_get_sysfs_path (device));
		up_backend_device_changed (backend, device);
	} else {
		g_debug ("unhandled action '%s' on %s", action, g_udev_device_get_sysfs_path (device));
	}
}

static gboolean
is_battery_iface_proxy (GDBusProxy *interface_proxy)
{
	const char *iface;

	iface = g_dbus_proxy_get_interface_name (interface_proxy);
	return g_str_equal (iface, "org.bluez.Battery1");
}

static gboolean
has_battery_iface (GDBusObject *object)
{
	GDBusInterface *iface;

	iface = g_dbus_object_get_interface (object, "org.bluez.Battery1");
	if (!iface)
		return FALSE;
	g_object_unref (iface);
	return TRUE;
}

static void
bluez_proxies_changed (GDBusObjectManagerClient *manager,
		       GDBusObjectProxy         *object_proxy,
		       GDBusProxy               *interface_proxy,
		       GVariant                 *changed_properties,
		       GStrv                     invalidated_properties,
		       gpointer                  user_data)
{
	UpBackend *backend = user_data;
	GObject *object;
	UpDeviceBluez *bluez;

	if (!is_battery_iface_proxy (interface_proxy))
		return;

	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (object_proxy));
	if (!object)
		return;

	bluez = UP_DEVICE_BLUEZ (object);
	up_device_bluez_update (bluez, changed_properties);
	g_object_unref (object);
}

static void
bluez_interface_removed (GDBusObjectManager *manager,
			 GDBusObject        *bus_object,
			 GDBusInterface     *interface,
			 gpointer            user_data)
{
	UpBackend *backend = user_data;
	GObject *object;

	/* It might be another iface on another device that got removed */
	if (has_battery_iface (bus_object))
		return;

	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (bus_object));
	if (!object)
		return;

	g_debug ("emitting device-removed: %s", g_dbus_object_get_object_path (bus_object));
	g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, bus_object, UP_DEVICE (object));

	g_object_unref (object);
}

static void
bluez_interface_added (GDBusObjectManager *manager,
		       GDBusObject        *bus_object,
		       GDBusInterface     *interface,
		       gpointer            user_data)
{
	UpBackend *backend = user_data;
	UpDevice *device;
	GObject *object;
	gboolean ret;

	if (!has_battery_iface (bus_object))
		return;

	object = up_device_list_lookup (backend->priv->device_list, G_OBJECT (bus_object));
	if (object != NULL) {
		g_object_unref (object);
		return;
	}

	device = UP_DEVICE (up_device_bluez_new ());
	ret = up_device_coldplug (device, backend->priv->daemon, G_OBJECT (bus_object));
	if (!ret) {
		g_object_unref (device);
		return;
	}

	g_debug ("emitting device-added: %s", g_dbus_object_get_object_path (bus_object));
	g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, bus_object, device);
}

static void
bluez_appeared (GDBusConnection *connection,
		const gchar     *name,
		const gchar     *name_owner,
		gpointer         user_data)
{
	UpBackend *backend = user_data;
	GError *error = NULL;
	GList *objects, *l;

	g_assert (backend->priv->bluez_client == NULL);

	backend->priv->bluez_client = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
										     G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
										     "org.bluez",
										     "/",
										     NULL, NULL, NULL,
										     NULL, &error);
	if (!backend->priv->bluez_client) {
		g_warning ("Failed to create object manager for BlueZ: %s",
			   error->message);
		g_error_free (error);
		return;
	}

	g_debug ("BlueZ appeared");

	g_signal_connect (backend->priv->bluez_client, "interface-proxy-properties-changed",
			  G_CALLBACK (bluez_proxies_changed), backend);
	g_signal_connect (backend->priv->bluez_client, "interface-removed",
			  G_CALLBACK (bluez_interface_removed), backend);
	g_signal_connect (backend->priv->bluez_client, "interface-added",
			  G_CALLBACK (bluez_interface_added), backend);

	objects = g_dbus_object_manager_get_objects (backend->priv->bluez_client);
	for (l = objects; l != NULL; l = l->next) {
		GDBusObject *object = l->data;
		GList *interfaces, *k;

		interfaces = g_dbus_object_get_interfaces (object);

		for (k = interfaces; k != NULL; k = k->next) {
			GDBusInterface *iface = k->data;

			bluez_interface_added (backend->priv->bluez_client,
					       object,
					       iface,
					       backend);
			g_object_unref (iface);
		}
		g_list_free (interfaces);
		g_object_unref (object);
	}
	g_list_free (objects);
}

static void
bluez_vanished (GDBusConnection *connection,
		const gchar     *name,
		gpointer         user_data)
{
	UpBackend *backend = user_data;
	GPtrArray *array;
	guint i;

	g_debug ("BlueZ disappeared");

	array = up_device_list_get_array (backend->priv->device_list);

	for (i = 0; i < array->len; i++) {
		UpDevice *device = UP_DEVICE (g_ptr_array_index (array, i));
		if (UP_IS_DEVICE_BLUEZ (device)) {
			GDBusObject *object;

			object = G_DBUS_OBJECT (up_device_get_native (device));
			g_debug ("emitting device-removed: %s", g_dbus_object_get_object_path (object));
			g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, object, UP_DEVICE (object));
		}
	}

	g_ptr_array_unref (array);

	g_clear_object (&backend->priv->bluez_client);
}

/**
 * up_backend_coldplug:
 * @backend: The %UpBackend class instance
 * @daemon: The %UpDaemon controlling instance
 *
 * Finds all the devices already plugged in, and emits device-add signals for
 * each of them.
 *
 * Return value: %TRUE for success
 **/
gboolean
up_backend_coldplug (UpBackend *backend, UpDaemon *daemon)
{
	GUdevDevice *native;
	GList *devices;
	GList *l;
	guint i;
	const gchar *subsystems_wup[] = {"power_supply", "usb", "usbmisc", "tty", NULL};
	const gchar *subsystems[] = {"power_supply", "usb", "usbmisc", NULL};

	backend->priv->daemon = g_object_ref (daemon);
	backend->priv->device_list = up_daemon_get_device_list (daemon);
	if (up_config_get_boolean (backend->priv->config, "EnableWattsUpPro"))
		backend->priv->gudev_client = g_udev_client_new (subsystems_wup);
	else
		backend->priv->gudev_client = g_udev_client_new (subsystems);
	g_signal_connect (backend->priv->gudev_client, "uevent",
			  G_CALLBACK (up_backend_uevent_signal_handler_cb), backend);

	/* add all subsystems */
	for (i=0; subsystems[i] != NULL; i++) {
		g_debug ("registering subsystem : %s", subsystems[i]);
		devices = g_udev_client_query_by_subsystem (backend->priv->gudev_client, subsystems[i]);
		for (l = devices; l != NULL; l = l->next) {
			native = l->data;
			up_backend_device_add (backend, native);
		}
		g_list_free_full (devices, (GDestroyNotify) g_object_unref);
	}

	backend->priv->bluez_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
							  "org.bluez",
							  G_BUS_NAME_WATCHER_FLAGS_NONE,
							  bluez_appeared,
							  bluez_vanished,
							  backend,
							  NULL);

	return TRUE;
}

/**
 * up_backend_unplug:
 * @backend: The %UpBackend class instance
 *
 * Forget about all learned devices, effectively undoing up_backend_coldplug.
 * Resources are released without emitting signals.
 */
void
up_backend_unplug (UpBackend *backend)
{
	g_clear_object (&backend->priv->gudev_client);
	g_clear_object (&backend->priv->device_list);
	/* set in init, clear the list to remove reference to UpDaemon */
	if (backend->priv->managed_devices != NULL)
		up_device_list_clear (backend->priv->managed_devices, FALSE);
	g_clear_object (&backend->priv->daemon);
	if (backend->priv->bluez_watch_id > 0) {
		g_bus_unwatch_name (backend->priv->bluez_watch_id);
		backend->priv->bluez_watch_id = 0;
	}
	g_clear_object (&backend->priv->bluez_client);
}

static gboolean
check_action_result (GVariant *result)
{
	if (result) {
		const char *s;

		g_variant_get (result, "(&s)", &s);
		if (g_strcmp0 (s, "yes") == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * up_backend_get_critical_action:
 * @backend: The %UpBackend class instance
 *
 * Which action will be taken when %UP_DEVICE_LEVEL_ACTION
 * warning-level occurs.
 **/
const char *
up_backend_get_critical_action (UpBackend *backend)
{
	struct {
		const gchar *method;
		const gchar *can_method;
	} actions[] = {
		{ "HybridSleep", "CanHybridSleep" },
		{ "Hibernate", "CanHibernate" },
		{ "PowerOff", NULL },
	};
	guint i = 0;
	char *action;

	g_return_val_if_fail (backend->priv->logind_proxy != NULL, NULL);

	/* Find the configured action first */
	action = up_config_get_string (backend->priv->config, "CriticalPowerAction");
	if (action != NULL) {
		for (i = 0; i < G_N_ELEMENTS (actions); i++)
			if (g_str_equal (actions[i].method, action))
				break;
		if (i >= G_N_ELEMENTS (actions))
			i = 0;
		g_free (action);
	}

	for (; i < G_N_ELEMENTS (actions); i++) {
		GVariant *result;

		if (actions[i].can_method) {
			gboolean action_available;

			/* Check whether we can use the method */
			result = g_dbus_proxy_call_sync (backend->priv->logind_proxy,
							 actions[i].can_method,
							 NULL,
							 G_DBUS_CALL_FLAGS_NONE,
							 -1, NULL, NULL);
			action_available = check_action_result (result);
			g_variant_unref (result);

			if (!action_available)
				continue;
		}

		return actions[i].method;
	}
	g_assert_not_reached ();
}

/**
 * up_backend_take_action:
 * @backend: The %UpBackend class instance
 *
 * Act upon the %UP_DEVICE_LEVEL_ACTION warning-level.
 **/
void
up_backend_take_action (UpBackend *backend)
{
	const char *method;

	method = up_backend_get_critical_action (backend);
	g_assert (method != NULL);

	/* Take action */
	g_debug ("About to call logind method %s", method);
	g_dbus_proxy_call (backend->priv->logind_proxy,
			   method,
			   g_variant_new ("(b)", FALSE),
			   G_DBUS_CALL_FLAGS_NONE,
			   G_MAXINT,
			   NULL,
			   NULL,
			   NULL);
}

/**
 * up_backend_inhibitor_lock_take:
 * @backend: The %UpBackend class instance
 * @reason: Why the inhibitor lock is taken
 * @mode: The mode of the lock ('delay' or 'block')
 *
 * Acquire a sleep inhibitor lock via systemd's logind that will
 * inhibit going to sleep until the lock is released again by
 * closing the file descriptor.
 */
int
up_backend_inhibitor_lock_take (UpBackend  *backend,
                                const char *reason,
                                const char *mode)
{
	GVariant *out, *input;
	GUnixFDList *fds = NULL;
	int fd;
	GError *error = NULL;

	g_return_val_if_fail (reason != NULL, -1);
	g_return_val_if_fail (mode != NULL, -1);
	g_return_val_if_fail (g_str_equal (mode, "delay") || g_str_equal (mode, "block"), -1);

	input = g_variant_new ("(ssss)",
			       "sleep",  /* what */
			       "UPower", /* who */
			       reason,   /* why */
			       mode);    /* mode */

	out = g_dbus_proxy_call_with_unix_fd_list_sync (backend->priv->logind_proxy,
							"Inhibit",
							input,
							G_DBUS_CALL_FLAGS_NONE,
							-1,
							NULL,
							&fds,
							NULL,
							&error);
	if (out == NULL) {
		g_warning ("Could not acquire inhibitor lock: %s",
			   error ? error->message : "Unknown reason");
		g_clear_error (&error);
		return -1;
	}

	if (g_unix_fd_list_get_length (fds) != 1) {
		g_warning ("Unexpected values returned by logind's 'Inhibit'");
		g_variant_unref (out);
		g_object_unref (fds);
		return -1;
	}

	fd = g_unix_fd_list_get (fds, 0, NULL);

	g_variant_unref (out);
	g_object_unref (fds);

	g_debug ("Acquired inhibitor lock (%i, %s)", fd, mode);

	return fd;
}

/**
 * up_backend_prepare_for_sleep:
 *
 * Callback for logind's PrepareForSleep signal. It receives
 * a boolean that indicates if we are about to sleep (TRUE)
 * or waking up (FALSE).
 * In case of the waking up we refresh the devices so we are
 * up to date, especially w.r.t. battery levels, since they
 * might have changed drastically.
 **/
static void
up_backend_prepare_for_sleep (GDBusConnection *connection,
			      const gchar     *sender_name,
			      const gchar     *object_path,
			      const gchar     *interface_name,
			      const gchar     *signal_name,
			      GVariant        *parameters,
			      gpointer         user_data)
{
	UpBackend *backend = user_data;
	gboolean will_sleep;
	GPtrArray *array;
	guint i;

	if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(b)"))) {
		g_warning ("logind PrepareForSleep has unexpected parameter(s)");
		return;
	}

	g_variant_get (parameters, "(b)", &will_sleep);

	if (will_sleep) {
		up_daemon_pause_poll (backend->priv->daemon);
		if (backend->priv->logind_delay_inhibitor_fd > 0) {
			close (backend->priv->logind_delay_inhibitor_fd);
			backend->priv->logind_delay_inhibitor_fd = 0;
		}
		return;
	}

	if (backend->priv->logind_delay_inhibitor_fd < 0)
		backend->priv->logind_delay_inhibitor_fd = up_backend_inhibitor_lock_take (backend, "Pause device polling", "delay");

	/* we are waking up, lets refresh all battery devices */
	g_debug ("Woke up from sleep; about to refresh devices");
	array = up_device_list_get_array (backend->priv->device_list);

	for (i = 0; i < array->len; i++) {
		UpDevice *device = UP_DEVICE (g_ptr_array_index (array, i));
		up_device_refresh_internal (device);
	}

	g_ptr_array_unref (array);

	up_daemon_resume_poll (backend->priv->daemon);
}


static void
up_backend_class_init (UpBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_backend_finalize;

	signals [SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpBackendClass, device_added),
			      NULL, NULL, NULL,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
	signals [SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpBackendClass, device_removed),
			      NULL, NULL, NULL,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
}

static void
up_backend_init (UpBackend *backend)
{
	GDBusConnection *bus;
	guint sleep_id;

	backend->priv = up_backend_get_instance_private (backend);
	backend->priv->config = up_config_new ();
	backend->priv->managed_devices = up_device_list_new ();
	backend->priv->logind_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
								     0,
								     NULL,
								     LOGIND_DBUS_NAME,
								     LOGIND_DBUS_PATH,
								     LOGIND_DBUS_INTERFACE,
								     NULL,
								     NULL);

	bus = g_dbus_proxy_get_connection (backend->priv->logind_proxy);
	sleep_id = g_dbus_connection_signal_subscribe (bus,
						       LOGIND_DBUS_NAME,
						       LOGIND_DBUS_INTERFACE,
						       "PrepareForSleep",
						       LOGIND_DBUS_PATH,
						       NULL,
						       G_DBUS_SIGNAL_FLAGS_NONE,
						       up_backend_prepare_for_sleep,
						       backend,
						       NULL);
	backend->priv->logind_sleep_id = sleep_id;
	backend->priv->logind_delay_inhibitor_fd = -1;

	backend->priv->logind_delay_inhibitor_fd = up_backend_inhibitor_lock_take (backend, "Pause device polling", "delay");
}

static void
up_backend_finalize (GObject *object)
{
	UpBackend *backend;
	GDBusConnection *bus;

	g_return_if_fail (UP_IS_BACKEND (object));

	backend = UP_BACKEND (object);

	if (backend->priv->bluez_watch_id > 0) {
		g_bus_unwatch_name (backend->priv->bluez_watch_id);
		backend->priv->bluez_watch_id = 0;
	}
	g_clear_object (&backend->priv->bluez_client);

	g_clear_object (&backend->priv->config);
	g_clear_object (&backend->priv->daemon);
	g_clear_object (&backend->priv->device_list);
	g_clear_object (&backend->priv->gudev_client);

	bus = g_dbus_proxy_get_connection (backend->priv->logind_proxy);
	g_dbus_connection_signal_unsubscribe (bus,
					      backend->priv->logind_sleep_id);

	if (backend->priv->logind_delay_inhibitor_fd >= 0)
		close (backend->priv->logind_delay_inhibitor_fd);

	g_clear_object (&backend->priv->logind_proxy);

	g_object_unref (backend->priv->managed_devices);

	G_OBJECT_CLASS (up_backend_parent_class)->finalize (object);
}

/**
 * up_backend_new:
 *
 * Return value: a new %UpBackend object.
 **/
UpBackend *
up_backend_new (void)
{
	return g_object_new (UP_TYPE_BACKEND, NULL);
}

