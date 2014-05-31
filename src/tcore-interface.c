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

#include "ps.h"

#include <server.h>
#include <plugin.h>
#include <storage.h>
#include <util.h>
#include <co_ps.h>
#include <co_modem.h>
#include <co_sim.h>
#include <co_network.h>

#include <util.h>

static enum tcore_hook_return __on_hook_call_status(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	struct tnoti_ps_call_status *cstatus = NULL;
	char *ifname;
	gboolean netif_updown = FALSE;
	GSList *contexts;
	CoreObject *co_context;

	dbg("CALL Status event");

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	cstatus = (struct tnoti_ps_call_status *)data;
	g_return_val_if_fail(cstatus != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	dbg("Context ID: [%d] Call State: [%s]", cstatus->context_id,
		((cstatus->state == PS_DATA_CALL_CTX_DEFINED) ? "DEFINED"
			: (cstatus->state == PS_DATA_CALL_CONNECTED) ? "CONNECTED"
			: "NOT CONNECTED"));

	if (cstatus->state == PS_DATA_CALL_CTX_DEFINED)
		goto out;
	else if (cstatus->state == PS_DATA_CALL_CONNECTED)
		netif_updown = TRUE;
	else if (cstatus->state == PS_DATA_CALL_NOT_CONNECTED)
		netif_updown = FALSE;

	/* Refer to context */
	contexts = tcore_ps_ref_context_by_id(source, cstatus->context_id);
	for (; contexts != NULL; contexts = g_slist_next(contexts)) {
		co_context = contexts->data;
		if (co_context == NULL) {
			dbg("Context is NULL");
			continue;
		}

		/* Get Interface name */
		ifname = tcore_context_get_ipv4_devname(co_context);
		if (ifname == NULL) {
			dbg("Interface name is NULL");
			continue;
		}

		/* Setup network interface */
		if (tcore_util_netif(ifname, netif_updown)
					!= TCORE_RETURN_SUCCESS) {
			g_slist_free(contexts);
			g_free(ifname);
			err("Failed to setup interface - Interface name: [%s] Interface Status: [%s]",
					ifname, (netif_updown ? "UP" : "DOWN"));
		}
		dbg("Successfully setup interface - Interface name: [%s] Interface Status: [%s]",
				ifname, (netif_updown ? "UP" : "DOWN"));
	}

out:
	//send activation event / deactivation event
	if (cstatus->state == PS_DATA_CALL_CTX_DEFINED) {			/* OK: PDP define is complete. */
		dbg("Service - [READY TO ACTIVATE]");
		_ps_service_set_ps_defined(service, TRUE, cstatus->context_id);
		//_ps_service_connect_default_context(service);
	}
	else if (cstatus->state == PS_DATA_CALL_CONNECTED) {		/* CONNECTED */
		dbg("Service - [ACTIVATED]");
		_ps_service_set_connected(service, cstatus->context_id, TRUE);
	}
	else if (cstatus->state == PS_DATA_CALL_NOT_CONNECTED) {	/* NO CARRIER */
		dbg("Service - [DEACTIVATED]");
		_ps_service_set_ps_defined(service, FALSE, cstatus->context_id);
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
		dbg("Mismatching PS object");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	dbg("IP Configuration event");
	_ps_service_set_context_info(service, devinfo);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_powered(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	gpointer modem = user_data;
	struct tnoti_modem_power *modem_power = NULL;

	gboolean power = FALSE;

	dbg("Powered event");

	g_return_val_if_fail(modem != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	modem_power = (struct tnoti_modem_power *)data;
	dbg("Modem Power state: [%s]",
		((modem_power->state == MODEM_STATE_ONLINE) ? "ONLINE"
		: (modem_power->state == MODEM_STATE_OFFLINE) ? "OFFLINE" : "ERROR"));

	if ( modem_power->state == MODEM_STATE_ONLINE )
		power = TRUE;
	else
		power = FALSE;

	/* Process modem Power state */
	_ps_modem_processing_power_enable(modem, power);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_flight(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	gpointer modem = user_data;
	struct tnoti_modem_flight_mode *modem_flight = NULL;

	dbg("Flight mode event");

	g_return_val_if_fail(modem != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	modem_flight = (struct tnoti_modem_flight_mode *)data;
	dbg("Flight mode: [%s]", (modem_flight->enable ? "ON" : "OFF"));

	/* Process Flight mode event */
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
	dbg("plmn(%s) act(%d)", network_change->plmn, network_change->act);
	_ps_service_set_access_technology(service, network_change->act);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_sim_init(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	struct tnoti_sim_status *sim_data;

	dbg("SIM INIT event");
	g_return_val_if_fail(user_data != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	sim_data = (struct tnoti_sim_status *)data;
	dbg("SIM status: [0x%02x]", sim_data->sim_status);

	if( sim_data->sim_status == SIM_STATUS_INIT_COMPLETED){
		struct tel_sim_imsi *sim_imsi = NULL;
		sim_imsi = tcore_sim_get_imsi(source);
		_ps_modem_processing_sim_complete((gpointer)user_data, TRUE, (gchar *)sim_imsi->plmn);
		g_free(sim_imsi);
	}

	return TCORE_HOOK_RETURN_CONTINUE;
}

gboolean _ps_hook_co_modem_event(gpointer modem)
{
	Server *s = NULL;
	TcorePlugin *p;
	g_return_val_if_fail(modem != NULL, FALSE);

	dbg("Hook Modem & SIM events");

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

	gboolean sim_init = FALSE, modem_powered = FALSE, flight_mode = FALSE;
	int sim_status = 0;
	struct tel_sim_imsi *sim_imsi = NULL;

	dbg("Extract modem values");

	g_return_val_if_fail(modem != NULL, FALSE);

	co_modem = _ps_modem_ref_co_modem(modem);
	if (!co_modem)
		return FALSE;

	plg = tcore_object_ref_plugin(co_modem);
	if (!plg)
		return FALSE;

	co_sim = tcore_plugin_ref_core_object(plg, CORE_OBJECT_TYPE_SIM);
	if (!co_sim)
		return FALSE;

	/* SIM State */
	sim_status = tcore_sim_get_status(co_sim);
	if(sim_status == SIM_STATUS_INIT_COMPLETED) {
		sim_init = TRUE;

		/*
		 * If SIM State is initialized then fetch the Modem Power,
		 * else wait for Modem Power Notification.
		 */
		modem_powered = tcore_modem_get_powered(co_modem);
	}

	/* IMSI */
	sim_imsi = tcore_sim_get_imsi(co_sim);

	/* Flight mode */
	flight_mode = tcore_modem_get_flight_mode_state(co_modem);

	msg("	SIM init: [%s]", sim_init ? "YES" : "NO");
	msg("	Modem powered: [%s]", modem_powered ? "YES" : "ON");
	msg("	Flight mode: [%s]", flight_mode ? "ON" : "OFF");

	/* Set Flight mode */
	_ps_modem_processing_flight_mode(modem, flight_mode);

	/* Set Power power */
	_ps_modem_processing_power_enable(modem, modem_powered);

	/* Process SIM state */
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

gboolean _ps_free_co_ps_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	g_return_val_if_fail(service != NULL, FALSE);

	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);

	tcore_server_remove_notification_hook(s, __on_hook_call_status);
	tcore_server_remove_notification_hook(s, __on_hook_session_data_counter);
	tcore_server_remove_notification_hook(s, __on_hook_ipconfiguration);

	return TRUE;
}

gboolean _ps_free_co_network_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	g_return_val_if_fail(service != NULL, FALSE);

	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);

	tcore_server_remove_notification_hook(s, __on_hook_net_register);
	tcore_server_remove_notification_hook(s, __on_hook_net_change);

	return TRUE;
}

gboolean _ps_update_cellular_state_key(gpointer service)
{
	Server *s = NULL;
	gpointer handle = NULL;
	static Storage *strg;
	int cur_cell_state = 0;
	int stored_cell_state = 0;

	s = tcore_plugin_ref_server( (TcorePlugin *)_ps_service_ref_plugin(service) );
	strg = tcore_server_find_storage(s, "vconf");
	handle = tcore_storage_create_handle(strg, "vconf");
	if (!handle){
		err("fail to create vconf handle");
		return FALSE;
	}

	cur_cell_state = _ps_service_check_cellular_state(service);
	stored_cell_state = tcore_storage_get_int(strg,STORAGE_KEY_CELLULAR_STATE);
	dbg("cellular state, current (%d), cur_cell_state (%d)", stored_cell_state, cur_cell_state);
	if(stored_cell_state != cur_cell_state)
		tcore_storage_set_int(strg,STORAGE_KEY_CELLULAR_STATE, cur_cell_state);

	return TRUE;
}
