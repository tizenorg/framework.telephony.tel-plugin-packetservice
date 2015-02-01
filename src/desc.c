/*
 * PacketService Control Module
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
#include <ps.h>

#ifdef TIZEN_DB_FROM_XML
static gboolean __ps_plugin_thread_finish_cb(gpointer data)
{
	GThread *selfi = data;

	if(!selfi) {
		err("thread is NULL !!");
		return FALSE;
	}
	dbg("Thread %p return is complete", selfi);
	g_thread_join(selfi);
	return FALSE;
}

static gpointer __ps_plugin_generate_database(gpointer data)
{	
	gboolean rv = FALSE;
	GThread *selfi = g_thread_self();

	_ps_context_init_global_apns_from_xml("/usr/share/ps-plugin/apns-conf.xml");
	/* remove exist sql */
	rv = ps_util_system_command("/bin/rm /opt/usr/share/telephony/dnet_db_init.sql");
	dbg("system command sent, rv(%d)", rv);
	/* Dump pdp_profile to sql */
	rv = ps_util_system_command("/usr/bin/sqlite3 /opt/dbspace/.dnet.db .dump | grep \"INSERT INTO \\\"pdp_profile\\\"\" > /opt/usr/share/telephony/dnet_db_init.sql");
	dbg("system command sent, rv(%d)", rv);
	if (TRUE == ps_util_thread_dispatch(g_main_context_default(), G_PRIORITY_LOW, (GSourceFunc)__ps_plugin_thread_finish_cb, selfi)) {
		dbg("Thread %p processing is complete", selfi);
	}
	return NULL;
}
#endif

static void _packet_service_cleanup(ps_custom_t *ps_ctx)
{
	dbg("Entered");

	if (ps_ctx == NULL)
		return;

	/*Cleaning up the master list*/
	g_slist_foreach(ps_ctx->master, __remove_master, NULL);

	/* Unowning the Gdbus */
	g_bus_unown_name(ps_ctx->bus_id);

	/* Freeing the memory allocated to the custom data for Packet Service */
	g_free(ps_ctx);

	dbg("Exiting");
}

static void on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
	gpointer *master = NULL;
	ps_custom_t *ps_ctx = user_data;

	dbg("Bus is acquired");

	/* Storing the GDbus connection in custom data */
	ps_ctx->conn = conn;

	/* Create master */
	master = _ps_master_create_master(conn, ps_ctx->p);
	if (!master) {
		err("Unable to Intialize the Packet Service");

		_packet_service_cleanup(ps_ctx);
		return;
	}

	ps_ctx->master = g_slist_append(ps_ctx->master, master);

	/* Create modems */
	(void)_ps_master_create_modems(master, NULL);

	dbg("initialized PacketService plugin!");
}

static gboolean on_load()
{
	dbg("PacketService plugin load!");

	return TRUE;
}

static gboolean on_init(TcorePlugin *p)
{
	ps_custom_t *ps_ctx = NULL;
	gboolean rv = FALSE;

	dbg("i'm init");

	if (!p)
		return FALSE;

	rv = _ps_context_initialize(p);
	if (rv == FALSE) {
		err("fail to initialize context global variable");
		return FALSE;
	}
#ifdef TIZEN_DB_FROM_XML
	do {	
		GThread *thread = NULL;
		thread = g_thread_new("GLOBAL-APN", __ps_plugin_generate_database, NULL);
		if (thread == NULL)
			err("Thread is not created");
	} while(0);
#endif
	ps_ctx = g_try_malloc0(sizeof(ps_custom_t));
	if (!ps_ctx) {
		err("Memory allocation failed for the custom data of PS");
		return FALSE;
	}

	/* Initializing the Custom data for PacketService */
	ps_ctx->conn = NULL;
	ps_ctx->p = p;
	ps_ctx->master = NULL;

	/* Link Custom data to Plug-in */
	tcore_plugin_link_user_data(p, ps_ctx);

	ps_ctx->bus_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
									PS_DBUS_SERVICE,
									G_BUS_NAME_OWNER_FLAGS_REPLACE,
									on_bus_acquired,
									NULL, NULL,
									ps_ctx,
									NULL);
	dbg("DBUS ID: [%d]", ps_ctx->bus_id);

	return TRUE;
}

static void on_unload(TcorePlugin *p)
{
	ps_custom_t *ps_ctx;
	dbg("i'm unload!");

	ps_ctx = tcore_plugin_ref_user_data(p);

	_packet_service_cleanup(ps_ctx);
}

EXPORT_API struct tcore_plugin_define_desc plugin_define_desc =
{
	.name = "PACKETSERVICE",
	.priority = TCORE_PLUGIN_PRIORITY_MID + 1,
	.version = 1,
	.load = on_load,
	.init = on_init,
	.unload = on_unload
};
