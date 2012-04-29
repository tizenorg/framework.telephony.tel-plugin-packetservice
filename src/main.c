/*
 * PacketService Control Module
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: DongHoo Park <donghoo.park@samsung.com>
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
#include <dbus/dbus-glib.h>

#include <tcore.h>
#include <plugin.h>

#include <ps.h>

static gboolean on_load()
{
	dbg("PacketService plugin load!");
	return TRUE;
}

static gboolean on_init(TcorePlugin *p)
{
	gpointer *master;
	DBusGConnection *conn;
	GError *error = NULL;
	gboolean rv=FALSE;

	//get dbus connection
	conn = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		err("fail to get dbus(%s)", error->message);
		return FALSE;
	}
	dbg("get dbus connection(%p)", conn);

	dbg("plugin pointer (%p)", p);
	rv = _ps_context_initialize(p);
	if(rv != TRUE){
		dbg("fail to initialize context global variable");
		return FALSE;
	}

	master = _ps_master_create_master(conn, p);
	rv = _ps_master_create_modems(master);

	dbg("initialized PacketService plugin!");
	return TRUE;
}

static void on_unload(TcorePlugin *p)
{
	dbg("i'm unload!");
	return;
}

struct tcore_plugin_define_desc plugin_define_desc =
{
	.name = "PACKETSERVICE",
	.priority = TCORE_PLUGIN_PRIORITY_MID + 1,
	.version = 1,
	.load = on_load,
	.init = on_init,
	.unload = on_unload
};
