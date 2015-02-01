/*
 * PacketService Control Module
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: DongHoo Park <donghoo.park@samsung.com>
 *	    Arun Shukla <arun.shukla@samsung.com>
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

#include "ps.h"
#include "generated-code.h"

#include <server.h>
#include <plugin.h>
#include <core_object.h>
#include <co_context.h>
#include <co_ps.h>

#define PROP_DEFAULT	FALSE
#define PROP_DEFAULT_STR   NULL
#define BOOL2STRING(a)	((a==TRUE) ? ("TRUE"):("FALSE"))

struct ps_thread_data {
	ps_modem_t* modem;
	GThread *selfi;
};

static void __ps_modem_emit_property_changed_signal(ps_modem_t *modem);
static void __ps_modem_emit_service_added_signal(ps_modem_t *modem, gpointer service);
/*static void __ps_modem_emit_service_removed_signal(ps_modem_t *modem, gpointer service);*/
static void _ps_modem_setup_interface(PacketServiceModem *modem, ps_modem_t *modem_data);

static void	__ps_modem_create_service(GDBusConnection *conn, TcorePlugin *p,
				gpointer modem, CoreObject *co_modem);
static void __ps_modem_remove_service(ps_modem_t *modem, gpointer service);
static void __ps_modem_get_ps_setting_from_storage(ps_modem_t *object);
static void __ps_modem_processing_modem_event(gpointer object);

static gboolean __ps_modem_set_powered(ps_modem_t *modem, int value);
static gboolean __ps_modem_set_sim_complete(ps_modem_t *modem, gboolean value);

static gboolean __ps_modem_thread_finish_cb(gpointer data)
{
	struct ps_thread_data *thread_data = data;
	ps_modem_t * modem;
	GHashTableIter iter;
	gpointer key, value;

	if(!thread_data) {
		err("thread_data is NULL !!");
		return FALSE;
	}

	modem = thread_data->modem;

	dbg("Thread %p return is complete", thread_data->selfi);

	_ps_get_co_modem_values(thread_data->modem);
	_ps_modem_set_reset_profile(thread_data->modem, FALSE);
	packet_service_modem_complete_reset_profile(thread_data->modem->if_obj, thread_data->modem->invocation, TRUE);

	g_thread_join(thread_data->selfi);
	dbg("Clean up of thread %p is complete", thread_data->selfi);
	thread_data->modem->invocation = NULL;
	thread_data->modem = NULL;
	thread_data->selfi = NULL;
	g_free(thread_data);

	/* Try to re-connect default contexts after reset profile is complete */
	g_hash_table_iter_init(&iter, modem->services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		_ps_service_connect_default_context(value);
	}

	return FALSE;
}

static gpointer __ps_modem_regenerate_database(gpointer data)
{
	gboolean rv = FALSE;
	ps_modem_t *modem = data;
	struct ps_thread_data *thread_data = NULL;

	thread_data = g_try_malloc0(sizeof(*thread_data));
	if(!thread_data) {
		err("mamory alloc is fail !!!");
		return NULL;
	}
	thread_data->modem = modem;
	thread_data->selfi = g_thread_self();

	_ps_context_reset_profile_table(modem->cp_name);
	/* Re-generate global APN database */
	if(g_str_has_suffix(modem->cp_name, "1"))
		rv = ps_util_system_command("/usr/bin/sqlite3 /opt/dbspace/.dnet2.db < /usr/share/ps-plugin/dnet_db_init.sql");
	else
		rv = ps_util_system_command("/usr/bin/sqlite3 /opt/dbspace/.dnet.db < /usr/share/ps-plugin/dnet_db_init.sql");
	ps_dbg_ex_co(modem->co_modem, "system command sent, rv(%d)", rv);
	rv = _ps_context_fill_profile_table_from_ini_file(modem->cp_name);

	if (TRUE == ps_util_thread_dispatch(g_main_context_default(), G_PRIORITY_LOW, (GSourceFunc)__ps_modem_thread_finish_cb, thread_data)) {
		dbg("Thread %p processing is complete", thread_data->selfi);
	}
	return NULL;
}

