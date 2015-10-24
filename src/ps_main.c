/*
 * tel-plugin-packetservice
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: DongHoo Park <donghoo.park@samsung.com>
 *			Arun Shukla <arun.shukla@samsung.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>

#include <glib.h>

#include <tcore.h>
#include <plugin.h>

#include "ps_main.h"
#include <ps_common.h>

/*
 * PS plugin Private information
 */
typedef struct {
	GDBusConnection *conn; /* DBUS connection */
	guint bus_id; /* Packet service BUS ID */

	/* Parent plug-in */
	TcorePlugin *p;

	/* List of masters */
	GSList *master;
} PsPrivInfo;

static void __packet_service_cleanup(PsPrivInfo *priv_info)
{
	/* Sanity Check */
	if (priv_info == NULL)
		return;

	/* Cleaning up the master list */
	g_slist_foreach(priv_info->master,
		__remove_master, NULL);

	/* Unowning the Gdbus */
	g_bus_unown_name(priv_info->bus_id);

	/* Free GDBusConnection */
	g_object_unref(priv_info->conn);

	/*
	 * Freeing the memory allocated to the
	 * custom data for Packet Service
	 */
	g_free(priv_info);
}

static void on_bus_acquired(GDBusConnection *conn,
	const gchar *name, gpointer user_data)
{
	gboolean rv = FALSE;
	gpointer *master = NULL;

	TcorePlugin *p = user_data;
	PsPrivInfo *priv_info = tcore_plugin_ref_user_data(p);

	dbg("Bus is acquired: [%s]", name);

	/*
	 * Create 'master'
	 */
	master = _ps_master_create_master(conn, p);
	if (!master) {
		err("Failed to create 'master' Object for Packet Service");
		goto FAILURE;
	}

	priv_info->master = g_slist_append(priv_info->master, master);

	/*
	 * Create and initialize 'modem(s)'
	 */
	rv = _ps_master_create_modems(master, NULL);
	if (!rv) {
		dbg("Failure : Modem creation Failed ");
		goto FAILURE;
	}

	dbg("Packet Service plugin initialization: [Successful]");

	return;

FAILURE:
	ps_main_exit(p);
}

/*
 * Packet service initializer
 */
gboolean ps_main_init(TcorePlugin *p)
{
	PsPrivInfo *priv_info = NULL;
	GDBusConnection *conn = NULL;
	gchar *address = NULL;
	guint id;

	GError *error = NULL;
	gboolean rv = FALSE;

	if (!p)
		return FALSE;

	/*
	 * Initialize context
	 */
	rv = _ps_context_initialize(p);
	if (rv != TRUE) {
		err("Failure : Initialize context global variable");
		return FALSE;
	}

	/*
	 * Memory allocation for private information
	 */
	priv_info = g_try_malloc0(sizeof(PsPrivInfo));
	if (!priv_info) {
		err("Failure :Memory allocation !!");
		return FALSE;
	}

	address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	g_assert_no_error(error);

	conn = g_dbus_connection_new_for_address_sync(address,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
		G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL,
		NULL,
		&error);
	g_assert_no_error(error);
	if (!conn)
		dbg("Failure: G-dBus Connection failed"); /* TODO - Clean-up */

	/*
	 * Storing the G-dBus connection in Private information of PS Plugin
	 */
	priv_info->conn = conn;

	id = g_bus_own_name_on_connection(conn,
		PS_DBUS_SERVICE,
		G_BUS_NAME_OWNER_FLAGS_REPLACE,
		on_bus_acquired,
		NULL,
		p,
		NULL);

	dbg("PacketService - dBus address: [%s] dBus connection ID: [%d]",
		address, id);

	/*
	 * Initializing custom data for Packet Service
	 */
	priv_info->bus_id = id;
	priv_info->master = NULL;
	priv_info->p = p;

	/*
	 * Setting User data of PS plugin
	 */
	tcore_plugin_link_user_data(p, (void *)priv_info);

	return TRUE;
}

/*
 * Packet service de-initializer
 */
void ps_main_exit(TcorePlugin *p)
{
	PsPrivInfo *priv_info = tcore_plugin_ref_user_data(p);

	/*
	 * Clean-up Packet Service
	 */
	__packet_service_cleanup(priv_info);
	tcore_plugin_link_user_data(p, NULL);

	dbg("Packet Service exited!! ");
}
