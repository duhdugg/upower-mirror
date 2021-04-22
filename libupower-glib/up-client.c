/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008-2010 Richard Hughes <richard@hughsie.com>
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

/**
 * SECTION:up-client
 * @short_description: Main client object for accessing the UPower daemon
 * @see_also: #UpDevice
 *
 * A helper GObject to use for accessing UPower information, and to be notified
 * when it is changed.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <glib-object.h>

#include "up-client.h"
#include "up-daemon-generated.h"
#include "up-device.h"

static void	up_client_class_init		(UpClientClass	*klass);
static void	up_client_initable_iface_init	(GInitableIface *iface);
static void	up_client_init			(UpClient	*client);
static void	up_client_finalize		(GObject	*object);

/**
 * UpClientPrivate:
 *
 * Private #UpClient data
 **/
struct _UpClientPrivate
{
	UpExportedDaemon *proxy;
};

enum {
	UP_CLIENT_DEVICE_ADDED,
	UP_CLIENT_DEVICE_REMOVED,
	UP_CLIENT_LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_DAEMON_VERSION,
	PROP_ON_BATTERY,
	PROP_LAST
};

static guint signals [UP_CLIENT_LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE_WITH_CODE (UpClient, up_client, G_TYPE_OBJECT,
			 G_ADD_PRIVATE(UpClient)
                         G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, up_client_initable_iface_init))

/**
 * up_client_get_devices:
 * @client: a #UpClient instance.
 *
 * Get a copy of the device objects. This function does not set the free
 * function for the #GPtrArray so you need use g_object_unref on all
 * elements when you are finished with the array.
 *
 * Return value: (element-type UpDevice) (transfer full): an array of #UpDevice objects or %NULL on error, free with g_ptr_array_unref()
 *
 * Since: 0.9.0
 * Deprecated: 0.99.8
 **/
GPtrArray *
up_client_get_devices (UpClient *client)
{
	GPtrArray *array = up_client_get_devices2 (client);
	if (array)
		g_ptr_array_set_free_func (array, NULL);
	return array;
}

/**
 * up_client_get_devices2:
 * @client: a #UpClient instance.
 *
 * Get a copy of the device objects.
 *
 * Return value: (element-type UpDevice) (transfer full): an array of #UpDevice objects or %NULL on error, free with g_ptr_array_unref()
 *
 * Since: 0.99.8
 **/
GPtrArray *
up_client_get_devices2 (UpClient *client)
{
	GError *error = NULL;
	char **devices;
	GPtrArray *array;
	guint i;

	g_return_val_if_fail (UP_IS_CLIENT (client), NULL);

	if (up_exported_daemon_call_enumerate_devices_sync (client->priv->proxy,
							    &devices,
							    NULL,
							    &error) == FALSE) {
		g_warning ("up_client_get_devices failed: %s", error->message);
		g_error_free (error);
		return NULL;
	}

	array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

	for (i = 0; devices[i] != NULL; i++) {
		UpDevice *device;
		const char *object_path = devices[i];
		gboolean ret;

		device = up_device_new ();
		ret = up_device_set_object_path_sync (device, object_path, NULL, NULL);
		if (!ret) {
			g_object_unref (device);
			continue;
		}

		g_ptr_array_add (array, device);
	}
	g_strfreev (devices);

	return array;
}

/**
 * up_client_get_display_device:
 * @client: a #UpClient instance.
 *
 * Get the composite display device.
 * Return value: (transfer full): a #UpDevice object, or %NULL on error.
 *
 * Since: 1.0
 **/
UpDevice *
up_client_get_display_device (UpClient *client)
{
	gboolean ret;
	UpDevice *device;

	device = up_device_new ();
	ret = up_device_set_object_path_sync (device, "/org/freedesktop/UPower/devices/DisplayDevice", NULL, NULL);
	if (!ret) {
		g_object_unref (G_OBJECT (device));
		return NULL;
	}
	return device;
}