void __remove_modem_handler(gpointer data)
{
	ps_modem_t *modem = data;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "Entered");
	if(!modem){
		dbg("Modem is NULL");
		return;
	}

	/*Need to remove the compelete hash table*/
	g_hash_table_remove_all(modem->services);

	/*Need to UNexport and Unref the master Object */
	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(modem->if_obj));

	g_object_unref(modem->if_obj);

	/*Need to free memory allocated for the internal structure*/
	g_queue_free(modem->work_queue);
	g_free(modem->path);
	g_free(modem->operator);
	g_free(modem->cp_name);
	g_free(modem);

	dbg("Exiting");
	return;
}

static void __ps_modem_emit_property_changed_signal(ps_modem_t *modem)
{
	GVariant *gv = NULL;
	GVariantBuilder properties;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "get modem properties");

	gv = _ps_modem_get_properties(modem, &properties);
	packet_service_modem_emit_property_changed(modem->if_obj, gv);

	return;
}

static void __ps_modem_emit_service_added_signal(ps_modem_t *modem, gpointer service)
{
	GVariant *gv = NULL;
	GVariantBuilder property;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "get service properties");

	gv = _ps_service_get_properties(service, &property);
	packet_service_modem_emit_service_added(modem->if_obj, gv);
	return;
}
/* blocked may be used later
static void __ps_modem_emit_service_removed_signal(ps_modem_t *modem, gpointer service)
{
	ps_service_t *psservice = service;
	packet_service_modem_emit_service_removed(modem->if_obj,psservice->path);
	return;
}
*/

static void __ps_modem_create_service(GDBusConnection *conn, TcorePlugin *p,
		gpointer modem, CoreObject *co_modem)
{
	gchar *t_path = NULL;
	GObject *object = NULL;

	GSList *co_pslist = NULL;
	GSList *co_networks = NULL;
	CoreObject *co_ps = NULL;
	CoreObject *co_network = NULL;
	TcorePlugin *target_plg = NULL;

	target_plg = tcore_object_ref_plugin(co_modem);
	co_pslist = tcore_plugin_get_core_objects_bytype(target_plg, CORE_OBJECT_TYPE_PS);
	if (!co_pslist)
		return;
	co_ps = co_pslist->data;
	g_slist_free(co_pslist);

	co_networks = tcore_plugin_get_core_objects_bytype(target_plg, CORE_OBJECT_TYPE_NETWORK);
	if (!co_networks)
		return;
	co_network = co_networks->data;
	g_slist_free(co_networks);

	if(!co_ps || !co_network)
		return;

	t_path = g_strdup_printf("%s/%s", _ps_modem_ref_path(modem), "umts_ps");
	ps_dbg_ex_co(co_modem, "service path (%s)", t_path);
	object = _ps_service_create_service(conn,p, modem, co_network, co_ps, t_path);
	if(object == NULL) {
		err("Failed to create service ");
		return;
	}

	g_hash_table_insert( ((ps_modem_t *) modem)->services, g_strdup(t_path), object);
	ps_dbg_ex_co(co_modem, "service (%p) insert to hash", object);
	__ps_modem_emit_service_added_signal((ps_modem_t *) modem, object);

	g_free(t_path);

	return;
}

static void __ps_modem_remove_service(ps_modem_t *modem, gpointer service)
{
	ps_service_t *psservice = service;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "Entered");

	/*Unexporting the interface for the modem*/
	if(psservice->if_obj){
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(psservice->if_obj));
		g_object_unref(psservice->if_obj);
		psservice->if_obj = NULL;
	}

	g_hash_table_remove(modem->services, _ps_service_ref_path(service));

	dbg("Successfully removed the service from the modem");
	return;
}

static gboolean __ps_modem_set_powered(ps_modem_t *modem, gboolean value)
{
	g_return_val_if_fail(modem != NULL, FALSE);

	modem->powered = value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) powered(%d)", modem, modem->powered);
	__ps_modem_emit_property_changed_signal(modem);
	return TRUE;
}

static gboolean __ps_modem_set_sim_complete(ps_modem_t *modem, gboolean value)
{
	g_return_val_if_fail(modem != NULL, FALSE);

	/* Update SIM init status */
	modem->sim_init = value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem [%p] SIM-Init [%s]", modem, (modem->sim_init ? "INITIALIZED" : "UNINITIALIZED"));

	__ps_modem_emit_property_changed_signal(modem);

	return TRUE;
}

