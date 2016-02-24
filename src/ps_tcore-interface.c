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

#include "ps_common.h"

#include <server.h>
#include <plugin.h>
#include <storage.h>
#include <co_ps.h>
#include <co_context.h>
#include <co_modem.h>
#include <co_sim.h>
#include <type/network.h>
#include <co_network.h>
#ifdef POWER_SAVING_FEATURE_WEARABLE
#include <co_call.h>
#endif
#include <user_request.h>

#define TIMEOUT_MAX			1280

enum ps_call_state {
	PS_CALL_STATE_RESULT_OK = 0x00,
	PS_CALL_STATE_RESULT_CONNECT = 0x01,
	PS_CALL_STATE_RESULT_NO_CARRIER = 0x03
};

struct work_queue_data {
	unsigned int id;
	UserRequest *ur;
};

static void __ps_modem_set_hook_flag(ps_modem_t *modem , enum tcore_request_command cmd);
static void __ps_modem_get_mode_pref_change(ps_modem_t *modem, UserRequest *ur);


#ifdef POWER_SAVING_FEATURE_WEARABLE
static gboolean __ps_is_any_call_in_progress(TcorePlugin *plugin, __ps_call_flow_type type, enum tcore_notification_command command);
static enum tcore_hook_return __on_hook_voice_call_status(Server *s, CoreObject *co_call,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data);
#endif


static gboolean ps_util_add_waiting_job(GQueue *queue, unsigned int id, UserRequest *ur)
{
	struct work_queue_data *wqd;

	if (!queue)
		return FALSE;

	wqd = calloc(sizeof(struct work_queue_data), 1);
	if (!wqd)
		return FALSE;

	wqd->id = id;
	wqd->ur = ur;
	g_queue_push_tail(queue, wqd);

	dbg("id = %d, ur = 0x%x", wqd->id, wqd->ur);
	return TRUE;
}

static guint ps_util_get_count_waiting_job(GQueue *queue, unsigned int id)
{
	guint i = 0;
	guint count = 0;
	struct work_queue_data *wqd = NULL;

	if (!queue)
		return count;

	dbg("job count: %d", g_queue_get_length(queue));

	do {
		wqd = g_queue_peek_nth(queue, i);
		if (!wqd)
			break;

		if (wqd->id == id)
			count++;

		i++;
	} while (wqd != NULL);

	dbg("count: %d, id = %d", count, id);

	return count;
}

static UserRequest *ps_util_pop_waiting_job(GQueue *queue, unsigned int id)
{
	int i = 0;
	UserRequest *ur;
	struct work_queue_data *wqd;

	if (!queue)
		return NULL;

	dbg("before waiting job count: %d", g_queue_get_length(queue));

	do {
		wqd = g_queue_peek_nth(queue, i);
		if (!wqd)
			return NULL;

		if (wqd->id == id) {
			wqd = g_queue_pop_nth(queue, i);
			break;
		}

		i++;
	} while (wqd != NULL);

	dbg("after  waiting job count: %d", g_queue_get_length(queue));

	if (!wqd)
		return NULL;

	ur = wqd->ur;
	free(wqd);

	return ur;
}

static gboolean __ps_check_pdp_permanent_reject_cause(int cause)
{
	gboolean ret = TRUE;

	switch (cause) {
	case PS_PDP_PERMANENT_REJECT_OPERATOR_DETERMINED_BARRING:
	case PS_PDP_PERMANENT_REJECT_UNKNOWN_APN:
	case PS_PDP_PERMANENT_REJECT_UNKNOWN_PDP:
	case PS_PDP_PERMANENT_REJECT_AUTH_FAILED:
	case PS_PDP_PERMANENT_REJECT_GGSN_REJECT:
	case PS_PDP_PERMANENT_REJECT_OPTION_NOT_SUPPORTED:
	case PS_PDP_PERMANENT_REJECT_OPTION_UNSUBSCRIBED:
		dbg("Permanent reject cause");
		ret = FALSE;
	break;

	default:
	break;
	}

	return ret;
}

static gboolean __ps_set_network_mode(int mode, void *data)
{
	int c_mode = 0;
	gboolean roaming = FALSE;
	struct treq_network_set_mode req;

	UserRequest *ur = NULL;
	ps_modem_t *modem = data;

	CoreObject *co_network = NULL;

	memset(&req, 0, sizeof(struct treq_network_set_mode));

	co_network = tcore_plugin_ref_core_object(tcore_object_ref_plugin(modem->co_modem),
			CORE_OBJECT_TYPE_NETWORK);
	c_mode = mode;
	dbg("current network mode (%d)", c_mode);

	if (modem->data_allowed)
		c_mode |= NETWORK_MODE_LTE;
	else
		c_mode &= ~NETWORK_MODE_LTE;

	dbg("network mode(%d) - data allowed(%d)", c_mode, modem->data_allowed);

	roaming = tcore_network_get_roaming_state(co_network);
	if (modem->data_allowed && roaming)
		c_mode &= ~NETWORK_MODE_LTE;

	dbg("network mode(%d) - roaming(%d)", c_mode, roaming);

	dbg("candidate mode(%d), current mode(%d)", c_mode, mode);
	if (c_mode == mode) {
		dbg("mode is the same as before, do not send");
		return FALSE;
	}

	req.mode = c_mode;

	ur = tcore_user_request_new(NULL, tcore_plugin_ref_plugin_name(tcore_object_ref_plugin(co_network)));
	tcore_user_request_set_data(ur, sizeof(struct treq_network_set_mode), &req);
	tcore_user_request_set_command(ur, TREQ_NETWORK_SET_MODE);

	if (TCORE_RETURN_SUCCESS != tcore_server_dispatch_request(tcore_plugin_ref_server(tcore_object_ref_plugin(modem->co_modem)), ur)) {
		err("Failed to dispatch ");
		tcore_user_request_unref(ur);
	}

	return TRUE;
}

/* Function will be used in case any dispatch request failed in ps plugin */
static void __ps_send_ur_dispatch_failure_response(UserRequest *ur, enum tcore_response_command command)
{
	dbg("User request dispatch failed so need to send response for command [%d]", command);
	switch (command) {
	case TRESP_NETWORK_SEARCH: {
		struct tresp_network_search search_rsp;
		memset(&search_rsp, 0, sizeof(struct tresp_network_search));

		search_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
		search_rsp.list_count = 0;
		tcore_user_request_send_response(ur, TRESP_NETWORK_SEARCH,
			sizeof(struct tresp_network_search), &search_rsp);
	}
	break;

	case TRESP_NETWORK_SET_PLMN_SELECTION_MODE: {
		struct tresp_network_set_plmn_selection_mode set_plmn_selection_mode_rsp;
		memset(&set_plmn_selection_mode_rsp, 0, sizeof(struct tresp_network_set_plmn_selection_mode));

		set_plmn_selection_mode_rsp.result =  TCORE_RETURN_FAILURE;
		tcore_user_request_send_response(ur, TRESP_NETWORK_SET_PLMN_SELECTION_MODE,
			sizeof(struct tresp_network_set_plmn_selection_mode), &set_plmn_selection_mode_rsp);
	}
	break;

	case TRESP_NETWORK_SET_MODE: {
		struct tresp_network_set_mode set_rsp;
		memset(&set_rsp, 0, sizeof(struct tresp_network_set_mode));

		set_rsp.result =  TCORE_RETURN_FAILURE;
		tcore_user_request_send_response(ur, TRESP_NETWORK_SET_MODE,
			sizeof(struct tresp_network_search), &set_rsp);
	}
	break;

	case TRESP_NETWORK_GET_MODE: {
		struct tresp_network_get_mode get_rsp;
		memset(&get_rsp, 0, sizeof(struct tresp_network_get_mode));

		get_rsp.result =  TCORE_RETURN_FAILURE;
		tcore_user_request_send_response(ur, TRESP_NETWORK_GET_MODE,
			sizeof(struct tresp_network_get_mode), &get_rsp);
	}
	break;

	case TRESP_MODEM_POWER_OFF: {
		struct tresp_modem_power_off set_power_off_rsp;
		memset(&set_power_off_rsp, 0, sizeof(struct tresp_modem_power_off));

		set_power_off_rsp.result =  TCORE_RETURN_FAILURE;
		tcore_user_request_send_response(ur, TRESP_MODEM_POWER_OFF,
			sizeof(struct tresp_modem_power_off), &set_power_off_rsp);
	}
	break;

	case TRESP_MODEM_POWER_LOW: {
		struct tresp_modem_power_low set_power_low_rsp;
		memset(&set_power_low_rsp, 0, sizeof(struct tresp_modem_power_low));

		set_power_low_rsp.result =  TCORE_RETURN_FAILURE;
		tcore_user_request_send_response(ur, TRESP_MODEM_POWER_LOW,
			sizeof(struct tresp_modem_power_low), &set_power_low_rsp);
	}
	break;

	case TRESP_MODEM_SET_FLIGHTMODE: {
		struct tresp_modem_set_flightmode set_flight_mode_rsp;
		memset(&set_flight_mode_rsp, 0, sizeof(struct tresp_modem_set_flightmode));

		set_flight_mode_rsp.result =  TCORE_RETURN_FAILURE;
		tcore_user_request_send_response(ur, TRESP_MODEM_SET_FLIGHTMODE,
			sizeof(struct tresp_modem_set_flightmode), &set_flight_mode_rsp);
	}
	break;

	default:
		err("Command type not expected [%d]", command);
	}
	/* Unref User request */
	tcore_user_request_unref(ur);
}

