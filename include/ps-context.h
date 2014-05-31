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

#ifndef __PS_CONTEXT_H__
#define __PS_CONTEXT_H__

#include <glib.h>
#include <dbus/dbus-glib.h>

G_BEGIN_DECLS

typedef struct PsContext		PsContext;
typedef struct PsContextClass	PsContextClass;

#define PS_TYPE_CONTEXT				( ps_context_get_type() )
#define PS_CONTEXT(obj)				( G_TYPE_CHECK_INSTANCE_CAST( (obj), PS_TYPE_CONTEXT, PsContext ) )
#define PS_IS_CONTEXT(obj)			( G_TYPE_CHECK_INSTANCE_TYPE( (obj), PS_TYPE_CONTEXT) )

#define PS_CONTEXT_CLASS(klass)		( G_TYPE_CHECK_CLASS_CAST( (klass), PS_TYPE_CONTEXT, PsContextClass ) )
#define PS_IS_CONTEXT_CLASS(klass)	( G_TYPE_CHECK_CLASS_TYPE( (klass), PS_TYPE_CONTEXT ) )
#define PS_CONTEXT_GET_CLASS(obj)	( G_TYPE_INSTANCE_GET_CLASS( (obj), PS_TYPE_CONTEXT, PsContextClass ) )

GType    ps_context_get_type(void);

G_END_DECLS

#endif /* __PS_CONTEXT_H__ */