/**
 * up_client_get_critical_action:
 * @client: a #UpClient instance.
 *
 * Gets a string representing the configured critical action,
 * depending on availability.
 *
 * Return value: the action name, or %NULL on error.
 *
 * Since: 1.0
 **/
char *
up_client_get_critical_action (UpClient *client)
{
	char *action;

	g_return_val_if_fail (UP_IS_CLIENT (client), NULL);
	if (!up_exported_daemon_call_get_critical_action_sync (client->priv->proxy,
							       &action,
							       NULL, NULL)) {
		return NULL;
	}
	return action;
}

/**
 * up_client_get_daemon_version:
 * @client: a #UpClient instance.
 *
 * Get UPower daemon version.
 *
 * Return value: string containing the daemon version, e.g. 008
 *
 * Since: 0.9.0
 **/
const gchar *
up_client_get_daemon_version (UpClient *client)
{
	g_return_val_if_fail (UP_IS_CLIENT (client), NULL);
	return up_exported_daemon_get_daemon_version (client->priv->proxy);
}

/**
 * up_client_get_on_battery:
 * @client: a #UpClient instance.
 *
 * Get whether the system is running on battery power.
 *
 * Return value: %TRUE if the system is currently running on battery, %FALSE otherwise.
 *
 * Since: 0.9.0
 **/
gboolean
up_client_get_on_battery (UpClient *client)
{
	g_return_val_if_fail (UP_IS_CLIENT (client), FALSE);
	return up_exported_daemon_get_on_battery (client->priv->proxy);
}

/*
 * up_client_add:
 */
static void
up_client_add (UpClient *client, const gchar *object_path)
{
	UpDevice *device = NULL;
	gboolean ret;

	/* create new device */
	device = up_device_new ();
	ret = up_device_set_object_path_sync (device, object_path, NULL, NULL);
	if (!ret)
		goto out;

	/* add to array */
	g_signal_emit (client, signals [UP_CLIENT_DEVICE_ADDED], 0, device);
out:
	g_clear_object (&device);
}

/*
 * up_client_notify_cb:
 */
static void
up_client_notify_cb (GObject    *gobject,
		     GParamSpec *pspec,
		     UpClient   *client)
{
	/* Proxy the notification from the D-Bus glue object
	 * to the real one, but only if the property exists
	 * for UpClient */
	if (!g_object_class_find_property (G_OBJECT_GET_CLASS (client),
					  pspec->name))
		return;

	g_object_notify (G_OBJECT (client), pspec->name);
}

/*
 * up_client_added_cb:
 */
static void
up_device_added_cb (UpExportedDaemon *proxy, const gchar *object_path, UpClient *client)
{
	up_client_add (client, object_path);
}

/*
 * up_client_removed_cb:
 */
static void
up_device_removed_cb (UpExportedDaemon *proxy, const gchar *object_path, UpClient *client)
{
	g_signal_emit (client, signals [UP_CLIENT_DEVICE_REMOVED], 0, object_path);
}