void __ps_hook_response_cb(UserRequest *ur, enum tcore_response_command command,
	unsigned int data_len, const void *data, void *user_data)
{
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);
	guint count;
	guint id;
	id = ((command & ~TCORE_RESPONSE) & TCORE_REQUEST);

	ps_dbg_ex_co(co_modem, "Entered");
	count = ps_util_get_count_waiting_job(modem->work_queue, id);

	if (count != 0) {
		ur = ps_util_pop_waiting_job(modem->work_queue, id);
		if (ur) {
			CoreObject *co_network = NULL;
			TReturn ret = TCORE_RETURN_SUCCESS;

			co_network = tcore_plugin_ref_core_object(tcore_object_ref_plugin(modem->co_modem),
				CORE_OBJECT_TYPE_NETWORK);

			ps_dbg_ex_co(co_modem, "Sending Pending Request of type = id", id);
			tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
			if ((command == TRESP_NETWORK_SET_CANCEL_MANUAL_SEARCH)
					|| (command == TRESP_NETWORK_SEARCH)
					|| (command == TRESP_NETWORK_SET_PLMN_SELECTION_MODE)
					|| (command == TRESP_NETWORK_SET_MODE)
					|| (command == TRESP_NETWORK_GET_MODE))
				ret = tcore_object_dispatch_request(co_network , ur);
			else if ((command == TRESP_MODEM_SET_FLIGHTMODE)
					|| (command == TRESP_MODEM_POWER_LOW)
					|| (command == TRESP_MODEM_POWER_OFF))
				ret = tcore_object_dispatch_request(modem->co_modem , ur);
			if (TCORE_RETURN_SUCCESS != ret) {
				/* send responce wrt to command */
				err("Failed to dispatch request, need to sent response to dbus");
				__ps_send_ur_dispatch_failure_response(ur, command);
			}
			return;
		}
	}

	switch (command) {
	case TRESP_NETWORK_SET_CANCEL_MANUAL_SEARCH:
	case TRESP_NETWORK_SEARCH:
		ps_dbg_ex_co(co_modem, "TRESP_NETWORK_SEARCH  response received");
		if (count == 0)
			modem->hook_flag &= PS_RESET_NETWORK_SEARCH_FLAG;
	break;

	case TRESP_NETWORK_SET_PLMN_SELECTION_MODE:
		ps_dbg_ex_co(co_modem, "TRESP_NETWORK_SET_PLMN_SELECTION_MODE response received ");
		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_SELECTION_FLAG;
	break;

	case TRESP_NETWORK_SET_MODE: {
		ps_dbg_ex_co(co_modem, "TRESP_NETWORK_SET_MODE response received ");

		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_SELECT_MODE_FLAG;
	}
	break;

	case TRESP_NETWORK_GET_MODE: {
		gboolean rv = FALSE;
		const struct tresp_network_get_mode *resp_get_mode = data;
		dbg("TRESP_NETWORK_GET_MODE response received mode (mode:[%d])", resp_get_mode->mode);

		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_GET_MODE_FLAG;

		rv = __ps_set_network_mode(resp_get_mode->mode, modem);
		if (rv) {
			dbg("network set mode request!");
			return;
		}
	}
	break;

	case TRESP_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION:
		ps_dbg_ex_co(co_modem, "TRESP_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION response received ");
		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_SET_DEFAULT_DATA_SUBS;
	break;

	case TRESP_MODEM_SET_FLIGHTMODE:
		ps_dbg_ex_co(co_modem, "TRESP_MODEM_SET_FLIGHTMODE response received ");
		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_SET_FLIGHT_MODE_FLAG;
	break;

	case TRESP_MODEM_POWER_LOW:
		ps_dbg_ex_co(co_modem, "TRESP_MODEM_POWER_LOW response received ");
		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_SET_POWER_LOW_FLAG;
	break;

	case TRESP_MODEM_POWER_OFF:
		ps_dbg_ex_co(co_modem, "TRESP_MODEM_POWER_OFF response received ");
		if (count == 0)
			modem->hook_flag &= PS_NETWORK_RESET_SET_POWER_OFF_FLAG;
	break;

	case TRESP_SIM_SET_POWERSTATE:
		ps_dbg_ex_co(co_modem, "TRESP_SIM_SET_POWERSTATE response received ");
		if (count == 0)
			modem->hook_flag &= PS_SIM_SET_POWER_STATE_FLAG;
	break;

	default: {
		ps_dbg_ex_co(co_modem, "Unexpected response ");
	}
	break;
	}
	ps_dbg_ex_co(co_modem, " FLAG %x", modem->hook_flag);

	if (modem->hook_flag == PS_NO_PENDING_REQUEST
		&& command != TRESP_MODEM_POWER_LOW
		 && command != TRESP_MODEM_POWER_OFF
		 && modem->mode_pref_changed == FALSE) {
		_ps_modem_set_data_allowed(modem, modem->data_allowed);
	}
}

void __ps_modem_get_mode_pref_change(ps_modem_t *modem, UserRequest *ur)
{
	enum telephony_network_service_type svc_type;
	enum tcore_request_command cmd;
	CoreObject *co_network = NULL;
	const struct treq_network_set_mode *req;

	cmd = tcore_user_request_get_command(ur);
	if (cmd != TREQ_NETWORK_SET_MODE) {
		err("Not a TREQ_NETWORK_SET_MODE");
		modem->mode_pref_changed = FALSE;
		return;
	}
	modem->mode_pref_changed = TRUE;

	co_network = tcore_plugin_ref_core_object(tcore_object_ref_plugin(modem->co_modem),
		CORE_OBJECT_TYPE_NETWORK);
	req = tcore_user_request_ref_data(ur, NULL);

	tcore_network_get_service_type(co_network, &svc_type);
	dbg("mode_pref[0x%x], svc_type[%d]", req->mode, svc_type);
	switch (svc_type) {
	case NETWORK_SERVICE_TYPE_2G:
	case NETWORK_SERVICE_TYPE_2_5G:
	case NETWORK_SERVICE_TYPE_2_5G_EDGE: {
		if (req->mode == NETWORK_MODE_GSM)
			modem->mode_pref_changed = FALSE;
	}
	break;

	case NETWORK_SERVICE_TYPE_3G:
	case NETWORK_SERVICE_TYPE_HSDPA: {
		if (req->mode & NETWORK_MODE_WCDMA)
			modem->mode_pref_changed = FALSE;
	}
	break;

	case NETWORK_SERVICE_TYPE_LTE: {
		if (req->mode & NETWORK_MODE_LTE)
			modem->mode_pref_changed = FALSE;
	}
	break;

	default:
	break;
	}

	dbg("mode_pref_changed : %d", modem->mode_pref_changed);
}

void __ps_modem_cp_reset_send_pending_request_response(gpointer data)
{
	gpointer *queue_data = NULL;
	ps_modem_t *modem = data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);

	ps_dbg_ex_co(co_modem, "Entered");
	queue_data = g_queue_pop_head(modem->work_queue);
	while (queue_data) {
		struct work_queue_data *wqd = (struct work_queue_data *)queue_data;
		if (wqd->ur) {
			enum tcore_request_command cmd = tcore_user_request_get_command(wqd->ur);

			if (cmd == TREQ_NETWORK_SEARCH) {
				struct tresp_network_search search_rsp;
				memset(&search_rsp, 0, sizeof(struct tresp_network_search));

				search_rsp.result = TCORE_RETURN_FAILURE;
				search_rsp.list_count = 0;
				tcore_user_request_send_response(wqd->ur, TRESP_NETWORK_SEARCH,
				sizeof(struct tresp_network_search), &search_rsp);
			} else if (cmd == TREQ_NETWORK_SET_PLMN_SELECTION_MODE) {
				struct tresp_network_set_plmn_selection_mode set_plmn_mode_rsp;
				memset(&set_plmn_mode_rsp, 0, sizeof(struct tresp_network_set_plmn_selection_mode));

				set_plmn_mode_rsp.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_NETWORK_SET_PLMN_SELECTION_MODE,
				sizeof(struct tresp_network_set_plmn_selection_mode), &set_plmn_mode_rsp);
			} else if (cmd == TREQ_NETWORK_SET_MODE) {
				struct tresp_network_set_mode setmode_rsp;
				memset(&setmode_rsp, 0, sizeof(struct tresp_network_set_mode));

				setmode_rsp.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_NETWORK_SET_MODE,
				sizeof(struct tresp_network_set_mode), &setmode_rsp);
			} else if (cmd == TREQ_NETWORK_SET_CANCEL_MANUAL_SEARCH) {
				struct tresp_network_set_cancel_manual_search search_cancel_rsp;
				memset(&search_cancel_rsp, 0, sizeof(struct tresp_network_set_cancel_manual_search));

				search_cancel_rsp.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_NETWORK_SET_CANCEL_MANUAL_SEARCH,
				sizeof(struct tresp_network_set_cancel_manual_search), &search_cancel_rsp);
			} else if (cmd == TREQ_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION) {
				struct tresp_network_set_default_data_subscription default_data_rsp;
				memset(&default_data_rsp, 0, sizeof(struct tresp_network_set_default_data_subscription));

				default_data_rsp.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION,
				sizeof(struct tresp_network_set_default_data_subscription), &default_data_rsp);
			} else if (cmd == TREQ_MODEM_SET_FLIGHTMODE) {
				struct tresp_modem_set_flightmode set_flight_mode;
				memset(&set_flight_mode, 0, sizeof(struct tresp_modem_set_flightmode));

				set_flight_mode.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_MODEM_SET_FLIGHTMODE,
				sizeof(struct tresp_modem_set_flightmode), &set_flight_mode);

			} else if (cmd == TREQ_MODEM_POWER_OFF) {
				struct tresp_modem_power_off set_power_off;
				memset(&set_power_off,  0, sizeof(struct tresp_modem_power_off));

				set_power_off.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_MODEM_POWER_OFF,
				sizeof(struct tresp_modem_power_off), &set_power_off);

			} else if (cmd == TREQ_MODEM_POWER_LOW) {
				struct tresp_modem_power_low set_power_low;
				memset(&set_power_low, 0, sizeof(struct tresp_modem_power_low));

				set_power_low.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_MODEM_POWER_LOW,
				sizeof(struct tresp_modem_power_low), &set_power_low);

			} else if (cmd == TREQ_SIM_SET_POWERSTATE) {
				struct tresp_sim_set_powerstate set_power;
				memset(&set_power, 0, sizeof(struct tresp_sim_set_powerstate));

				set_power.result = TCORE_RETURN_FAILURE;
				tcore_user_request_send_response(wqd->ur, TRESP_SIM_SET_POWERSTATE,
				sizeof(struct tresp_sim_set_powerstate), &set_power);

			} else {
				err("Unexpected command ");
			}
			tcore_user_request_unref(wqd->ur);

			/* Memory Free */
			free(wqd);
		}
		queue_data = g_queue_pop_head(modem->work_queue);
	}
}

static void __ps_modem_cp_reset_handler(gpointer object)
{
	ps_modem_t *modem = object;

	dbg("Entred");
	/* check for any pending request in modem queue and respond with error */
	__ps_modem_cp_reset_send_pending_request_response(modem);

	/* reset modem flag */
	modem->hook_flag  &= PS_NO_PENDING_REQUEST;
}


