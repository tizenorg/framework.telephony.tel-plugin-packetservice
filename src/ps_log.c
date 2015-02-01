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

#include <tcore.h>
#include <server.h>
#include <core_object.h>

#include <glib.h>

gchar *ps_log_get_tag(CoreObject *co);

gchar *ps_log_get_tag(CoreObject *co)
{
	const char *cp_name;
	cp_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co));

	return g_strdup_printf("PS/%s", cp_name);
}