static gboolean __ps_modem_set_flght_mode(ps_modem_t *modem, gboolean value)
{
	g_return_val_if_fail(modem != NULL, FALSE);

	modem->flight_mode = value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) flight_mode(%d)", modem, modem->flight_mode);
	__ps_modem_emit_property_changed_signal(modem);
	__ps_modem_processing_modem_event(modem);

	return TRUE;
}

static void __ps_modem_get_ps_setting_from_storage(ps_modem_t *object)
{
	gboolean key_3g_enable = FALSE, key_roaming_allowed = FALSE;
	gboolean key_nw_restrict_mode = FALSE, key_roaming_allowed_app_setting = FALSE;
	gint key_ps_mode = 0;
	ps_modem_t *modem = NULL;
	CoreObject *co_modem;

	modem = (ps_modem_t *) object;
	co_modem = _ps_modem_ref_co_modem(modem);
	key_3g_enable = _ps_master_get_storage_value_bool(modem->p_master, KEY_3G_ENABLE);
	key_roaming_allowed = _ps_master_get_storage_value_bool(modem->p_master, KEY_DATA_ROAMING_SETTING);
	key_roaming_allowed_app_setting = _ps_master_get_storage_value_bool(modem->p_master, KEY_DATA_ROAMING_APP_SETTING);
	key_ps_mode = _ps_master_get_storage_value_int(modem->p_master, KEY_POWER_SAVING_MODE);
	key_nw_restrict_mode = _ps_master_get_storage_value_bool(modem->p_master, KEY_NETWORK_RESTRICT_MODE);

	_ps_modem_set_data_allowed(modem, key_3g_enable);

	if(!key_roaming_allowed_app_setting){
		_ps_modem_set_data_roaming_allowed(modem, key_roaming_allowed);
	}
	else{
		_ps_modem_set_data_roaming_allowed(modem, TRUE);
	}

	_ps_modem_set_psmode(modem, key_ps_mode);
	ps_dbg_ex_co(co_modem, "data allowed(%d) roaming allowed(%d) power saving mode(%d), network restrict mode (%d)",
		key_3g_enable, key_roaming_allowed, key_ps_mode, key_nw_restrict_mode);
	return;
}

static void __ps_modem_processing_modem_event(gpointer object)
{
	ps_modem_t * modem = object;
	GHashTableIter iter;
	gpointer key, value;

	g_return_if_fail(modem != NULL);

	if(!modem->services)
		return;

	g_hash_table_iter_init(&iter, modem->services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gboolean s_roaming = FALSE;

		s_roaming = _ps_service_get_roaming(value);
		_ps_update_cellular_state_key(value);

		if(modem->powered == PS_MODEM_STATE_OFFLINE){
			_ps_service_remove_contexts(value);
			_ps_free_co_ps_event(value);
			_ps_free_co_network_event(value);
			__ps_modem_remove_service(modem, value);
			continue;
		}

		if(modem->flight_mode || !modem->data_allowed || (s_roaming && !modem->roaming_allowed) ){
			_ps_service_disconnect_contexts(value);
			continue;
		}

		//only available case
		_ps_service_connect_default_context(value);
	}

	return;
}

