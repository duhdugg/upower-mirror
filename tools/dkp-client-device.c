/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib.h>

#include "dkp-debug.h"
#include "dkp-client-device.h"
#include "dkp-object.h"
#include "dkp-history-obj.h"

static void	dkp_client_device_class_init	(DkpClientDeviceClass	*klass);
static void	dkp_client_device_init		(DkpClientDevice	*device);
static void	dkp_client_device_finalize	(GObject		*object);

#define DKP_CLIENT_DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_CLIENT_DEVICE, DkpClientDevicePrivate))

struct DkpClientDevicePrivate
{
	gchar			*object_path;
	DkpObject		*obj;
	DBusGConnection		*bus;
	DBusGProxy		*proxy_source;
	DBusGProxy		*proxy_props;
};

enum {
	DKP_CLIENT_DEVICE_CHANGED,
	DKP_CLIENT_DEVICE_LAST_SIGNAL
};

static guint signals [DKP_CLIENT_DEVICE_LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (DkpClientDevice, dkp_client_device, G_TYPE_OBJECT)

/**
 * dkp_client_device_get_device_properties:
 **/
static GHashTable *
dkp_client_device_get_device_properties (DkpClientDevice *device)
{
	gboolean ret;
	GError *error = NULL;
	GHashTable *hash_table = NULL;

	ret = dbus_g_proxy_call (device->priv->proxy_props, "GetAll", &error,
				 G_TYPE_STRING, "org.freedesktop.DeviceKit.Power.Device",
				 G_TYPE_INVALID,
				 dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE),
				 &hash_table,
				 G_TYPE_INVALID);
	if (!ret) {
		dkp_debug ("Couldn't call GetAll() to get properties for %s: %s", device->priv->object_path, error->message);
		g_error_free (error);
		goto out;
	}
out:
	return hash_table;
}

/**
 * dkp_client_device_refresh_internal:
 **/
static gboolean
dkp_client_device_refresh_internal (DkpClientDevice *device)
{
	GHashTable *hash;

	/* get all the properties */
	hash = dkp_client_device_get_device_properties (device);
	if (hash == NULL) {
		dkp_warning ("Cannot get device properties for %s", device->priv->object_path);
		return FALSE;
	}
	dkp_object_set_from_map (device->priv->obj, hash);
	g_hash_table_unref (hash);
	return TRUE;
}

/**
 * dkp_client_device_changed_cb:
 **/
static void
dkp_client_device_changed_cb (DBusGProxy *proxy, DkpClientDevice *device)
{
	g_return_if_fail (DKP_IS_CLIENT_DEVICE (device));
	dkp_client_device_refresh_internal (device);
	g_signal_emit (device, signals [DKP_CLIENT_DEVICE_CHANGED], 0, device->priv->obj);
}

/**
 * dkp_client_device_set_object_path:
 **/