void __ps_modem_set_hook_flag(ps_modem_t *modem , enum tcore_request_command cmd)
{
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);

	switch (cmd) {
	case TREQ_NETWORK_SEARCH:
		ps_dbg_ex_co(co_modem, "TREQ_NETWORK_SEARCH");
		modem->hook_flag |= PS_NETWORK_SEARCH_PENDING;
		ps_dbg_ex_co(co_modem, "TREQ_NETWORK_SEARCH setting flag %x", modem->hook_flag);
	break;

	case TREQ_NETWORK_SET_PLMN_SELECTION_MODE:
		modem->hook_flag |= PS_NETWORK_SELECTION_PENDING;
		ps_dbg_ex_co(co_modem, "TREQ_NETWORK_SET_PLMN_SELECTION_MODE setting flag %x", modem->hook_flag);
	break;

	case TREQ_NETWORK_SET_MODE:
		modem->hook_flag |= PS_NETWORK_SELECT_MODE;
		ps_dbg_ex_co(co_modem, "TREQ_NETWORK_SET_MODE setting flag %x", modem->hook_flag);
	break;

	case TREQ_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION:
		modem->hook_flag |= PS_NETWORK_SET_DEFAULT_DATA_SUBS;
		ps_dbg_ex_co(co_modem, "TREQ_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION setting flag %x", modem->hook_flag);
	break;

	case TREQ_MODEM_SET_FLIGHTMODE:
		modem->hook_flag |= PS_NETWORK_SET_FLIGHT_MODE;
		ps_dbg_ex_co(co_modem, "TREQ_MODEM_SET_FLIGHTMODE setting flag %x", modem->hook_flag);
	break;

	case TREQ_MODEM_POWER_OFF:
		modem->hook_flag |= PS_NETWORK_SET_POWER_OFF;
		ps_dbg_ex_co(co_modem, "TREQ_MODEM_POWER_OFF setting flag %x", modem->hook_flag);
	break;

	case TREQ_MODEM_POWER_LOW:
		modem->hook_flag |= PS_NETWORK_SET_POWER_LOW;
		ps_dbg_ex_co(co_modem, "TREQ_MODEM_POWER_LOW setting flag %x", modem->hook_flag);
	break;

	case TREQ_SIM_SET_POWERSTATE:
		modem->hook_flag |= PS_SIM_SET_POWER_STATE;
		ps_dbg_ex_co(co_modem, "TREQ_SIM_SET_POWERSTATE setting flag %x", modem->hook_flag);
	break;

	default:
		ps_dbg_ex_co(co_modem, "Not handled request");
	break;
	}
}

enum tcore_hook_return ps_handle_hook(Server *s, UserRequest *ur, void *user_data)
{
	gboolean ret = FALSE;
	TReturn rv = TCORE_RETURN_FAILURE;

	CoreObject *co_ps = NULL;
	TcorePlugin *target_plg = NULL;
	int value = 0;
	guint job_cnt = 0;
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);

	char *modem_name = NULL;
	enum tcore_request_command cmd = tcore_user_request_get_command(ur);

	ps_dbg_ex_co(co_modem, "Entered");

	modem_name = tcore_user_request_get_modem_name(ur);
	if (!modem_name)
		return TCORE_HOOK_RETURN_CONTINUE;

	target_plg = tcore_object_ref_plugin(modem->co_modem);
	if (g_strcmp0(tcore_server_get_cp_name_by_plugin(target_plg), modem_name) != 0) {
		ps_dbg_ex_co(co_modem, "request modem (%s) not matched current modem(%s)",
				modem_name,
				tcore_server_get_cp_name_by_plugin(target_plg));
		free(modem_name);
		if (cmd == TREQ_NETWORK_SEARCH || cmd == TREQ_MODEM_SET_FLIGHTMODE) {
			co_ps = tcore_plugin_ref_core_object(target_plg, CORE_OBJECT_TYPE_PS);
			if (!co_ps) {
				ps_dbg_ex_co(co_modem, "No ps core object present ");
				return TCORE_HOOK_RETURN_CONTINUE;
			}

			if (FALSE == tcore_ps_any_context_activating_activated(co_ps, &value)) {
				ps_dbg_ex_co(co_modem, "No activating/activated context present");
				/* Block PS always-on while network operations. */
				__ps_modem_set_hook_flag(modem, cmd);
				tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
				return TCORE_HOOK_RETURN_CONTINUE;
			}

			ps_dbg_ex_co(co_modem, "Value returned [%d]", value);
			if (CONTEXT_STATE_ACTIVATED == value) {
				ps_dbg_ex_co(co_modem, "Activated/Activating context present need to deactivate them");
				rv = tcore_ps_deactivate_contexts(co_ps);
				if (rv != TCORE_RETURN_SUCCESS) {
					ps_dbg_ex_co(co_modem, "fail to deactivation");
					return TCORE_HOOK_RETURN_CONTINUE;
				}
			} else if (CONTEXT_STATE_ACTIVATING == value) {
				if (cmd == TREQ_MODEM_SET_FLIGHTMODE)  {
					ps_dbg_ex_co(co_modem, "No need to stop these request for pdp in activating state ");
					return TCORE_HOOK_RETURN_CONTINUE;
				}
				ps_dbg_ex_co(co_modem, "For rest command will wait for activation successful ");
			}
			if (!modem->work_queue) {
				ps_err_ex_co(co_modem, "no queue present unable to handle request");
				return TCORE_HOOK_RETURN_CONTINUE;
			}

			job_cnt = ps_util_get_count_waiting_job(modem->work_queue, cmd);
			if (job_cnt) {
				ps_err_ex_co(co_modem, "duplicated job for cmd(%d)", cmd);

				if (cmd == TREQ_NETWORK_SEARCH) {
					struct tresp_network_search search_rsp;
					memset(&search_rsp, 0, sizeof(struct tresp_network_search));

					search_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
					search_rsp.list_count = 0;
					tcore_user_request_send_response(ur, TRESP_NETWORK_SEARCH,
						sizeof(struct tresp_network_search), &search_rsp);
				} else if (cmd == TREQ_MODEM_SET_FLIGHTMODE) {
					struct tresp_modem_set_flightmode set_flight_mode;
					memset(&set_flight_mode, 0, sizeof(struct tresp_modem_set_flightmode));

					set_flight_mode.result = TCORE_RETURN_OPERATION_ABORTED;
					tcore_user_request_send_response(ur, TRESP_MODEM_SET_FLIGHTMODE,
						sizeof(struct tresp_modem_set_flightmode), &set_flight_mode);
				}
				tcore_user_request_unref(ur);
				return TCORE_HOOK_RETURN_STOP_PROPAGATION;
			}
			ret = ps_util_add_waiting_job(modem->work_queue, cmd , ur);
			if (!ret) {
				ps_dbg_ex_co(co_modem, "fail to add the request to queue");
				return TCORE_HOOK_RETURN_CONTINUE;
			}

			__ps_modem_get_mode_pref_change(modem, ur);
			__ps_modem_set_hook_flag(modem, cmd);
			return TCORE_HOOK_RETURN_STOP_PROPAGATION;
		} else if (cmd == TREQ_NETWORK_SET_CANCEL_MANUAL_SEARCH) {
			UserRequest *ur_pending = NULL;
			ur_pending = ps_util_pop_waiting_job(modem->work_queue, TREQ_NETWORK_SEARCH);

			if (!ur_pending) {
				ps_dbg_ex_co(co_modem, "no pendig search request");
				tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
				return TCORE_HOOK_RETURN_CONTINUE;
			} else {
				struct tresp_network_search search_rsp;
				struct tresp_network_set_cancel_manual_search search_cancel_rsp;

				memset(&search_rsp, 0, sizeof(struct tresp_network_search));
				memset(&search_cancel_rsp, 0, sizeof(struct tresp_network_set_cancel_manual_search));

				search_rsp.list_count = 0;
				ps_dbg_ex_co(co_modem, "send search response to upper layer");
				tcore_user_request_send_response(ur_pending, TRESP_NETWORK_SEARCH, sizeof(struct tresp_network_search), &search_rsp);
				tcore_user_request_unref(ur_pending);

				tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
				search_cancel_rsp.result = TCORE_RETURN_SUCCESS;
				tcore_user_request_send_response(ur, TRESP_NETWORK_SET_CANCEL_MANUAL_SEARCH,
					sizeof(struct tresp_network_set_cancel_manual_search), &search_cancel_rsp);

				return TCORE_HOOK_RETURN_STOP_PROPAGATION;
			}
		}
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	if (modem_name)
		free(modem_name);

	co_ps = tcore_plugin_ref_core_object(target_plg, CORE_OBJECT_TYPE_PS);
	if (!co_ps)
		return TCORE_HOOK_RETURN_CONTINUE;

	if (cmd == TREQ_MODEM_POWER_ON) {
		if (modem->powered == PS_MODEM_STATE_ONLINE) {
			struct tresp_modem_power_on set_power_on;
			memset(&set_power_on,  0, sizeof(struct tresp_modem_power_on));
			dbg("FLAG: 0x%x", modem->hook_flag);

			if (modem->hook_flag & PS_NETWORK_SET_POWER_LOW) {
				dbg("LOW power request is pending, send abort response");
				set_power_on.result = TCORE_RETURN_OPERATION_ABORTED;
				tcore_user_request_send_response(ur, TRESP_MODEM_POWER_ON,
					sizeof(struct tresp_modem_power_on), &set_power_on);
			} else {
				dbg("No pending LOW power request, send success response.");
				set_power_on.result = TCORE_RETURN_EALREADY;
				tcore_user_request_send_response(ur, TRESP_MODEM_POWER_ON,
					sizeof(struct tresp_modem_power_on), &set_power_on);
			}
			tcore_user_request_unref(ur);
			return TCORE_HOOK_RETURN_STOP_PROPAGATION;
		}
		return TCORE_HOOK_RETURN_CONTINUE;
	}
	if (FALSE == tcore_ps_any_context_activating_activated(co_ps, &value)) {
		ps_dbg_ex_co(co_modem, "No activating/activated context present");
		/* Block PS always-on while network operations. */
#ifdef POWER_SAVING_FEATURE_WEARABLE
		if ((cmd != TREQ_MODEM_POWER_LOW)
				|| (FALSE == __ps_is_any_call_in_progress(tcore_object_ref_plugin(modem->co_modem), ON_REQUEST, TNOTI_UNKNOWN))) {
			__ps_modem_set_hook_flag(modem, cmd);
			tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
			return TCORE_HOOK_RETURN_CONTINUE;
		}
#else
		__ps_modem_get_mode_pref_change(modem, ur);
		__ps_modem_set_hook_flag(modem, cmd);
		tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
		return TCORE_HOOK_RETURN_CONTINUE;

#endif
	}

	ps_dbg_ex_co(co_modem, "Value returned [%d]", value);
	if (CONTEXT_STATE_ACTIVATED == value) {
		ps_dbg_ex_co(co_modem, "Activated/Activating context present need to deactivate them");
		rv = tcore_ps_deactivate_contexts(co_ps);
		if (rv != TCORE_RETURN_SUCCESS) {
			ps_dbg_ex_co(co_modem, "fail to deactivation");
			return TCORE_HOOK_RETURN_CONTINUE;
		}
	} else if (CONTEXT_STATE_ACTIVATING == value) {
#ifdef POWER_SAVING_FEATURE_WEARABLE
		if ((cmd != TREQ_MODEM_POWER_LOW)
				|| (FALSE == __ps_is_any_call_in_progress(tcore_object_ref_plugin(modem->co_modem), ON_REQUEST, TNOTI_UNKNOWN))) {
			return TCORE_HOOK_RETURN_CONTINUE;
		}
#else
		if ((cmd == TREQ_MODEM_SET_FLIGHTMODE) || (cmd == TREQ_MODEM_POWER_OFF)) {
			ps_dbg_ex_co(co_modem, "No need to stop these request for pdp in activating state ");
			return TCORE_HOOK_RETURN_CONTINUE;
		}
#endif
		ps_dbg_ex_co(co_modem, "For rest command will wait for activation successful ");
	}

	if (!modem->work_queue) {
		ps_err_ex_co(co_modem, "no queue present unable to handle request");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	job_cnt = ps_util_get_count_waiting_job(modem->work_queue, cmd);
	if (job_cnt) {
		ps_err_ex_co(co_modem, "duplicated job for cmd(%d)", cmd);

		if (cmd == TREQ_NETWORK_SEARCH) {
			struct tresp_network_search search_rsp;
			memset(&search_rsp, 0, sizeof(struct tresp_network_search));

			search_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
			search_rsp.list_count = 0;
			tcore_user_request_send_response(ur, TRESP_NETWORK_SEARCH,
				sizeof(struct tresp_network_search), &search_rsp);
		} else if (cmd == TREQ_NETWORK_SET_PLMN_SELECTION_MODE) {
			struct tresp_network_set_plmn_selection_mode set_plmn_mode_rsp;
			memset(&set_plmn_mode_rsp, 0, sizeof(struct tresp_network_set_plmn_selection_mode));

			set_plmn_mode_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_NETWORK_SET_PLMN_SELECTION_MODE,
				sizeof(struct tresp_network_set_plmn_selection_mode), &set_plmn_mode_rsp);
		} else if (cmd == TREQ_NETWORK_SET_MODE) {
			struct tresp_network_set_mode setmode_rsp;
			memset(&setmode_rsp, 0, sizeof(struct tresp_network_set_mode));

			setmode_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_NETWORK_SET_MODE,
				sizeof(struct tresp_network_set_mode), &setmode_rsp);
		} else if (cmd == TREQ_NETWORK_SET_CANCEL_MANUAL_SEARCH) {
			struct tresp_network_set_cancel_manual_search search_cancel_rsp;
			memset(&search_cancel_rsp, 0, sizeof(struct tresp_network_set_cancel_manual_search));

			search_cancel_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_NETWORK_SET_CANCEL_MANUAL_SEARCH,
				sizeof(struct tresp_network_set_cancel_manual_search), &search_cancel_rsp);
		} else if (cmd == TREQ_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION) {
			struct tresp_network_set_default_data_subscription default_data_rsp;
			memset(&default_data_rsp, 0, sizeof(struct tresp_network_set_default_data_subscription));

			default_data_rsp.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION,
				sizeof(struct tresp_network_set_default_data_subscription), &default_data_rsp);
		} else if (cmd == TREQ_MODEM_SET_FLIGHTMODE) {
			struct tresp_modem_set_flightmode set_flight_mode;
			memset(&set_flight_mode, 0, sizeof(struct tresp_modem_set_flightmode));

			set_flight_mode.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_MODEM_SET_FLIGHTMODE,
				sizeof(struct tresp_modem_set_flightmode), &set_flight_mode);

		} else if (cmd == TREQ_MODEM_POWER_OFF) {
			struct tresp_modem_power_off set_power_off;
			memset(&set_power_off,  0, sizeof(struct tresp_modem_power_off));

			set_power_off.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_MODEM_POWER_OFF,
				sizeof(struct tresp_modem_power_off), &set_power_off);

		} else if (cmd == TREQ_MODEM_POWER_LOW) {
			struct tresp_modem_power_low set_power_low;
			memset(&set_power_low, 0, sizeof(struct tresp_modem_power_low));

			set_power_low.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_MODEM_POWER_LOW,
				sizeof(struct tresp_modem_power_low), &set_power_low);

		} else if (cmd == TREQ_SIM_SET_POWERSTATE) {
			struct tresp_sim_set_powerstate set_power;
			memset(&set_power, 0, sizeof(struct tresp_sim_set_powerstate));

			set_power.result = TCORE_RETURN_OPERATION_ABORTED;
			tcore_user_request_send_response(ur, TRESP_SIM_SET_POWERSTATE,
				sizeof(struct tresp_sim_set_powerstate), &set_power);

		}
		tcore_user_request_unref(ur);
		return TCORE_HOOK_RETURN_STOP_PROPAGATION;
	}

	ps_dbg_ex_co(co_modem, "Deactivation request is sent, wait for call disconnect notification ");

	if (TREQ_NETWORK_SET_CANCEL_MANUAL_SEARCH == cmd) {
		UserRequest *ur_pending = NULL;
		ur_pending = ps_util_pop_waiting_job(modem->work_queue, TREQ_NETWORK_SEARCH);

		if (!ur_pending) {
			ps_dbg_ex_co(co_modem, "no pendig search request");
			tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
			return TCORE_HOOK_RETURN_CONTINUE;
		} else {
			struct tresp_network_search search_rsp;
			struct tresp_network_set_cancel_manual_search search_cancel_rsp;

			memset(&search_rsp, 0, sizeof(struct tresp_network_search));
			memset(&search_cancel_rsp, 0, sizeof(struct tresp_network_set_cancel_manual_search));

			search_rsp.list_count = 0;
			ps_dbg_ex_co(co_modem, "send search response to upper layer");
			tcore_user_request_send_response(ur_pending, TRESP_NETWORK_SEARCH, sizeof(struct tresp_network_search), &search_rsp);
			tcore_user_request_unref(ur_pending);

			tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
			search_cancel_rsp.result = TCORE_RETURN_SUCCESS;
			tcore_user_request_send_response(ur, TRESP_NETWORK_SET_CANCEL_MANUAL_SEARCH,
				sizeof(struct tresp_network_set_cancel_manual_search), &search_cancel_rsp);

			return TCORE_HOOK_RETURN_STOP_PROPAGATION;
		}
	}

	ret = ps_util_add_waiting_job(modem->work_queue, cmd , ur);
	if (!ret) {
		ps_dbg_ex_co(co_modem, "fail to add the request to queue");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	__ps_modem_get_mode_pref_change(modem, ur);
	__ps_modem_set_hook_flag(modem, cmd);
	return TCORE_HOOK_RETURN_STOP_PROPAGATION;
}