gpointer _ps_modem_create_modem(GDBusConnection *conn, TcorePlugin *p, gpointer master,
				gchar* modem_name, gpointer co_modem , gchar *cp_name)
{
	PacketServiceModem *modem;
	ps_modem_t *new_modem;
	GError *error = NULL;

	ps_dbg_ex_co(co_modem, "modem object create");
	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(master != NULL, NULL);

	/*creating the master object for the interface com.tcore.ps.modem*/
	modem = packet_service_modem_skeleton_new();

	/*Initializing the modem list for internal referencing*/
	new_modem = g_try_malloc0(sizeof(ps_modem_t));
	if(NULL == new_modem){
		ps_err_ex_co(co_modem, "Unable to allocate memory for modem");
		return NULL;
	}

	/*Add work queue to keep user request in case of handling active PDP context*/
	new_modem->hook_flag = 0x00;
	new_modem->work_queue = g_queue_new();
	if( NULL == new_modem->work_queue ){
		ps_err_ex_co(co_modem, "Unable to get work queue for modem");
		g_free(new_modem);
		return NULL;
	}

	new_modem->conn = conn;
	new_modem->p_master = master;
	new_modem->plg = p;
	new_modem->co_modem = co_modem;
	new_modem->path = g_strdup(modem_name);
	new_modem->cp_name = g_strdup(cp_name);
	new_modem->if_obj = modem;
	new_modem->services = g_hash_table_new_full(g_str_hash,g_str_equal, g_free, __remove_service_handler);
	new_modem->contexts = g_hash_table_new_full(g_str_hash,g_str_equal, g_free, NULL);

	__ps_modem_get_ps_setting_from_storage(new_modem);
	_ps_hook_co_modem_event(new_modem);
	_ps_get_co_modem_values(new_modem);

	/*Setting the interface call backs functions*/
	_ps_modem_setup_interface(modem, new_modem);

	/*exporting the interface object to the path mention for modem*/
	g_dbus_interface_skeleton_export((G_DBUS_INTERFACE_SKELETON(modem)),
			conn,
			modem_name,
			&error);

	g_assert_no_error (error);

	/* Adding hooks for special Network Requests */
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_NETWORK_SEARCH,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_NETWORK_SET_PLMN_SELECTION_MODE,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_NETWORK_SET_MODE,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_NETWORK_SET_CANCEL_MANUAL_SEARCH,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_SIM_SET_POWERSTATE,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_MODEM_SET_FLIGHTMODE,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_MODEM_POWER_OFF,
			ps_handle_hook, new_modem);
	tcore_server_add_request_hook(tcore_plugin_ref_server(p),
			TREQ_MODEM_POWER_LOW,
			ps_handle_hook, new_modem);

	ps_err_ex_co(co_modem, "Successfully created the modem");
	return new_modem;
}

gboolean _ps_modem_processing_flight_mode(gpointer object, gboolean enable)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	if (modem->flight_mode == enable)
		return TRUE;

	__ps_modem_set_flght_mode(modem, enable);
	return TRUE;
}

gboolean _ps_modem_processing_power_enable(gpointer object, gboolean enable)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	dbg("modem->powered -> [%d] & Modem->sim_init ->[%d] & Modem->reset_profile ->[%d]",
		modem->powered, modem->sim_init, modem->reset_profile);

	if (modem->sim_init  && (modem->powered == PS_MODEM_STATE_ONLINE) && !modem->reset_profile) {
		return TRUE;
	}

	__ps_modem_set_powered(modem, enable);

	if (enable == PS_MODEM_STATE_ONLINE) {
		if(!modem->initial_bootup) {
			dbg("initial bootup");
			modem->initial_bootup = TRUE;
		} else {
			return TRUE;
		}

		__ps_modem_create_service(modem->conn, modem->plg, modem, modem->co_modem);
		/* Populate Profile for operator as it was removed by fligh mode operation  */

		if (modem->sim_init == TRUE && modem->operator != NULL){
			GHashTable *contexts = _ps_context_ref_hashtable(modem);;

			if (contexts != NULL) {
				GHashTableIter iter;
				gpointer key, value;
				g_hash_table_iter_init(&iter, modem->services);

				while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
					_ps_service_set_number_of_pdn_cnt(value, modem->operator);
					_ps_service_ref_contexts(value, contexts, modem->operator);
				}
			}
		}
	}
	else if(enable == PS_MODEM_STATE_OFFLINE) {
		modem->initial_bootup = FALSE;
		__ps_modem_processing_modem_event(modem);
		/* SIM init should be set to FALSE during CP silent reset.
		 * MODEM_STATE_RESET -> PS_MODEM_STATE_OFFLINE.
		 * and will be updated when SIM initialized complete notification is received.
		 */
	}
	return TRUE;
}

