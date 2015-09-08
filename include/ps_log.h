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

#pragma once

extern gchar *ps_log_get_tag(CoreObject *co);

#define ps_err_ex_co(c,...) do {	\
		gchar *tag = ps_log_get_tag(c);	\
		err_ex(tag, "[ERROR] "__VA_ARGS__);	\
		g_free(tag);	\
	} while (0)

#define ps_warn_ex_co(c,...) do {	\
		gchar *tag = ps_log_get_tag(c);	\
		warn_ex(tag, "[WARN] "__VA_ARGS__);	\
		g_free(tag);	\
	} while (0)

#define ps_msg_ex_co(c,...) do {	\
		gchar *tag = ps_log_get_tag(c);	\
		msg_ex(tag, __VA_ARGS__);	\
		g_free(tag);	\
	} while (0)

#define ps_dbg_ex_co(c,...) do {	\
		gchar *tag = ps_log_get_tag(c);	\
		dbg_ex(tag, __VA_ARGS__);	\
		g_free(tag);	\
	} while (0)
