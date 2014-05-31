/*
 * tel-plugin-packetservice
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

#ifndef __PS_ERROR_H__
#define __PS_ERROR_H__

#include <glib.h>
#include <dbus/dbus-glib.h>

G_BEGIN_DECLS

typedef enum {
	PS_ERR_INTERNAL,
	PS_ERR_NO_SERVICE,
	PS_ERR_TRASPORT,
	PS_ERR_NO_PROFILE,
	PS_ERR_WRONG_PROFILE,
	PS_ERR_MAX
} PS_ERR;

GQuark ps_error_quark(void);

#define    PS_ERROR	( ps_error_quark() )

G_END_DECLS

#endif /* __PS_ERROR_H__ */