gboolean _ps_modem_processing_sim_complete(gpointer object, gboolean complete, gchar *operator)
{
	ps_modem_t *modem = object;
	CoreObject *co_modem;
	g_return_val_if_fail(modem != NULL, FALSE);

	co_modem = _ps_modem_ref_co_modem(modem);
	ps_dbg_ex_co(co_modem, "Entered: sim_init[%d]", modem->sim_init);

	if (modem->sim_init == complete && !modem->reset_profile) {
		ps_dbg_ex_co(co_modem, "No change in SIM state");
		return TRUE;
	}
	else {
		gboolean different_sim = FALSE;

		if (operator && (g_strcmp0(modem->operator, operator) != 0)) {
			ps_dbg_ex_co(co_modem, "Previous operator: [%s] Present operator: [%s]", modem->operator, operator);
			g_free(modem->operator);

			modem->operator = g_strdup(operator);
			different_sim = TRUE;
		}

		/* Update SIM state */
		__ps_modem_set_sim_complete(modem, complete);

		/* free modem operator */
		if( FALSE == complete ) {
			g_free(modem->operator);
			modem->operator = NULL;
		}
		/* Modem power off notification coming due to which profile are removed */
		/* and needed to be re inserted in db with sim off notification  */
		/*
		 * Context table creation
		 *
		 * Create context if -
		 *	SIM is initiatized
		 * Initialzed SIM is different (if same SIM is re-initialized then need not create context)
		 * or Reset profile Operator is non-null
		 * Delete context if sim_init = FALSE;
		 * This will be coming when SIM power off or CARD error is received.
		 */
		if(modem->sim_init == TRUE) {
			if((different_sim || modem->reset_profile) && (modem->operator != NULL)) {
				GHashTable *contexts;

				ps_dbg_ex_co(co_modem, "Creating Hash table...");
				contexts = _ps_context_create_hashtable(object);
				if (contexts != NULL) {
					GHashTableIter iter;
					gpointer key, value;

					g_hash_table_iter_init(&iter, modem->services);
					while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
						_ps_service_set_number_of_pdn_cnt(value, modem->operator);
						_ps_service_ref_contexts(value, contexts, modem->operator);
					}
				}
				else {
					ps_dbg_ex_co(co_modem, "Failed to create HASH table");
					return FALSE;
				}
			}
		} else {
			GHashTableIter iter;
			gpointer key, value;

			if(!modem->services)
				goto EXIT;

			g_hash_table_iter_init(&iter, modem->services);
			while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
				ps_dbg_ex_co(co_modem, "Remove contexts on service(%p)", value);
				_ps_service_remove_contexts(value);
			}
		}
	}
EXIT:
	dbg("Exiting");
	return TRUE;
}

gboolean _ps_modem_set_reset_profile(gpointer object, gboolean value)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->reset_profile = value;
	dbg("modem(%p) reset_profile(%d)", modem, modem->reset_profile);
	return TRUE;
}

gboolean _ps_modem_get_reset_profile(gpointer object)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	dbg("modem(%p) reset_profile(%d)", modem, modem->reset_profile);

	return modem->reset_profile;
}

gboolean _ps_modem_set_sim_enabled(gpointer object, gboolean value)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->sim_init = value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) sim_enabled(%d)", modem, modem->sim_init);
	return TRUE;
}

gboolean _ps_modem_set_data_allowed(gpointer object, gboolean value)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->data_allowed = value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) data allowed(%d)", modem, modem->data_allowed);
	__ps_modem_emit_property_changed_signal(modem);
	__ps_modem_processing_modem_event(modem);

	return TRUE;
}

gboolean _ps_modem_get_data_allowed(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->data_allowed;
}

gboolean _ps_modem_set_data_roaming_allowed(gpointer object, gboolean roaming_allowed)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->roaming_allowed = roaming_allowed;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) roaming allowed(%d)", modem, modem->roaming_allowed);
	__ps_modem_emit_property_changed_signal(modem);

	if(!modem->services)
		return TRUE;

	if(modem->roaming)
		__ps_modem_processing_modem_event(modem);

	return TRUE;
}

gboolean _ps_modem_set_psmode(gpointer object, gint value)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->ps_mode= value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) power saving mode(%d)", modem, modem->ps_mode);

	return TRUE;
}

gboolean _ps_modem_get_roaming(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->roaming;
}

void _ps_modem_set_roaming(gpointer object, gboolean value)
{
	ps_modem_t * modem = object;
	g_return_if_fail(modem != NULL);

	modem->roaming = value;
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "modem(%p) roaming(%d)", modem, modem->roaming);

	return;
}

gint _ps_modem_get_psmode(gpointer object)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->ps_mode;
}

guchar _ps_modem_get_hook_flag(gpointer object)
{
	ps_modem_t * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->hook_flag;
}

gboolean _ps_modem_get_data_roaming_allowed(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->roaming_allowed;
}

gboolean _ps_modem_get_flght_mode(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->flight_mode;
}