void __ps_send_pending_user_request(gpointer data)
{
	ps_modem_t *modem =  data;
	CoreObject *co_network = NULL;
	CoreObject *co_sim = NULL;
	gpointer *queue_data = NULL;

	co_network = tcore_plugin_ref_core_object(tcore_object_ref_plugin(modem->co_modem),
			CORE_OBJECT_TYPE_NETWORK);
	co_sim = tcore_plugin_ref_core_object(tcore_object_ref_plugin(modem->co_modem),
			CORE_OBJECT_TYPE_SIM);

	ps_dbg_ex_co(co_network, "Extracting the user request from the work queue");

	queue_data = g_queue_pop_head(modem->work_queue);
	while (queue_data) {
		struct work_queue_data *wqd = (struct work_queue_data *)queue_data;
		ps_dbg_ex_co(co_network, " sending Pending request [%x]", wqd->id);
		if (wqd->ur) {
			ps_dbg_ex_co(co_network, "Setting responce hook for request ");
			tcore_user_request_set_response_hook(wqd->ur, __ps_hook_response_cb, modem);

			switch (wqd->id) {
			case TREQ_NETWORK_SEARCH:
			case TREQ_NETWORK_SET_MODE:
			case TREQ_NETWORK_SET_PLMN_SELECTION_MODE:
			case TREQ_NETWORK_SET_DEFAULT_DATA_SUBSCRIPTION: {
				char *modem_name;
				TcorePlugin *modem_plugin, *ur_modem_plugin;

				modem_name = tcore_user_request_get_modem_name(wqd->ur);
				if (!modem_name)
					return;

				modem_plugin = tcore_object_ref_plugin(modem->co_modem);
				ur_modem_plugin = tcore_server_find_plugin(tcore_plugin_ref_server(modem_plugin), modem_name);
				if (modem_plugin != ur_modem_plugin) {
					ps_dbg_ex_co(co_network, "request modem (%s) not matched current modem(%s)",
							modem_name, tcore_server_get_cp_name_by_plugin(modem_plugin));
					/* Network search request enqueued for other sim, so dispatch it */
					if (wqd->id == TREQ_NETWORK_SEARCH) {
						CoreObject *co_other_network;

						co_other_network = tcore_plugin_ref_core_object(ur_modem_plugin, CORE_OBJECT_TYPE_NETWORK);
						if (co_other_network == NULL) {
							ps_warn_ex_co(co_network, "No network core object in other modem plugin");
						} else {
							if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(co_other_network, wqd->ur))
								err("Failed to dispatch ");
						}
						free(modem_name);
						break;
					} else {
						free(modem_name);
						return;
					}
				}
				free(modem_name);
				if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(co_network, wqd->ur))
					err("Failed to dispatch ");
			}
			break;

			case TREQ_MODEM_SET_FLIGHTMODE:
			case TREQ_MODEM_POWER_OFF: {
				char *modem_name;
				TcorePlugin *modem_plugin, *ur_modem_plugin;

				modem_name = tcore_user_request_get_modem_name(wqd->ur);
				if (!modem_name)
					return;

				modem_plugin = tcore_object_ref_plugin(modem->co_modem);
				ur_modem_plugin = tcore_server_find_plugin(tcore_plugin_ref_server(modem_plugin), modem_name);
				if (modem_plugin != ur_modem_plugin) {
					ps_dbg_ex_co(co_network, "request modem (%s) not matched current modem(%s)",
							modem_name, tcore_server_get_cp_name_by_plugin(modem_plugin));
					/* Flight mode request enqueued for other sim, so dispatch it */
					if (wqd->id == TREQ_MODEM_SET_FLIGHTMODE) {
						CoreObject *co_other_modem;

						co_other_modem = tcore_plugin_ref_core_object(ur_modem_plugin, CORE_OBJECT_TYPE_MODEM);
						if (co_other_modem == NULL) {
							ps_warn_ex_co(co_network, "No modem core object in other modem plugin");
						} else {
							if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(co_other_modem, wqd->ur))
								err("Failed to dispatch ");
						}
						free(modem_name);
						break;
					} else {
						free(modem_name);
						return;
					}
				}
				free(modem_name);

				if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(modem->co_modem, wqd->ur))
					err("Failed to dispatch ");
			}
			break;

			case TREQ_MODEM_POWER_LOW:
