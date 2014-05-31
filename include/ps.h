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
#ifndef __PS_H__
#define __PS_H__

#define PS_DBUS_SERVICE	"com.tcore.ps"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <dbus/dbus-glib.h>

#include <tcore.h>
#include <plugin.h>
#include <storage.h>
#include <server.h>
#include <core_object.h>
#include <hal.h>

/*Storage Key value*/
#define KEY_3G_ENABLE				STORAGE_KEY_3G_ENABLE
#define KEY_DATA_ROAMING_SETTING	STORAGE_KEY_SETAPPL_STATE_DATA_ROAMING_BOOL

/*MASTER*/
gpointer    _ps_master_create_master(DBusGConnection *conn, TcorePlugin *p);
gboolean    _ps_master_create_modems(gpointer master);
gboolean    _ps_master_get_storage_value(gpointer master, enum tcore_storage_key key);
gboolean    _ps_master_set_storage_value(gpointer master, enum tcore_storage_key key, gboolean value);

/*MODEM*/
gpointer    _ps_modem_create_modem(DBusGConnection *conn, TcorePlugin *p, gpointer master,
				gchar* modem_name, gpointer co_modem);
gboolean    _ps_modem_processing_flight_mode(gpointer object, gboolean enable);
gboolean    _ps_modem_processing_power_enable(gpointer modem, gboolean enable);
gboolean    _ps_modem_processing_sim_complete(gpointer modem, gboolean complete, gchar *operator);
gboolean    _ps_modem_set_sim_enabled(gpointer object, gboolean value);
gboolean    _ps_modem_set_data_allowed(gpointer modem, gboolean value);
gboolean    _ps_modem_get_data_allowed(gpointer modem);
gboolean    _ps_modem_set_data_roaming_allowed(gpointer modem, gboolean value);
gboolean    _ps_modem_get_data_roaming_allowed(gpointer modem);
gboolean    _ps_modem_get_flght_mode(gpointer object);
gboolean    _ps_modem_get_sim_init(gpointer object);
gboolean    _ps_modem_get_power(gpointer object);
gchar*      _ps_modem_ref_operator(gpointer object);
gboolean    _ps_modem_get_properties(gpointer modem, GHashTable *properties);
GHashTable* _ps_modem_ref_services(gpointer modem);
gchar*      _ps_modem_ref_path(gpointer modem);
gpointer    _ps_modem_ref_plugin(gpointer modem);
gpointer    _ps_modem_ref_dbusconn(gpointer modem);
gpointer    _ps_modem_ref_co_modem(gpointer modem);

/*SERVICE*/
gpointer    _ps_service_create_service(DBusGConnection *conn, TcorePlugin *p,
				gpointer modem, CoreObject *co_network, CoreObject *co_ps, gchar* path);
gboolean    _ps_service_ref_context(gpointer object, gpointer context);
gboolean    _ps_service_ref_contexts(gpointer service, GHashTable *contexts, gchar *operator);
gboolean    _ps_service_unref_context(gpointer service, gpointer context);
gboolean    _ps_service_get_properties(gpointer service, GHashTable *properties);
gchar*      _ps_service_ref_path(gpointer service);
gpointer    _ps_service_ref_plugin(gpointer service);
gpointer    _ps_service_ref_co_network(gpointer service);
gpointer    _ps_service_ref_co_ps(gpointer service);
gboolean    _ps_service_set_context_info(gpointer service, struct tnoti_ps_pdp_ipconfiguration *devinfo);
int 		_ps_service_define_context(gpointer object, gpointer context);
int         _ps_service_activate_context(gpointer service, gpointer context);
gboolean    _ps_service_deactivate_context(gpointer service, gpointer context);
void        _ps_service_connection_timer(gpointer service, gpointer context);
void        _ps_service_reset_connection_timer(gpointer context);
void        _ps_service_connect_default_context(gpointer service);
void        _ps_service_remove_contexts(gpointer object);
void        _ps_service_disconnect_contexts(gpointer service);
gboolean    _ps_service_processing_network_event(gpointer service, gboolean ps_attached, gboolean roaming);
gpointer    _ps_service_return_default_context(gpointer object);
gboolean    _ps_service_set_connected(gpointer service, int context_id, gboolean enabled);
void		_ps_service_set_ps_defined(gpointer *object, gboolean value, int cid);
gboolean    _ps_service_set_ps_attached(gpointer service, gboolean value);
gboolean    _ps_service_set_roaming(gpointer service, gboolean value);
gboolean    _ps_service_get_roaming(gpointer object);
gboolean    _ps_service_set_access_technology(gpointer service,
				enum telephony_network_access_technology value);
enum telephony_ps_state
			_ps_service_check_cellular_state(gpointer object);

/*CONTEXT*/
gboolean    _ps_context_initialize(gpointer plugin);
gboolean    _ps_context_reset_profile_table(void);
gboolean    _ps_context_fill_profile_table_from_ini_file(void);
gboolean    _ps_context_reset_hashtable(void);
GHashTable* _ps_context_create_hashtable(DBusGConnection *conn, TcorePlugin *p, gchar *mccmnc);
GHashTable* _ps_context_ref_hashtable(void);
gboolean    _ps_context_add_context(gpointer modem, gchar *operator, GHashTable *property);
gboolean    _ps_context_get_properties(gpointer context, GHashTable *properties);
gboolean    _ps_context_set_service(gpointer context, gpointer service);
gpointer    _ps_context_ref_service(gpointer object);
gboolean    _ps_context_get_alwayson_enable(gpointer object);
gchar*      _ps_context_ref_path(gpointer context);
gpointer    _ps_context_ref_co_context(gpointer context);
gboolean    _ps_context_set_connected(gpointer context, gboolean enabled);
gboolean 	_ps_context_set_ps_defined(gpointer *object, gboolean value, int cid);
gboolean 	_ps_context_get_ps_defined(gpointer *object);
gboolean    _ps_context_set_alwayson_enable(gpointer object, gboolean enabled);
gboolean    _ps_context_get_default_internet(gpointer object);
gboolean    _ps_context_remove_context(gpointer context);

/*PLUGIN INTERFACE*/
gboolean    _ps_hook_co_modem_event(gpointer modem);
gboolean    _ps_get_co_modem_values(gpointer modem);
gboolean    _ps_hook_co_network_event(gpointer service);
gboolean    _ps_free_co_network_event(gpointer service);
gboolean    _ps_get_co_network_values(gpointer service);
gboolean    _ps_hook_co_ps_event(gpointer service);
gboolean    _ps_free_co_ps_event(gpointer service);
gboolean    _ps_update_cellular_state_key(gpointer service);

#endif /* __PS_H__ */