gboolean _ps_modem_get_sim_init(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->sim_init;
}

gboolean _ps_modem_get_power(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->powered;
}

gchar* _ps_modem_ref_operator(gpointer object)
{
	ps_modem_t * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->operator;
}

ps_subs_type _ps_modem_get_subs_type(gpointer object)
{
	ps_modem_t *modem = object;
	const gchar *cp_name;

	g_return_val_if_fail(modem != NULL, FALSE);

	cp_name = modem->cp_name;
	if (g_str_has_suffix(cp_name, "0")) {
		return PS_SUBS_PRIMARY;
	} else if (g_str_has_suffix(cp_name, "1")) {
		return PS_SUBS_SECONDARY;
	} else if (g_str_has_suffix(cp_name, "2")) {
		return PS_SUBS_TERTIARY;
	}

	return PS_SUBS_MAX;
}

gboolean _ps_modem_get_properties_handler(gpointer object, GVariantBuilder *properties)
{
	ps_modem_t *modem = object;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "get modem properties");
	g_return_val_if_fail(modem != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	g_variant_builder_open(properties,G_VARIANT_TYPE("a{ss}"));
	g_variant_builder_add(properties, "{ss}", "path", modem->path);

	if(modem->operator){
		g_variant_builder_add(properties, "{ss}", "operator", modem->operator);
	}
	g_variant_builder_add(properties, "{ss}", "powered", BOOL2STRING(modem->powered));
	g_variant_builder_add(properties, "{ss}", "sim_init", BOOL2STRING(modem->sim_init));
	g_variant_builder_add(properties, "{ss}", "flight_mode", BOOL2STRING(modem->flight_mode));
	g_variant_builder_add(properties, "{ss}", "roaming_allowed", BOOL2STRING(modem->roaming_allowed));
	g_variant_builder_add(properties, "{ss}", "data_allowed", BOOL2STRING(modem->data_allowed));
	g_variant_builder_close(properties);

	dbg("Exiting");
	return TRUE;
}

GVariant *_ps_modem_get_properties(gpointer object, GVariantBuilder *properties)
{
	ps_modem_t *modem = object;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "get modem properties");
	g_return_val_if_fail(modem != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	g_variant_builder_init(properties, G_VARIANT_TYPE("a{ss}"));

	g_variant_builder_add(properties, "{ss}", "path", modem->path);

	if(modem->operator){
		g_variant_builder_add(properties, "{ss}", "operator", modem->operator);
	}
	g_variant_builder_add(properties, "{ss}", "powered", BOOL2STRING(modem->powered));
	g_variant_builder_add(properties, "{ss}", "sim_init", BOOL2STRING(modem->sim_init));
	g_variant_builder_add(properties, "{ss}", "flight_mode", BOOL2STRING(modem->flight_mode));
	g_variant_builder_add(properties, "{ss}", "roaming_allowed", BOOL2STRING(modem->roaming_allowed));
	g_variant_builder_add(properties, "{ss}", "data_allowed", BOOL2STRING(modem->data_allowed));

	dbg("Exiting");
	return g_variant_builder_end(properties);
}

GHashTable* _ps_modem_ref_services(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->services;
}

gchar* _ps_modem_ref_path(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->path;
}

gpointer _ps_modem_ref_plugin(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->plg;
}

gpointer _ps_modem_ref_dbusconn(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->conn;
}

gpointer _ps_modem_ref_co_modem(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->co_modem;
}

gpointer _ps_modem_ref_work_queue(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->work_queue;
}

gchar *_ps_modem_ref_cp_name(gpointer object)
{
	ps_modem_t *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->cp_name;
}


static gboolean on_modem_get_properties (PacketServiceModem *obj_modem,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariant *gv = NULL;
	GVariantBuilder properties;

	dbg("get modem properties");

	gv = _ps_modem_get_properties(user_data, &properties);
	packet_service_modem_complete_get_properties(obj_modem, invocation, gv);
	return TRUE;
}