gboolean
dkp_client_device_set_object_path (DkpClientDevice *device, const gchar *object_path)
{
	GError *error = NULL;
	gboolean ret = FALSE;
	DBusGProxy *proxy_source;
	DBusGProxy *proxy_props;

	g_return_val_if_fail (DKP_IS_CLIENT_DEVICE (device), FALSE);

	if (device->priv->object_path != NULL)
		return FALSE;
	if (object_path == NULL)
		return FALSE;

	/* connect to the bus */
	device->priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (device->priv->bus == NULL) {
		dkp_warning ("Couldn't connect to system bus: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* connect to the correct path for properties */
	proxy_props = dbus_g_proxy_new_for_name (device->priv->bus, "org.freedesktop.DeviceKit.Power",
						 object_path, "org.freedesktop.DBus.Properties");
	if (proxy_props == NULL) {
		dkp_warning ("Couldn't connect to proxy");
		goto out;
	}

	/* connect to the correct path for all the other methods */
	proxy_source = dbus_g_proxy_new_for_name (device->priv->bus, "org.freedesktop.DeviceKit.Power",
						  object_path, "org.freedesktop.DeviceKit.Power.Source");
	if (proxy_source == NULL) {
		dkp_warning ("Couldn't connect to proxy");
		goto out;
	}

	/* listen to Changed */
	dbus_g_proxy_add_signal (proxy_source, "Changed", G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (proxy_source, "Changed",
				     G_CALLBACK (dkp_client_device_changed_cb), device, NULL);

	/* yay */
	dkp_debug ("using object_path: %s", object_path);
	device->priv->proxy_source = proxy_source;
	device->priv->proxy_props = proxy_props;
	device->priv->object_path = g_strdup (object_path);

	/* coldplug */
	ret = dkp_client_device_refresh_internal (device);
	if (!ret)
		dkp_warning ("cannot refresh");
out:
	return ret;
}

/**
 * dkp_client_device_get_object_path:
 **/
const gchar *
dkp_client_device_get_object_path (const DkpClientDevice *device)
{
	g_return_val_if_fail (DKP_IS_CLIENT_DEVICE (device), NULL);
	return device->priv->object_path;
}

/**
 * dkp_client_device_get_object:
 **/
const DkpObject *
dkp_client_device_get_object (const DkpClientDevice *device)
{
	g_return_val_if_fail (DKP_IS_CLIENT_DEVICE (device), NULL);
	return device->priv->obj;
}

/**
 * dkp_client_device_print:
 **/
gboolean
dkp_client_device_print (const DkpClientDevice *device)
{
	g_return_val_if_fail (DKP_IS_CLIENT_DEVICE (device), FALSE);

	/* print to screen */
	dkp_object_print (device->priv->obj);

	/* if we can, get stats */
	dkp_client_device_get_statistics (device, "charge", 120);
	dkp_client_device_get_statistics (device, "rate", 120);
	return TRUE;
}

/**
 * dkp_client_device_refresh:
 **/
gboolean
dkp_client_device_refresh (DkpClientDevice *device)
{
	GError *error = NULL;
	gboolean ret;

	g_return_val_if_fail (DKP_IS_CLIENT_DEVICE (device), FALSE);
	g_return_val_if_fail (device->priv->proxy_source != NULL, FALSE);

	/* just refresh the device */
	ret = dbus_g_proxy_call (device->priv->proxy_source, "Refresh", &error,
				 G_TYPE_INVALID, G_TYPE_INVALID);
	if (!ret) {
		dkp_debug ("Refresh() on %s failed: %s", device->priv->object_path, error->message);
		g_error_free (error);
		goto out;
	}
out:
	return ret;
}

/**
 * dkp_client_device_get_statistics:
 *
 * Returns an array of %DkpHistoryObj's
 **/
GPtrArray *
dkp_client_device_get_statistics (const DkpClientDevice *device, const gchar *type, guint timespec)
{
	GError *error = NULL;
	GType g_type_gvalue_array;
	GPtrArray *gvalue_ptr_array = NULL;
	GValueArray *gva;
	GValue *gv;
	guint i;
	DkpHistoryObj *obj;
	GPtrArray *array = NULL;
	gboolean ret;

	g_return_val_if_fail (DKP_IS_CLIENT_DEVICE (device), FALSE);

	g_type_gvalue_array = dbus_g_type_get_collection ("GPtrArray",
					dbus_g_type_get_struct("GValueArray",
						G_TYPE_UINT,
						G_TYPE_DOUBLE,
						G_TYPE_STRING,
						G_TYPE_INVALID));

	/* get compound data */
	ret = dbus_g_proxy_call (device->priv->proxy_source, "GetStatistics", &error,
				 G_TYPE_STRING, type,
				 G_TYPE_UINT, timespec,
				 G_TYPE_INVALID,
				 g_type_gvalue_array, &gvalue_ptr_array,
				 G_TYPE_INVALID);
	if (!ret) {
		dkp_debug ("GetStatistics(%s,%i) on %s failed: %s", type, timespec,
			   device->priv->object_path, error->message);
		g_error_free (error);
		goto out;
	}

	/* no data */
	if (gvalue_ptr_array->len == 0)
		goto out;

	/* convert */
	array = g_ptr_array_sized_new (gvalue_ptr_array->len);
	for (i=0; i<gvalue_ptr_array->len; i++) {
		gva = (GValueArray *) g_ptr_array_index (gvalue_ptr_array, i);
		obj = dkp_history_obj_new ();
		/* 0 */
		gv = g_value_array_get_nth (gva, 0);
		obj->time = g_value_get_uint (gv);
		g_value_unset (gv);
		/* 1 */
		gv = g_value_array_get_nth (gva, 1);
		obj->value = g_value_get_double (gv);
		g_value_unset (gv);
		/* 2 */
		gv = g_value_array_get_nth (gva, 2);
		obj->state = dkp_source_state_from_text (g_value_get_string (gv));
		g_value_unset (gv);
		g_ptr_array_add (array, obj);
		g_value_array_free (gva);
	}

out:
	if (gvalue_ptr_array != NULL)
		g_ptr_array_free (gvalue_ptr_array, TRUE);
	return array;
}

/**
 * dkp_client_device_class_init:
 * @klass: The DkpClientDeviceClass
 **/
static void
dkp_client_device_class_init (DkpClientDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_client_device_finalize;

	/**
	 * PkClient::changed:
	 * @device: the #DkpClientDevice instance that emitted the signal
	 * @obj: the #DkpObject that has changed
	 *
	 * The ::changed signal is emitted when the device data has changed.
	 **/
	signals [DKP_CLIENT_DEVICE_CHANGED] =
		g_signal_new ("changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpClientDeviceClass, changed),
			      NULL, NULL, g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1, G_TYPE_POINTER);

	g_type_class_add_private (klass, sizeof (DkpClientDevicePrivate));
}

/**
 * dkp_client_device_init:
 * @client_device: This class instance
 **/
static void
dkp_client_device_init (DkpClientDevice *device)
{
	device->priv = DKP_CLIENT_DEVICE_GET_PRIVATE (device);
	device->priv->object_path = NULL;
	device->priv->proxy_source = NULL;
	device->priv->proxy_props = NULL;
	device->priv->obj = dkp_object_new ();
}

/**
 * dkp_client_device_finalize:
 * @object: The object to finalize
 **/
static void
dkp_client_device_finalize (GObject *object)
{
	DkpClientDevice *device;

	g_return_if_fail (DKP_IS_CLIENT_DEVICE (object));

	device = DKP_CLIENT_DEVICE (object);

	g_free (device->priv->object_path);
	dkp_object_free (device->priv->obj);
	if (device->priv->proxy_source != NULL)
		g_object_unref (device->priv->proxy_source);
	if (device->priv->proxy_props != NULL)
		g_object_unref (device->priv->proxy_props);
	dbus_g_connection_unref (device->priv->bus);

	G_OBJECT_CLASS (dkp_client_device_parent_class)->finalize (object);
}

/**
 * dkp_client_device_new:
 *
 * Return value: a new DkpClientDevice object.
 **/
DkpClientDevice *
dkp_client_device_new (void)
{
	DkpClientDevice *device;
	device = g_object_new (DKP_TYPE_CLIENT_DEVICE, NULL);
	return DKP_CLIENT_DEVICE (device);
}

