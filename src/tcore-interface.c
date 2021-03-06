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

#include "ps.h"

#include <server.h>
#include <plugin.h>
#include <storage.h>
#include <co_ps.h>
#include <co_modem.h>
#include <co_sim.h>
#include <co_network.h>

static enum tcore_hook_return __on_hook_call_status(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	struct tnoti_ps_call_status *cstatus = NULL;

	dbg("call status event");
	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	cstatus = (struct tnoti_ps_call_status *) data;
	dbg("call status event cid(%d) state(%d) reason(%d)",
			cstatus->context_id, cstatus->state, cstatus->result);

	//send activation event / deactivation event
	if (cstatus->state == 1) {/* CONNECTED */
		dbg("service is activated");
		_ps_service_set_connected(service, cstatus->context_id, TRUE);
	}
	else if (cstatus->state == 3) { /* NO CARRIER */
		dbg("service is deactivated");
		_ps_service_set_connected(service, cstatus->context_id, FALSE);
	}

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_session_data_counter(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	g_return_val_if_fail(user_data != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	dbg("session data counter event");

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_ipconfiguration(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	CoreObject *co_ps = NULL;
	struct tnoti_ps_pdp_ipconfiguration *devinfo = NULL;

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	devinfo = (struct tnoti_ps_pdp_ipconfiguration *) data;
	co_ps = (CoreObject *) _ps_service_ref_co_ps(service);

	if (co_ps != source) {
		dbg("ps object is different");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	dbg("ip configuration event");
	_ps_service_set_context_info(service, devinfo);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_powered(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	gpointer modem = user_data;
	struct tnoti_modem_power *modem_power = NULL;

	gboolean power = FALSE;

	dbg("powered event called");

	g_return_val_if_fail(modem != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	modem_power = (struct tnoti_modem_power *)data;

	if ( modem_power->state == MODEM_STATE_ONLINE )
		power = TRUE;
	else
		power = FALSE;

	_ps_modem_processing_power_enable(modem, power);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_flight(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	gpointer modem = user_data;
	struct tnoti_modem_flight_mode *modem_flight = NULL;
	dbg("powered event called");

	g_return_val_if_fail(modem != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	modem_flight = (struct tnoti_modem_flight_mode *)data;
	_ps_modem_processing_flight_mode(modem, modem_flight->enable);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_net_register(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	gboolean ps_attached = FALSE;
	struct tnoti_network_registration_status *regist_status;

	dbg("network register event called");
	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	regist_status = (struct tnoti_network_registration_status *) data;
	if (regist_status->ps_domain_status == NETWORK_SERVICE_DOMAIN_STATUS_FULL)
		ps_attached = TRUE;

	_ps_service_processing_network_event(service, ps_attached, regist_status->roaming_status);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_net_change(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	struct tnoti_network_change *network_change;

	dbg("network change event called");
	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	network_change = (struct tnoti_network_change *) data;
	_ps_service_set_access_technology(service, network_change->act);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_sim_init(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	struct tnoti_sim_status *sim_data;

	dbg("sim init event called");
	g_return_val_if_fail(user_data != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	sim_data = (struct tnoti_sim_status *)data;
	dbg("sim status is (%d)", sim_data->sim_status);

	if( sim_data->sim_status == SIM_STATUS_INIT_COMPLETED){
		struct tel_sim_imsi *sim_imsi = NULL;
		sim_imsi = tcore_sim_get_imsi(source);
		_ps_modem_processing_sim_complete( (gpointer)user_data, TRUE, (gchar *)sim_imsi->plmn);
		g_free(sim_imsi);
	}

	return TCORE_HOOK_RETURN_CONTINUE;
}

gboolean _ps_hook_co_modem_event(gpointer modem)
{
	Server *s = NULL;
	TcorePlugin *p;
	g_return_val_if_fail(modem != NULL, FALSE);

	p = _ps_modem_ref_plugin(modem);
	s = tcore_plugin_ref_server(p);

	tcore_server_add_notification_hook(s, TNOTI_MODEM_POWER, __on_hook_powered, modem);
	tcore_server_add_notification_hook(s, TNOTI_MODEM_FLIGHT_MODE, __on_hook_flight, modem);
	tcore_server_add_notification_hook(s, TNOTI_SIM_STATUS, __on_hook_sim_init, modem);

	return TRUE;
}

gboolean _ps_get_co_modem_values(gpointer modem)
{
	TcorePlugin *plg;
	CoreObject *co_modem = NULL;
	CoreObject *co_sim = NULL;

	GSList *co_lists = NULL;
	gboolean sim_init = FALSE, modem_powered = FALSE, flight_mode = FALSE;
	int sim_status = 0;
	struct tel_sim_imsi *sim_imsi = NULL;

	g_return_val_if_fail(modem != NULL, FALSE);

	co_modem = _ps_modem_ref_co_modem(modem);
	if (!co_modem)
		return FALSE;

	plg = tcore_object_ref_plugin(co_modem);
	if (!plg)
		return FALSE;

	co_lists = tcore_plugin_get_core_objects_bytype(plg, CORE_OBJECT_TYPE_SIM);
	if (!co_lists)
		return FALSE;

	co_sim = co_lists->data;
	g_slist_free(co_lists);

	sim_status = tcore_sim_get_status(co_sim);
	if(sim_status == SIM_STATUS_INIT_COMPLETED)
		sim_init = TRUE;

	sim_imsi = tcore_sim_get_imsi(co_sim);
	modem_powered = tcore_modem_get_powered(co_modem);
	flight_mode = tcore_modem_get_flight_mode_state(co_modem);

	_ps_modem_processing_flight_mode(modem, flight_mode);
	_ps_modem_processing_power_enable(modem, modem_powered);
	_ps_modem_processing_sim_complete(modem, sim_init, (gchar *)sim_imsi->plmn);

	g_free(sim_imsi);
	return TRUE;
}

gboolean _ps_hook_co_network_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;

	g_return_val_if_fail(service != NULL, FALSE);

	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);

	tcore_server_add_notification_hook(s, TNOTI_NETWORK_REGISTRATION_STATUS, __on_hook_net_register, service);
	tcore_server_add_notification_hook(s, TNOTI_NETWORK_CHANGE, __on_hook_net_change, service);

	return TRUE;
}

gboolean _ps_get_co_network_values(gpointer service)
{
	CoreObject *co_network = NULL;
	gboolean ps_attached = FALSE;

	enum telephony_network_service_domain_status ps_status;
	enum telephony_network_access_technology act;

	g_return_val_if_fail(service != NULL, FALSE);

	co_network = _ps_service_ref_co_network(service);

	tcore_network_get_service_status(co_network, TCORE_NETWORK_SERVICE_DOMAIN_TYPE_PACKET, &ps_status);
	tcore_network_get_access_technology(co_network, &act);

	if (ps_status == NETWORK_SERVICE_DOMAIN_STATUS_FULL)
		ps_attached = TRUE;

	_ps_service_set_roaming(service, tcore_network_get_roaming_state(co_network));
	_ps_service_set_ps_attached(service, ps_attached);
	_ps_service_set_access_technology(service, act);

	return TRUE;
}

gboolean _ps_hook_co_ps_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	g_return_val_if_fail(service != NULL, FALSE);

	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);

	tcore_server_add_notification_hook(s, TNOTI_PS_CALL_STATUS, __on_hook_call_status, service);
	tcore_server_add_notification_hook(s, TNOTI_PS_CURRENT_SESSION_DATA_COUNTER, __on_hook_session_data_counter, service);
	tcore_server_add_notification_hook(s, TNOTI_PS_PDP_IPCONFIGURATION, __on_hook_ipconfiguration, service);

	return TRUE;
}

gboolean _ps_update_cellular_state_key(gpointer service)
{
	Server *s = NULL;
	gpointer handle = NULL;
	static Storage *strg;
	int err_reason = 0;

	s = tcore_plugin_ref_server( (TcorePlugin *)_ps_service_ref_plugin(service) );
	strg = tcore_server_find_storage(s, "vconf");
	handle = tcore_storage_create_handle(strg, "vconf");
	if (!handle){
		err("fail to create vconf handle");
		return FALSE;
	}

	err_reason = _ps_service_check_cellular_state(service);
	tcore_storage_set_int(strg,STORAGE_KEY_CELLULAR_STATE, err_reason);

	return TRUE;
}