static gboolean on_modem_get_services (PacketServiceModem *obj_modem,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariantBuilder b_service;
	GVariant *services;

	GHashTableIter iter;
	gpointer key, value;
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);

	ps_dbg_ex_co(co_modem, "modem get service interface");

	if (modem->services == NULL) {
		FAIL_RESPONSE(invocation,PS_ERR_INTERNAL);
		return TRUE;
	}

	g_variant_builder_init(&b_service, G_VARIANT_TYPE("a{sa{ss}}"));
	g_hash_table_iter_init(&iter,modem->services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *path = NULL;

		g_variant_builder_open(&b_service,G_VARIANT_TYPE("{sa{ss}}"));
		path = _ps_service_ref_path(value);
		ps_dbg_ex_co(co_modem, "path added [%s]", path);
		g_variant_builder_add(&b_service, "s", path);
		if(FALSE == _ps_service_get_properties_handler(value, &b_service)){
			g_variant_builder_close(&b_service);
			FAIL_RESPONSE(invocation,PS_ERR_INTERNAL);
			return TRUE;
		}
		g_variant_builder_close(&b_service);
	}

	services = g_variant_builder_end(&b_service);
	packet_service_modem_complete_get_services(obj_modem, invocation, services);
	return TRUE;
}

static gboolean on_modem_get_profile_list (PacketServiceModem *obj_modem,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	int profile_index = 0;
	GHashTableIter iter;
	gpointer key, value;

	guint len =0;
	gchar **strv = NULL;
	GSList *profiles = NULL;
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);

	ps_dbg_ex_co(co_modem, "master get the profile list");

	if(modem->contexts == NULL){
		ps_err_ex_co(co_modem, "no profiles");
		FAIL_RESPONSE(invocation, PS_ERR_NO_PROFILE);
		return TRUE;
	}

	g_hash_table_iter_init(&iter, modem->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *s_path = NULL;

		s_path = _ps_context_ref_path(value);
		ps_dbg_ex_co(co_modem, "key(%s), value(%p), path(%s)", (gchar *)key, value, s_path);
		if(s_path)
			profiles = g_slist_append(profiles, g_strdup((const gchar*)s_path));
	}

	if (profiles == NULL) {
		ps_dbg_ex_co(co_modem, "no profiles");
		FAIL_RESPONSE(invocation, PS_ERR_NO_PROFILE);
		return TRUE;
	}

	len = g_slist_length(profiles);
	strv = g_new(gchar *, len+1);

	while (profiles) {
		strv[profile_index] = g_strdup(profiles->data);
		profile_index++;

		profiles = profiles->next;
	}
	strv[profile_index] = NULL;

	packet_service_modem_complete_get_profile_list(obj_modem,
				invocation,(const gchar *const *)strv);

	g_strfreev(strv);
	profiles = g_slist_nth(profiles, 0);
	g_slist_free_full(profiles, g_free);
	dbg("Exiting");
	return TRUE;
}

static gboolean on_modem_add_profile (PacketServiceModem *obj_modem,
		GDBusMethodInvocation *invocation,
		GVariant *property,
		gpointer user_data)
{
	GVariantIter g_iter;
	gchar *g_value;
	gchar *g_key;

	gboolean rv = FALSE;
	gchar *operator = NULL;
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);
	GHashTable *profile_property = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	ps_dbg_ex_co(co_modem, "add profile request");

	operator = _ps_modem_ref_operator(modem);

	if(!operator){
		ps_dbg_ex_co(co_modem, "there is no active modem");
		FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
		goto OUT;
	}

	/*Create a hash table for the profile property as all fucntion already use ghash table */
	g_variant_iter_init (&g_iter, property);
	while (g_variant_iter_next (&g_iter, "{ss}", &g_key, &g_value)){

		ps_dbg_ex_co(co_modem, " '%s' value '%s'", g_key, g_value);
		g_hash_table_insert(profile_property, g_strdup(g_key), g_strdup(g_value));
		/* must free data for ourselves */
		g_free (g_value);
		g_free (g_key);
	}

	rv = _ps_context_add_context(modem, operator, profile_property);
	if(rv != TRUE){
		ps_err_ex_co(co_modem, "Failed to add the Profile");
		FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
		goto OUT;
	}

	packet_service_modem_complete_add_profile(obj_modem, invocation, TRUE);
OUT:
	g_hash_table_destroy(profile_property);
	dbg("Exiting");
	return TRUE;
}