#ifdef POWER_SAVING_FEATURE_WEARABLE
				__ps_check_handle_modem_off_request(modem, ON_REQUEST, TNOTI_UNKNOWN);
#else
				if (modem->hook_flag & PS_NETWORK_SET_POWER_LOW)
					if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(modem->co_modem, wqd->ur))
						err("Failed to dispatch ");
#endif
			break;

			case TREQ_SIM_SET_POWERSTATE:
				if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(co_sim, wqd->ur))
					err("Failed to dispatch ");
			break;

			default:
				ps_err_ex_co(co_network, "No expected request ");
			}
		}

		/* Freeing Allocated memory*/
		free(wqd);
		queue_data = g_queue_pop_head(modem->work_queue);
	}
	ps_dbg_ex_co(co_network, "All pending request sent ");
}

static enum tcore_hook_return __on_hook_call_status(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer modem = NULL;
	gpointer service = user_data;
	CoreObject *co_network;
	gpointer co_ps = NULL;
	GSList *co_list;

	gboolean b_data_allowed = FALSE;
	gboolean b_roaming_checker = TRUE;
	gboolean b_mms_checker = FALSE;
	gboolean b_ims_checker = FALSE;

	struct tnoti_ps_call_status *cstatus = NULL;

	dbg("call status event");
	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	co_network = _ps_service_ref_co_network(service);
	cstatus = (struct tnoti_ps_call_status *) data;
	co_ps = (CoreObject *)_ps_service_ref_co_ps(service);
	if (co_ps != source) {
		ps_warn_ex_co(co_network, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	modem = _ps_service_ref_modem(service);
	if (!modem) {
		ps_err_ex_co(co_network, "modem does not exist");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	b_data_allowed = _ps_modem_get_data_allowed(modem);

	co_list = tcore_ps_ref_context_by_id(co_ps, cstatus->context_id);
	for (; co_list; co_list = co_list->next) {
		CoreObject *co_context = NULL;
		enum co_context_role role = CONTEXT_ROLE_UNKNOWN;

		co_context = co_list->data;
		role = tcore_context_get_role(co_context);

		if (role == CONTEXT_ROLE_MMS || role == CONTEXT_ROLE_PREPAID_MMS) {
			b_mms_checker = TRUE;
			break;
		}
		if (role == CONTEXT_ROLE_IMS || role == CONTEXT_ROLE_IMS_EMERGENCY) {
			b_ims_checker = TRUE;
			break;
		}
	}

#if !defined(TIZEN_SUPPORT_MMS_CONNECT_FORCE)
		ps_dbg_ex_co(co_network, "csc runtime feature disabled");
		b_mms_checker = FALSE;
#endif

	if ((_ps_modem_get_roaming(modem)) && !(_ps_modem_get_data_roaming_allowed(modem))) {
		ps_dbg_ex_co(co_network, "roaming network is not allowed");
		b_roaming_checker = FALSE;
	}

	ps_dbg_ex_co(co_network, "data_allowed(%d) call status event cid(%d) state(%d) reason(%d)",
			b_data_allowed, cstatus->context_id, cstatus->state, cstatus->result);

	if (!b_roaming_checker || (!b_data_allowed && !b_mms_checker && !b_ims_checker)) {
		ps_dbg_ex_co(co_network, "mismatched: roaming checker(%d) data_allowed(%d) mms_checker(%d) b_ims_checker(%d)",
			 b_roaming_checker, b_data_allowed, b_mms_checker, b_ims_checker);

		if (cstatus->state == PS_CALL_STATE_RESULT_OK) {
			/*
			 * If attach_apn_complete is FALSE, need to set TRUE.
			 * In case of boot-up with roaming-SIM and Data Roaming option disabled,
			 * although define process is triggered, 'attach_apn_complete' flag does not set TRUE.
			 * So, when Data Roaming option is enabled, pdp activation is not triggered.
			 */
			if (((ps_service_t *)service)->attach_apn_complete) {
				_ps_service_set_connected(service, cstatus, FALSE);
				tcore_ps_set_cid_active(co_ps, cstatus->context_id, FALSE);
			} else {
				_ps_service_set_ps_defined(service, TRUE, cstatus->context_id);
			}
			return TCORE_HOOK_RETURN_CONTINUE;
		} else if (cstatus->state == PS_CALL_STATE_RESULT_CONNECT) {
			_ps_service_set_connected(service, cstatus, TRUE);
			_ps_service_disconnect_contexts(service);
			return TCORE_HOOK_RETURN_CONTINUE;
		}
	}

	ps_dbg_ex_co(co_network, "service(%p) status(%d)", service, cstatus->state);
	if (cstatus->state == PS_CALL_STATE_RESULT_OK) {		/* DEFINE */
		_ps_service_set_ps_defined(service, TRUE, cstatus->context_id);
	} else if (cstatus->state == PS_CALL_STATE_RESULT_CONNECT) {	/* CONNECTED */
		TReturn rv;

		if (tcore_ps_get_cid_active(co_ps, cstatus->context_id) == FALSE) {
			ps_dbg_ex_co(co_network, "DDS scenario");

			/* De-activate context */
			rv = tcore_ps_deactivate_contexts(co_ps);
			if (rv != TCORE_RETURN_SUCCESS) {
				ps_dbg_ex_co(co_network, "fail to deactivation");
				return TCORE_HOOK_RETURN_CONTINUE;
			}
		} else {
			gboolean ret = TRUE;
#ifdef PREPAID_SIM_APN_SUPPORT
			ps_context_t *prepaid_context;
#endif

			ret = _ps_service_set_connected(service, cstatus, TRUE);
			tcore_ps_set_cid_connected(co_ps, cstatus->context_id, TRUE);

			if (ret == FALSE || g_queue_get_length((GQueue *)_ps_modem_ref_work_queue(modem)) || (_ps_modem_get_reset_profile(modem) == TRUE)) {
				ps_dbg_ex_co(co_network, "Special request present in queue or pending activation request");

				rv = tcore_ps_deactivate_contexts(co_ps);
				if (rv != TCORE_RETURN_SUCCESS) {
					ps_dbg_ex_co(co_network,  "fail to deactivation");
					return TCORE_HOOK_RETURN_CONTINUE;
				}
			}

#ifdef PREPAID_SIM_APN_SUPPORT
			prepaid_context = _ps_service_return_non_default_context(service, CONTEXT_ROLE_INTERNET);
			if (prepaid_context) {
				ps_context_t *def_context;
				int prepaid_cid = 0;
				CoreObject *prepaid_co_context;
				prepaid_co_context = _ps_context_ref_co_context(prepaid_context);
				prepaid_cid = tcore_context_get_id(prepaid_co_context);

				if (cstatus->context_id == prepaid_cid) {
					ps_dbg_ex_co(co_network, "Prepaid context will be handled as default context!!");
					__ps_context_update_default_internet_to_db(prepaid_context, TRUE);
					prepaid_context->is_default = TRUE;
					prepaid_context->alwayson = TRUE;

					def_context = _ps_service_return_default_context(service, CONTEXT_ROLE_INTERNET);
					if (def_context) {
						__ps_context_update_default_internet_to_db(def_context, FALSE);
						def_context->is_default = FALSE;
						def_context->alwayson = FALSE;
					}
				} else {
					ps_dbg_ex_co(co_network, "Not happen case!!! (context_id : %d)", cstatus->context_id);
				}
			}
#endif
		}
	} else if (cstatus->state == PS_CALL_STATE_RESULT_NO_CARRIER) {	/* DISCONNECTED-NO CARRIER */
		gpointer def_context = NULL;
		unsigned char def_cid = 0;
		int value = 0;
		gboolean retry = TRUE;
#ifdef PREPAID_SIM_APN_SUPPORT
		gpointer prepaid_context = NULL;
#endif

		retry = __ps_check_pdp_permanent_reject_cause(cstatus->result);
		/* if retry not permitted by network */
		if (FALSE == retry) {
			ps_dbg_ex_co(co_network, "DO NOT RETRY NETWORK CONNECTION AUTOMATICALLY");
			ps_dbg_ex_co(co_network, "permanent reject cause (%d)", cstatus->result);

			def_context = _ps_service_return_default_context(service, CONTEXT_ROLE_INTERNET);
			if (def_context) {
				gpointer co_context = NULL;
				co_context = _ps_context_ref_co_context(def_context);
				def_cid = tcore_context_get_id(co_context);
			}
		}

#ifdef PREPAID_SIM_APN_SUPPORT
		prepaid_context = _ps_service_return_non_default_context(service, CONTEXT_ROLE_INTERNET);
		if (prepaid_context) {
			gpointer prepaid_co_context = NULL;
			char *prepaid_apn;
			unsigned char prepaid_cid;
			TReturn rv;

			prepaid_co_context = _ps_context_ref_co_context(prepaid_context);
			prepaid_cid = tcore_context_get_id(prepaid_co_context);
			prepaid_apn = tcore_context_get_apn(prepaid_co_context);
			ps_dbg_ex_co(co_network, "Default context disconnected!!! "
				"Try again with prepaid context!!! apn[%s]", prepaid_apn);

			if (def_context == NULL) {
				def_context = _ps_service_return_default_context(service, CONTEXT_ROLE_INTERNET);
				if (def_context) {
					gpointer co_context = NULL;
					co_context = _ps_context_ref_co_context(def_context);
					def_cid = tcore_context_get_id(co_context);
				}
			}

			_ps_service_set_ps_defined(service, FALSE, cstatus->context_id);
			tcore_ps_set_cid_active(co_ps, cstatus->context_id, FALSE);
			tcore_ps_set_cid_connected(co_ps, cstatus->context_id, FALSE);
			_ps_service_set_connected(service, cstatus, FALSE);

			if (cstatus->context_id == def_cid) {
				dbg("Resetting connection time for default context");
				_ps_service_reset_connection_timer(def_context);

				/* Connect to prepaid internet */
				rv = _ps_service_activate_context(service, prepaid_context);
				dbg("prepaid internet connect - rv[%d]", rv);
			} else if (cstatus->context_id == prepaid_cid) {
				dbg("Resetting connection time for prepaid internet context");
				_ps_service_reset_connection_timer(def_context);

				/* Connect to default internet */
				rv =  _ps_service_connect_default_context(service);
				dbg("default internet connect - rv[%d]", rv);
			}
			return TCORE_HOOK_RETURN_CONTINUE;
		}
#endif

		_ps_service_set_ps_defined(service, FALSE, cstatus->context_id);
		tcore_ps_set_cid_active(co_ps, cstatus->context_id, FALSE);
		tcore_ps_set_cid_connected(co_ps, cstatus->context_id, FALSE);
		_ps_service_set_connected(service, cstatus, FALSE);

		if (FALSE == tcore_ps_any_context_activating_activated(co_ps, &value)) {
			ps_dbg_ex_co(co_network, "No open connections, publish disconnected signal");

			/* Handle any pending request if present */
			modem = _ps_service_ref_modem(service);
			__ps_send_pending_user_request(modem);

			/* Ensured that set_reset_profile is always done default thread's context */
			if (_ps_modem_get_reset_profile(modem) == TRUE) {
				/* Initiate Reset Profile */
				ps_dbg_ex_co(co_network, "Profiles are being reset");
				/* Shouldn't invoke set profile directly, as it will remove hooks registered to server while being hook callback*/
				if (NULL == _ps_modem_get_profile_reset_gsource(modem)) {
					GSource *gsource = NULL;
					gsource = ps_util_gsource_dispatch(g_main_context_default(), G_PRIORITY_LOW, (GSourceFunc)_ps_modem_initiate_reset_profile, modem);
					_ps_modem_set_profile_reset_gsource(modem, gsource);
				}
			}
		}

		ps_dbg_ex_co(co_network, "any context activating or activated [%d]", value);
		if (FALSE == retry)
			if (cstatus->context_id == def_cid)
				_ps_service_reset_connection_timer(def_context);
	} /* disconnected case */

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_call_status_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_call_status(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_call_status_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_call_status(s, source, command, data_len, data, user_data);
}


static enum tcore_hook_return __on_hook_session_data_counter_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	ps_dbg_ex_co(_ps_service_ref_co_network(service), "session data counter event");

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_session_data_counter_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	ps_dbg_ex_co(_ps_service_ref_co_network(service), "session data counter event");

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_dedicated_bearerinfo(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	CoreObject *co_ps = NULL;
	struct tnoti_ps_dedicated_bearer_info *bearer_info = NULL;

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_CONTINUE);

	bearer_info = (struct tnoti_ps_dedicated_bearer_info *) data;
	co_ps = (CoreObject *)_ps_service_ref_co_ps(service);
	if (co_ps != source) {
		ps_warn_ex_co(co_ps, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	_ps_service_set_context_bearerinfo(service, bearer_info);

	ps_dbg_ex_co(co_ps, "dedicated bearer information event");
	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_dedicated_bearerinfo_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_dedicated_bearerinfo(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_dedicated_bearerinfo_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_dedicated_bearerinfo(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_ipconfiguration(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	CoreObject *co_ps = NULL;
	CoreObject *co_network = NULL;
	struct tnoti_ps_pdp_ipconfiguration *devinfo = NULL;
	char ipv4[16], ipv4_dns_1[16], ipv4_dns_2[16];

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_STOP_PROPAGATION);

	co_network = _ps_service_ref_co_network(service);
	devinfo = (struct tnoti_ps_pdp_ipconfiguration *) data;
	co_ps = (CoreObject *)_ps_service_ref_co_ps(service);
	if (co_ps != source) {
		ps_warn_ex_co(co_network, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	ps_dbg_ex_co(co_network, "ip configuration event");

	/*
	 * In case IPv4 address is available and DNS address
	 * is NOT available, set -
	 * DNS 1 - Google DNS
	 * DNS 2 - Open DNS
	 */
	snprintf(ipv4, 16, "%d.%d.%d.%d",
		devinfo->ip_address[0], devinfo->ip_address[1],
		devinfo->ip_address[2], devinfo->ip_address[3]);
	if (!g_str_equal(ipv4, "0.0.0.0")) {
		snprintf(ipv4_dns_1, 16, "%d.%d.%d.%d",
			devinfo->primary_dns[0], devinfo->primary_dns[1],
			devinfo->primary_dns[2], devinfo->primary_dns[3]);
		if (g_str_equal(ipv4_dns_1, "0.0.0.0")) {
			err("[IPV4]primary dns address is 0");

			/* google dns 1st */
			devinfo->primary_dns[0] = 8;
			devinfo->primary_dns[1] = 8;
			devinfo->primary_dns[2] = 8;
			devinfo->primary_dns[3] = 8;
		}

		snprintf(ipv4_dns_2, 16, "%d.%d.%d.%d",
			devinfo->secondary_dns[0], devinfo->secondary_dns[1],
			devinfo->secondary_dns[2], devinfo->secondary_dns[3]);
		if (g_str_equal(ipv4_dns_2, "0.0.0.0")) {
			/* open dns 2nd */
			err("[IPV4]secondary dns address is 0");
			devinfo->secondary_dns[0] = 208;
			devinfo->secondary_dns[1] = 67;
			devinfo->secondary_dns[2] = 222;
			devinfo->secondary_dns[3] = 222;
		}
	}

	/*
	 * In case IPv6 address is available and DNS address
	 * is NOT available, set -
	 * DNS 1 - Google DNS
	 * DNS 2 - Open DNS
	 */
	if (devinfo->ipv6_address != NULL) {
		if (devinfo->ipv6_primary_dns == NULL) {
			err("[IPV6]primary dns address is 0");
			devinfo->ipv6_primary_dns = g_strdup("2001:4860:4860::8888");

		}
		if (devinfo->ipv6_secondary_dns == NULL) {
			err("[IPV6]secondary dns address is 0");
			devinfo->ipv6_secondary_dns = g_strdup("2620:0:ccc::2");
		}
	}

	_ps_service_set_context_devinfo(service, devinfo);

	return TCORE_HOOK_RETURN_CONTINUE;
}


static enum tcore_hook_return __on_hook_ipconfiguration_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_ipconfiguration(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_ipconfiguration_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_ipconfiguration(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_powered(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	gpointer modem = user_data;
	CoreObject *co_modem;
	struct tnoti_modem_power *modem_power = NULL;
	int  power = PS_MODEM_STATE_UNKNOWN;

	CORE_OBJECT_CHECK_RETURN(source, CORE_OBJECT_TYPE_MODEM, TCORE_HOOK_RETURN_CONTINUE);

	g_return_val_if_fail(modem != NULL, TCORE_HOOK_RETURN_CONTINUE);
	co_modem = _ps_modem_ref_co_modem(modem);

	modem_power = (struct tnoti_modem_power *)data;
	g_return_val_if_fail(modem_power != NULL, TCORE_HOOK_RETURN_CONTINUE);
	ps_dbg_ex_co(co_modem, "powered event called: state [%d]", modem_power->state);

	switch (modem_power->state) {
	case MODEM_STATE_ONLINE:
		power = PS_MODEM_STATE_ONLINE;
	break;

	case MODEM_STATE_LOW:
		power = PS_MODEM_STATE_LOW;
	break;

	case MODEM_STATE_ERROR:
	case MODEM_STATE_RESET:
		/* Reset hook flag in any present */
		__ps_modem_cp_reset_handler(modem);
	break;

	default:
		ps_warn_ex_co(co_modem, "Unhandled modem power event.");
	break;
	}

	if (power != PS_MODEM_STATE_UNKNOWN)
		_ps_modem_processing_power_enable(modem, power);

	return TCORE_HOOK_RETURN_CONTINUE;
}


static enum tcore_hook_return __on_hook_powered_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	return __on_hook_powered(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_powered_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	return __on_hook_powered(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_flight(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	gpointer modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);
	struct tnoti_modem_flight_mode *modem_flight = NULL;

	g_return_val_if_fail(modem != NULL, TCORE_HOOK_RETURN_CONTINUE);
	ps_dbg_ex_co(co_modem, "flight mode event called");

	modem_flight = (struct tnoti_modem_flight_mode *)data;
	_ps_modem_processing_flight_mode(modem, modem_flight->enable);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_flight_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	return __on_hook_flight(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_flight_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	return __on_hook_flight(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_net_register(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	gboolean ps_attached = FALSE;
	struct tnoti_network_registration_status *regist_status;
	CoreObject *co_network;
	dbg("network register event called");

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_CONTINUE);


	co_network = (CoreObject *)_ps_service_ref_co_network(service);
	if (co_network != source) {
		ps_dbg_ex_co(co_network, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	regist_status = (struct tnoti_network_registration_status *) data;
	if (regist_status->ps_domain_status == NETWORK_SERVICE_DOMAIN_STATUS_FULL)
		ps_attached = TRUE;

	_ps_modem_set_roaming(_ps_service_ref_modem(service), regist_status->roaming_status);
	_ps_service_processing_network_event(service, ps_attached, regist_status->roaming_status);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_net_register_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_net_register(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_net_register_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_net_register(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_net_change(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	struct tnoti_network_change *network_change;
	CoreObject *co_network;
	dbg("network change event called");

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_CONTINUE);

	co_network = (CoreObject *)_ps_service_ref_co_network(service);
	if (co_network != source) {
		ps_dbg_ex_co(co_network, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	network_change = (struct tnoti_network_change *) data;
	ps_dbg_ex_co(co_network, "plmn(%s) act(%d)", network_change->plmn, network_change->act);
	_ps_service_set_access_technology(service, network_change->act);

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_net_change_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_net_change(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_net_change_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_net_change(s, source, command, data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_net_restricted_state(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	gpointer service = user_data;
	struct tnoti_network_restricted_state *network_restricted;
	CoreObject *co_network;
	dbg("network restricted event called");

	g_return_val_if_fail(service != NULL, TCORE_HOOK_RETURN_CONTINUE);

	co_network = (CoreObject *)_ps_service_ref_co_network(service);
	if (co_network != source) {
		ps_warn_ex_co(co_network, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}

	network_restricted = (struct tnoti_network_restricted_state *) data;
	ps_dbg_ex_co(co_network, "network restricted state(%d)", network_restricted->restricted_state);

	_ps_service_set_restricted(service, ((network_restricted->restricted_state & NETWORK_RESTRICTED_STATE_PS_ALL) ? TRUE : FALSE));

	return TCORE_HOOK_RETURN_CONTINUE;
}

static enum tcore_hook_return __on_hook_net_restricted_state_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_net_restricted_state(s, source, command, data_len, data, user_data);
}


static enum tcore_hook_return __on_hook_net_restricted_state_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data,
		void *user_data)
{
	return __on_hook_net_restricted_state(s, source, command, data_len, data, user_data);
}


static enum tcore_hook_return __on_hook_sim_init(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	struct tnoti_sim_status *sim_data;
	ps_modem_t *modem = user_data;
	CoreObject *co_modem = _ps_modem_ref_co_modem(modem);
	gchar *cp_name, *source_cp_name;
	ps_dbg_ex_co(co_modem, "sim init event called");

	g_return_val_if_fail(user_data != NULL, TCORE_HOOK_RETURN_CONTINUE);

	cp_name = _ps_modem_ref_cp_name(modem);
	source_cp_name = (gchar *)tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(source));
	if (g_strcmp0(cp_name, source_cp_name) != 0) {
		ps_warn_ex_co(co_modem, "Received notification for different Subscription - neglecting the notification!!!");
		return TCORE_HOOK_RETURN_CONTINUE;
	}


	sim_data = (struct tnoti_sim_status *)data;
	ps_dbg_ex_co(co_modem, "sim status is (%d)", sim_data->sim_status);

	switch (sim_data->sim_status) {
	case SIM_STATUS_INIT_COMPLETED: {
		struct tel_sim_imsi *sim_imsi = NULL;
		enum tel_sim_type sim_type = SIM_TYPE_UNKNOWN;
		sim_type = tcore_sim_get_type(source);

		if (sim_type == SIM_TYPE_NVSIM) {
			dbg("initial boot from CDMA network.");
			_ps_modem_processing_sim_complete((gpointer)user_data, TRUE, PS_CDMA_DUMMY_PROFILE_PLMN);
		} else {
			sim_imsi = tcore_sim_get_imsi(source);
			_ps_modem_processing_sim_complete((gpointer)user_data, TRUE, (gchar *)sim_imsi->plmn);

			g_free(sim_imsi);
		}
	}
	break;

	case SIM_STATUS_CARD_ERROR: /* FALLTHROUGH */
	case SIM_STATUS_CARD_REMOVED: /* FALLTHROUGH */
	case SIM_STATUS_CARD_CRASHED: /* FALLTHROUGH */
	case SIM_STATUS_CARD_POWEROFF:
		/* Set SIM complete FALSE, operator is not required */
		_ps_modem_processing_sim_complete((gpointer)user_data, FALSE, NULL);

		/* TODO: Handle CDMA specific case */
	break;

	default:
		ps_dbg_ex_co(co_modem,  "Unhandled SIM state: [%d]", sim_data->sim_status);
	break;
	}

	return TCORE_HOOK_RETURN_CONTINUE;
}

#ifdef POWER_SAVING_FEATURE_WEARABLE
static gboolean __ps_is_any_call_in_progress(TcorePlugin *plugin, __ps_call_flow_type type, enum tcore_notification_command command)
{
	CoreObject *co_call = 0;
	int total_call_cnt = 0;

	gboolean call_in_progress = FALSE;

	co_call = tcore_plugin_ref_core_object(plugin, CORE_OBJECT_TYPE_CALL);

	total_call_cnt = tcore_call_object_total_length(co_call);
	dbg("totall call cnt (%d)", total_call_cnt);

	if (((type == ON_REQUEST || type == ON_NON_CALL_NOTI_HOOK) && total_call_cnt !=  0)
		|| ((type == ON_CALL_NOTI_HOOK)
		&& ((command != TNOTI_CALL_STATUS_IDLE) || (total_call_cnt > 1))))	{
		dbg("call is still connected");
		call_in_progress = TRUE;
	} else {
		dbg("No call is in progress");
	}

	return call_in_progress;
}


/* Check for pending TREQ_MODEM_POWER_OFF request */
void __ps_check_handle_modem_off_request(gpointer data, __ps_call_flow_type type, enum tcore_notification_command command)
{
	ps_modem_t *modem = data;

	if (!modem)
		return;

	if (modem->hook_flag & PS_NETWORK_SET_POWER_LOW) {
		UserRequest *ur = NULL;
		ur = ps_util_pop_waiting_job(modem->work_queue, TREQ_MODEM_POWER_LOW);
		if (ur) {
			gboolean call_in_progress;
			dbg("Sending Pending SET POWER OFF");

			/* Checking if any voice or MMS is in progress, if so, delay Modem power off.
			   Need to hook on both voice call status and MMS profile de-activation.
			*/
			call_in_progress = __ps_is_any_call_in_progress(tcore_object_ref_plugin(modem->co_modem), type, command);

			if (call_in_progress) {
				gboolean ret;
				/* add to the waiting queue and continue and wait till there is no call or MMS */
				ret = ps_util_add_waiting_job(modem->work_queue, TREQ_MODEM_POWER_LOW , ur);
				if (!ret) {
					err("fail to add the request to queue");
					tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
					if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(modem->co_modem, ur)) {
						__ps_send_ur_dispatch_failure_response(ur, TRESP_MODEM_POWER_LOW);
						modem->hook_flag &= PS_NETWORK_RESET_SET_POWER_LOW_FLAG;

					}
				}
			} else {
				tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);
				if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(modem->co_modem, ur)) {
					__ps_send_ur_dispatch_failure_response(ur, TRESP_MODEM_POWER_LOW);
					modem->hook_flag &= PS_NETWORK_RESET_SET_POWER_LOW_FLAG;
				}
			}
		}
	} else {
		dbg("No pending TREQ_MODEM_POWER_LOW reqeust");
	}
}

static enum tcore_hook_return __on_hook_voice_call_status(Server *s, CoreObject *co_call,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	__ps_check_handle_modem_off_request(user_data, ON_CALL_NOTI_HOOK, command);
	return TCORE_HOOK_RETURN_CONTINUE;
}
#endif

static enum tcore_hook_return __on_hook_sim_init_0(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	return  __on_hook_sim_init(s, source, command,  data_len, data, user_data);
}

static enum tcore_hook_return __on_hook_sim_init_1(Server *s, CoreObject *source,
		enum tcore_notification_command command, unsigned int data_len, void *data, void *user_data)
{
	return  __on_hook_sim_init(s, source, command,  data_len, data, user_data);
}

void _ps_get_network_mode(gpointer data)
{
	UserRequest *ur = NULL;
	ps_modem_t *modem =  data;

	CoreObject *co_network = NULL;

	ps_dbg_ex_co(_ps_modem_ref_co_modem(modem), "network get mode by data allowed option");

	co_network = tcore_plugin_ref_core_object(tcore_object_ref_plugin(modem->co_modem),
			CORE_OBJECT_TYPE_NETWORK);

	ur = tcore_user_request_new(NULL, NULL);
	tcore_user_request_set_data(ur, 0, NULL);
	tcore_user_request_set_command(ur, TREQ_NETWORK_GET_MODE);
	tcore_user_request_set_response_hook(ur, __ps_hook_response_cb, modem);

	__ps_modem_set_hook_flag(modem, TREQ_NETWORK_GET_MODE);

	if (TCORE_RETURN_SUCCESS != tcore_object_dispatch_request(co_network, ur)) {
		err("Failed to dispatch ");
		__ps_send_ur_dispatch_failure_response(ur, TRESP_NETWORK_GET_MODE);
		modem->hook_flag &= PS_NETWORK_RESET_GET_MODE_FLAG;
	}

	return;
}

gboolean _ps_hook_co_modem_event(gpointer modem)
{
	Server *s = NULL;
	TcorePlugin *p;
	CoreObject *co_modem;
	const char *modem_name = NULL;
	g_return_val_if_fail(modem != NULL, FALSE);

	p = _ps_modem_ref_plugin(modem);
	s = tcore_plugin_ref_server(p);
	co_modem = _ps_modem_ref_co_modem(modem);

	modem_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_modem));
	if (TRUE == g_str_has_suffix(modem_name , "0")) {
		tcore_server_add_notification_hook(s, TNOTI_MODEM_POWER, __on_hook_powered_0, modem);
		tcore_server_add_notification_hook(s, TNOTI_MODEM_FLIGHT_MODE, __on_hook_flight_0, modem);
		tcore_server_add_notification_hook(s, TNOTI_SIM_STATUS, __on_hook_sim_init_0, modem);
#ifdef POWER_SAVING_FEATURE_WEARABLE /* TODO: Modify for DSDS support */
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_IDLE, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_ACTIVE, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_HELD, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_DIALING, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_ALERT, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_INCOMING, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_WAITING, __on_hook_voice_call_status, modem);
#endif /* #ifdef POWER_SAVING_FEATURE_WEARABLE */
	} else {
		tcore_server_add_notification_hook(s, TNOTI_MODEM_POWER, __on_hook_powered_1, modem);
		tcore_server_add_notification_hook(s, TNOTI_MODEM_FLIGHT_MODE, __on_hook_flight_1, modem);
		tcore_server_add_notification_hook(s, TNOTI_SIM_STATUS, __on_hook_sim_init_1, modem);
#ifdef POWER_SAVING_FEATURE_WEARABLE /* TODO: Modify for DSDS support */
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_IDLE, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_ACTIVE, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_HELD, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_DIALING, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_ALERT, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_INCOMING, __on_hook_voice_call_status, modem);
		tcore_server_add_notification_hook(s, TNOTI_CALL_STATUS_WAITING, __on_hook_voice_call_status, modem);
#endif /* #ifdef POWER_SAVING_FEATURE_WEARABLE */
	}
	return TRUE;
}

gboolean _ps_free_co_modem_event(gpointer modem)
{
	Server *s = NULL;
	TcorePlugin *p;
	CoreObject *co_modem;
	const char *modem_name = NULL;
	g_return_val_if_fail(modem != NULL, FALSE);

	p = _ps_modem_ref_plugin(modem);
	s = tcore_plugin_ref_server(p);
	co_modem = _ps_modem_ref_co_modem(modem);

	modem_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_modem));
	if (TRUE == g_str_has_suffix(modem_name , "0")) {
		tcore_server_remove_notification_hook(s, __on_hook_powered_0);
		tcore_server_remove_notification_hook(s, __on_hook_flight_0);
		tcore_server_remove_notification_hook(s, __on_hook_sim_init_0);
	} else {
		tcore_server_remove_notification_hook(s, __on_hook_powered_1);
		tcore_server_remove_notification_hook(s, __on_hook_flight_1);
		tcore_server_remove_notification_hook(s, __on_hook_sim_init_1);
	}
	return TRUE;
}

enum tcore_hook_return __on_hook_modem_added(Server *s,
		CoreObject *source, enum tcore_notification_command command,
		unsigned int data_len, void *data, void *user_data)
{
	gpointer *master = user_data;
	TcorePlugin *plg = data;

	if (FALSE == _ps_master_create_modems(master, plg))
		err("Failed to create modem");

	return TCORE_HOOK_RETURN_CONTINUE;
}

enum tcore_hook_return __on_hook_modem_removed(Server *s,
		CoreObject *source, enum tcore_notification_command command,
		unsigned int data_len, void *data, void *user_data)
{
	gpointer *master = user_data;
	TcorePlugin *plg = data;

	if (FALSE == _ps_master_destroy_modem(master, plg))
		err("Failed to destroy modem");

	return TCORE_HOOK_RETURN_CONTINUE;
}

gboolean _ps_get_co_modem_values(gpointer modem)
{
	TcorePlugin *plg;
	CoreObject *co_modem = NULL;
	CoreObject *co_sim = NULL;

	gboolean sim_init = FALSE, modem_powered = FALSE, flight_mode = FALSE;
	int sim_status = 0;
	enum tel_sim_type sim_type = SIM_TYPE_UNKNOWN;
	struct tel_sim_imsi *sim_imsi = NULL;

	g_return_val_if_fail(modem != NULL, FALSE);

	co_modem = _ps_modem_ref_co_modem(modem);
	if (!co_modem)
		return FALSE;

	plg = tcore_object_ref_plugin(co_modem);
	if (!plg)
		return FALSE;

	co_sim = tcore_plugin_ref_core_object(plg, CORE_OBJECT_TYPE_SIM);

	sim_status = tcore_sim_get_status(co_sim);
	if (sim_status == SIM_STATUS_INIT_COMPLETED)
		sim_init = TRUE;

	sim_imsi = tcore_sim_get_imsi(co_sim);
	modem_powered = tcore_modem_get_powered(co_modem);
	flight_mode = tcore_modem_get_flight_mode_state(co_modem);

	_ps_modem_processing_flight_mode(modem, flight_mode);
	_ps_modem_processing_power_enable(modem, modem_powered);

	sim_type = tcore_sim_get_type(co_sim);

	if (sim_type == SIM_TYPE_NVSIM)
		_ps_modem_processing_sim_complete(modem, sim_init, PS_CDMA_DUMMY_PROFILE_PLMN);
	else
		_ps_modem_processing_sim_complete(modem, sim_init, (gchar *)sim_imsi->plmn);
	g_free(sim_imsi);
	return TRUE;
}

gboolean _ps_hook_co_network_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	CoreObject *co_network = NULL;
	const char *modem_name = NULL;

	g_return_val_if_fail(service != NULL, FALSE);

	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);
	co_network = _ps_service_ref_co_network(service);

	modem_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_network));
	if (TRUE == g_str_has_suffix(modem_name , "0")) {
		tcore_server_add_notification_hook(s, TNOTI_NETWORK_REGISTRATION_STATUS, __on_hook_net_register_0, service);
		tcore_server_add_notification_hook(s, TNOTI_NETWORK_CHANGE, __on_hook_net_change_0, service);
		tcore_server_add_notification_hook(s, TNOTI_NETWORK_RESTRICTED_STATE, __on_hook_net_restricted_state_0, service);
	} else {
		tcore_server_add_notification_hook(s, TNOTI_NETWORK_REGISTRATION_STATUS, __on_hook_net_register_1, service);
		tcore_server_add_notification_hook(s, TNOTI_NETWORK_CHANGE, __on_hook_net_change_1, service);
		tcore_server_add_notification_hook(s, TNOTI_NETWORK_RESTRICTED_STATE, __on_hook_net_restricted_state_1, service);
	}
	return TRUE;
}

gboolean _ps_get_co_network_values(gpointer service)
{
	CoreObject *co_network = NULL;
	gboolean ps_attached = FALSE;
	gint ps_restricted = 0;

	enum telephony_network_service_domain_status ps_status;
	enum telephony_network_access_technology act;

	g_return_val_if_fail(service != NULL, FALSE);

	co_network = _ps_service_ref_co_network(service);
	ps_dbg_ex_co(co_network, "Entered ");

	tcore_network_get_service_status(co_network, TCORE_NETWORK_SERVICE_DOMAIN_TYPE_PACKET, &ps_status);
	tcore_network_get_access_technology(co_network, &act);

	if (ps_status == NETWORK_SERVICE_DOMAIN_STATUS_FULL)
		ps_attached = TRUE;

	ps_restricted = tcore_network_get_restricted_state(co_network);

	_ps_service_set_restricted(service, ((ps_restricted == NETWORK_RESTRICTED_STATE_PS_ALL) ? TRUE : FALSE));
	_ps_service_set_roaming(service, tcore_network_get_roaming_state(co_network));
	_ps_service_set_ps_attached(service, ps_attached);
	_ps_service_set_access_technology(service, act);

	return TRUE;
}

gboolean _ps_hook_co_ps_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	CoreObject *co_ps = NULL;
	const char *modem_name = NULL;
	g_return_val_if_fail(service != NULL, FALSE);

	ps_dbg_ex_co(_ps_service_ref_co_network(service), "Entered ");
	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);
	co_ps = _ps_service_ref_co_ps(service);

	modem_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_ps));
	if (TRUE == g_str_has_suffix(modem_name , "0")) {
		tcore_server_add_notification_hook(s, TNOTI_PS_CALL_STATUS, __on_hook_call_status_0, service);
		tcore_server_add_notification_hook(s, TNOTI_PS_CURRENT_SESSION_DATA_COUNTER, __on_hook_session_data_counter_0, service);
		tcore_server_add_notification_hook(s, TNOTI_PS_PDP_IPCONFIGURATION, __on_hook_ipconfiguration_0, service);
		tcore_server_add_notification_hook(s, TNOTI_PS_DEDICATED_BEARER_INFO, __on_hook_dedicated_bearerinfo_0, service);
	} else {
		tcore_server_add_notification_hook(s, TNOTI_PS_CALL_STATUS, __on_hook_call_status_1, service);
		tcore_server_add_notification_hook(s, TNOTI_PS_CURRENT_SESSION_DATA_COUNTER, __on_hook_session_data_counter_1, service);
		tcore_server_add_notification_hook(s, TNOTI_PS_PDP_IPCONFIGURATION, __on_hook_ipconfiguration_1, service);
		tcore_server_add_notification_hook(s, TNOTI_PS_DEDICATED_BEARER_INFO, __on_hook_dedicated_bearerinfo_1, service);
	}
	return TRUE;
}

gboolean _ps_free_modem_event(gpointer modem)
{
	Server *s = NULL;
	TcorePlugin *p;
	g_return_val_if_fail(modem != NULL, FALSE);

	p = _ps_modem_ref_plugin(modem);
	s = tcore_plugin_ref_server(p);

	tcore_server_remove_notification_hook(s, __on_hook_powered);
	tcore_server_remove_notification_hook(s, __on_hook_flight);
	tcore_server_remove_notification_hook(s, __on_hook_sim_init);

#ifdef POWER_SAVING_FEATURE_WEARABLE
	tcore_server_remove_notification_hook(s, __on_hook_voice_call_status);
#endif /* #ifdef POWER_SAVING_FEATURE_WEARABLE */

	return TRUE;

}

gboolean _ps_free_co_ps_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	CoreObject *co_ps = NULL;
	CoreObject *co_network;
	const char *modem_name = NULL;

	g_return_val_if_fail(service != NULL, FALSE);
	co_network = _ps_service_ref_co_network(service);

	ps_dbg_ex_co(co_network, "Entered ");
	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);
	co_ps = _ps_service_ref_co_ps(service);

	modem_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_ps));
	if (modem_name)
		ps_dbg_ex_co(co_network, "modem name %s", modem_name);

	if (TRUE == g_str_has_suffix(modem_name , "0")) {
		tcore_server_remove_notification_hook(s, __on_hook_call_status_0);
		tcore_server_remove_notification_hook(s, __on_hook_session_data_counter_0);
		tcore_server_remove_notification_hook(s, __on_hook_ipconfiguration_0);
	 } else {
		tcore_server_remove_notification_hook(s, __on_hook_call_status_1);
		tcore_server_remove_notification_hook(s, __on_hook_session_data_counter_1);
		tcore_server_remove_notification_hook(s, __on_hook_ipconfiguration_1);
	 }
	 return TRUE;
}