static void
up_client_get_property (GObject *object,
			 guint prop_id,
			 GValue *value,
			 GParamSpec *pspec)
{
	UpClient *client;
	client = UP_CLIENT (object);

	if (client->priv->proxy == NULL)
                return;

	switch (prop_id) {
	case PROP_DAEMON_VERSION:
		g_value_set_string (value, up_exported_daemon_get_daemon_version (client->priv->proxy));
		break;
	case PROP_ON_BATTERY:
		g_value_set_boolean (value, up_exported_daemon_get_on_battery (client->priv->proxy));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*
 * up_client_class_init:
 * @klass: The UpClientClass
 */
static void
up_client_class_init (UpClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = up_client_get_property;
	object_class->finalize = up_client_finalize;

	/**
	 * UpClient:daemon-version:
	 *
	 * The daemon version.
	 *
	 * Since: 0.9.0
	 */
	g_object_class_install_property (object_class,
					 PROP_DAEMON_VERSION,
					 g_param_spec_string ("daemon-version",
							      "Daemon version",
							      NULL,
							      NULL,
							      G_PARAM_READABLE));
	/**
	 * UpClient:on-battery:
	 *
	 * If the computer is on battery power.
	 *
	 * Since: 0.9.0
	 */
	g_object_class_install_property (object_class,
					 PROP_ON_BATTERY,
					 g_param_spec_boolean ("on-battery",
							       "If the computer is on battery power",
							       NULL,
							       FALSE,
							       G_PARAM_READABLE));

	/**
	 * UpClient::device-added:
	 * @client: the #UpClient instance that emitted the signal
	 * @device: the #UpDevice that was added.
	 *
	 * The ::device-added signal is emitted when a power device is added.
	 *
	 * Since: 0.9.0
	 **/
	signals [UP_CLIENT_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpClientClass, device_added),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, UP_TYPE_DEVICE);

	/**
	 * UpClient::device-removed:
	 * @client: the #UpClient instance that emitted the signal
	 * @object_path: the object path of the #UpDevice that was removed.
	 *
	 * The ::device-removed signal is emitted when a power device is removed.
	 *
	 * Since: 1.0
	 **/
	signals [UP_CLIENT_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UpClientClass, device_removed),
			      NULL, NULL, g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);
}

/*
 * up_client_init:
 * @client: This class instance
 */
static gboolean
up_client_initable_init (GInitable *initable, GCancellable *cancellable, GError **error)
{
	UpClient *client = UP_CLIENT (initable);
	client->priv = up_client_get_instance_private (client);

	/* connect to main interface */
	client->priv->proxy = up_exported_daemon_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
									 G_DBUS_PROXY_FLAGS_NONE,
									 "org.freedesktop.UPower",
									 "/org/freedesktop/UPower",
									 cancellable,
									 error);
	if (client->priv->proxy == NULL)
		return FALSE;

	/* all callbacks */
	g_signal_connect (client->priv->proxy, "device-added",
			  G_CALLBACK (up_device_added_cb), client);
	g_signal_connect (client->priv->proxy, "device-removed",
			  G_CALLBACK (up_device_removed_cb), client);
	g_signal_connect (client->priv->proxy, "notify",
			  G_CALLBACK (up_client_notify_cb), client);

	return TRUE;
}

static void
up_client_initable_iface_init (GInitableIface *iface)
{
	iface->init = up_client_initable_init;
}

/*
 * up_client_init:
 * @client: This class instance
 */
static void
up_client_init (UpClient *client)
{
}

/*
 * up_client_finalize:
 */
static void
up_client_finalize (GObject *object)
{
	UpClient *client;

	g_return_if_fail (UP_IS_CLIENT (object));

	client = UP_CLIENT (object);

	g_clear_object (&client->priv->proxy);

	G_OBJECT_CLASS (up_client_parent_class)->finalize (object);
}

/**
 * up_client_new_full:
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Creates a new #UpClient object. If connecting to upowerd on D-Bus fails,
 * this returns %NULL and sets @error.
 *
 * Return value: a new UpClient object, or %NULL on failure.
 *
 * Since: 0.99.5
 **/
UpClient *
up_client_new_full (GCancellable *cancellable, GError **error)
{
	return g_initable_new (UP_TYPE_CLIENT, cancellable, error, NULL);
}

/**
 * up_client_new:
 *
 * Creates a new #UpClient object. If connecting to upowerd on D-Bus fails,
 * this returns %NULL and prints out a warning with the error message.
 * Consider using up_client_new_full() instead which allows you to handle errors
 * and cancelling long operations yourself.
 *
 * Return value: a new UpClient object, or %NULL on failure.
 *
 * Since: 0.9.0
 **/
UpClient *
up_client_new (void)
{
	GError *error = NULL;
	UpClient *client;
	client = up_client_new_full (NULL, &error);
	if (client == NULL) {
		g_warning ("Couldn't connect to proxy: %s", error->message);
		g_error_free (error);
	}
	return client;
}