gboolean _ps_modem_initiate_reset_profile(gpointer value)
{
	CoreObject *co_modem;
	ps_modem_t *modem = value;
	GHashTableIter iter;
	gpointer key, key_value;

	g_return_val_if_fail(modem != NULL, FALSE);

	co_modem = _ps_modem_ref_co_modem(modem);

	_ps_modem_processing_power_enable(modem, PS_MODEM_STATE_OFFLINE);
	ps_dbg_ex_co(co_modem, "Reseting the hash table");
	_ps_context_reset_hashtable(modem);

	if(modem->type == 0){
		GThread *thread;
		gchar *name = g_strdup_printf("REGEN-DB-%s", modem->cp_name);

		thread = g_thread_new(name, __ps_modem_regenerate_database, modem);
		g_free(name);
		if (thread == NULL) {
			dbg("Thread is not created");
			FAIL_RESPONSE(modem->invocation ,PS_ERR_INTERNAL);
			return FALSE;
		} else {
			dbg("Thread(%p) is created", thread);
		}
		return FALSE;
	}

	_ps_get_co_modem_values(modem);
	_ps_modem_set_reset_profile(modem, FALSE);
	packet_service_modem_complete_reset_profile(modem->if_obj, modem->invocation, TRUE);
	modem->invocation = NULL;

	/* Try to re-connect default contexts after reset profile is complete */
	g_hash_table_iter_init(&iter, modem->services);
	while (g_hash_table_iter_next(&iter, &key, &key_value) == TRUE) {
		//only available case
		_ps_service_connect_default_context(key_value);
	}

	ps_dbg_ex_co(co_modem, "Exiting");

	return FALSE;
}

static gboolean on_modem_reset_profile (PacketServiceModem *obj_modem,
		GDBusMethodInvocation *invocation,
		gint type,
		gpointer user_data)
{
	TReturn rv;
	gboolean contexts_active;
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);
	CoreObject *co_ps;
	int state;

	ps_dbg_ex_co(co_modem, "reset profile request type(%d)", type);

	if (_ps_modem_get_reset_profile(modem) == TRUE) {
		ps_err_ex_co(co_modem, "Reset Profile is already in Progress");
		packet_service_modem_complete_reset_profile(obj_modem, invocation, FALSE);
		ps_dbg_ex_co(co_modem, "Exiting");
		return FALSE;
	}

	_ps_modem_set_reset_profile(modem, TRUE);

	co_ps = tcore_plugin_ref_core_object(tcore_object_ref_plugin(_ps_modem_ref_co_modem(modem)),
										CORE_OBJECT_TYPE_PS);

	modem->invocation = invocation;
	modem->type = type;

	contexts_active = tcore_ps_any_context_activating_activated(co_ps, &state);

	if(contexts_active == TRUE) {
		ps_dbg_ex_co(co_modem, "Contexts are in [%d] state", state);
		if (state == CONTEXT_STATE_ACTIVATED) {
			ps_dbg_ex_co(co_modem, "Contexts are in Actived state. Sending Diconnect Notification to all connected contexts");
			rv = tcore_ps_deactivate_contexts(co_ps);
			if(rv != TCORE_RETURN_SUCCESS){
				ps_dbg_ex_co(co_modem, "fail to deactivation");
			}
		}else if (state == CONTEXT_STATE_ACTIVATING) {
			ps_dbg_ex_co(co_modem, "Contexts are in Activating state. Wait for them to connect");
		}
	}else {
		ps_dbg_ex_co(co_modem, "No contexts are in activating or activated state");
		ps_dbg_ex_co(co_modem, "Profiles reset is being initiated");
		_ps_modem_initiate_reset_profile(modem);
	}
	return TRUE;
}


static void _ps_modem_setup_interface(PacketServiceModem *modem, ps_modem_t *modem_data)
{
	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem_data), "Entered");

	g_signal_connect (modem,
			"handle-get-properties",
			G_CALLBACK (on_modem_get_properties),
			modem_data);

	g_signal_connect (modem,
			"handle-get-services",
			G_CALLBACK (on_modem_get_services),
			modem_data);

	g_signal_connect (modem,
			"handle-get-profile-list",
			G_CALLBACK (on_modem_get_profile_list),
			modem_data);

	g_signal_connect (modem,
			"handle-add-profile",
			G_CALLBACK (on_modem_add_profile),
			modem_data);

	g_signal_connect (modem,
			"handle-reset-profile",
			G_CALLBACK (on_modem_reset_profile),
			modem_data);

	return;
}