gboolean _ps_free_co_network_event(gpointer service)
{
	Server *s = NULL;
	TcorePlugin *p;
	CoreObject *co_network = NULL;
	const char *modem_name = NULL;
	g_return_val_if_fail(service != NULL, FALSE);

	ps_dbg_ex_co(_ps_service_ref_co_network(service), "Entered ");
	p = _ps_service_ref_plugin(service);
	s = tcore_plugin_ref_server(p);
	co_network = _ps_service_ref_co_network(service);

	modem_name = tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_network));
	if (TRUE == g_str_has_suffix(modem_name , "0")) {
		tcore_server_remove_notification_hook(s, __on_hook_net_register_0);
		tcore_server_remove_notification_hook(s, __on_hook_net_change_0);
		tcore_server_remove_notification_hook(s, __on_hook_net_restricted_state_0);
	} else {
		tcore_server_remove_notification_hook(s, __on_hook_net_register_1);
		tcore_server_remove_notification_hook(s, __on_hook_net_change_1);
		tcore_server_remove_notification_hook(s, __on_hook_net_restricted_state_1);
	}
	return TRUE;
}

gboolean _ps_update_cellular_state_key(gpointer object)
{
	Server *s = NULL;
	static Storage *strg;
	int current_state = 0;
	int stored_state = 0;
	ps_service_t *service = object;
	CoreObject *co_network = _ps_service_ref_co_network(service);
	ps_modem_t *modem = _ps_service_ref_modem(service);
	ps_subs_type subs_type = _ps_modem_get_subs_type(modem);
	int selected_sim = -1;

	ps_dbg_ex_co(co_network, "Update cellular state for [SIM%d]", subs_type + 1);

	s = tcore_plugin_ref_server(_ps_service_ref_plugin(service));
	strg = tcore_server_find_storage(s, "vconf");

	selected_sim = tcore_storage_get_int(strg, STORAGE_KEY_TELEPHONY_DUALSIM_DEFAULT_DATA_SERVICE_INT);
	if ((selected_sim != -1) && (selected_sim != (int)subs_type)) {
		ps_warn_ex_co(co_network, "Update for only [SIM%d] selected by Setting", selected_sim + 1);
		return FALSE;
	}

	current_state = _ps_service_check_cellular_state(service);

	if (tcore_modem_get_flight_mode_state(modem->co_modem) == TRUE)
		current_state = TELEPHONY_PS_FLIGHT_MODE;

	stored_state = tcore_storage_get_int(strg, STORAGE_KEY_CELLULAR_STATE);
	ps_dbg_ex_co(co_network, "Cellular state, current: [%d], stored: [%d]", current_state, stored_state);
	if (current_state != stored_state)
		tcore_storage_set_int(strg, STORAGE_KEY_CELLULAR_STATE, current_state);

	return TRUE;
}

