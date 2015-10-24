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

static gboolean on_load()
{
	dbg("i'm load!");

	return TRUE;
}

static gboolean on_init(TcorePlugin *plugin)
{
	dbg("i'm init!");

	return ps_main_init(plugin);
}

static void on_unload(TcorePlugin *plugin)
{
	dbg("i'm unload!");

	ps_main_exit(plugin);
}

/*
 * Packet service plug-in descriptor structure
 */
EXPORT_API struct tcore_plugin_define_desc plugin_define_desc = {
	.name = "PACKETSERVICE",
	.priority = TCORE_PLUGIN_PRIORITY_MID + 1,
	.version = 1,
	.load = on_load,
	.init = on_init,
	.unload = on_unload
};
