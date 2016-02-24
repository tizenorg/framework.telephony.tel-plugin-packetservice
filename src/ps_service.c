/*
 * tel-plugin-packetservice
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

#include "ps_common.h"
#include "generated-code.h"

#include <core_object.h>
#include <co_ps.h>
#include <co_context.h>
#include <storage.h>

#define PROP_DEFAULT	FALSE
#define PROP_DEFAULT_STR   NULL

#define TIMEOUT_DEFAULT		5
#define TIMEOUT_MAX			1800


static void __ps_service_emit_property_changed_signal(ps_service_t *service);
static void __ps_service_emit_context_added_signal(ps_service_t *service, gpointer context);
static void __ps_service_emit_context_removed_signal(ps_service_t *service, gpointer context);
static void _ps_service_setup_interface(PacketServiceService *service, ps_service_t *service_data);

static char *__ps_service_act2string(enum telephony_network_access_technology act);
static gboolean __ps_service_check_connection_option(gpointer service, gpointer context);
static int __ps_service_connetion_timeout_handler(alarm_id_t alarm_id, void *context);

void __remove_service_handler(gpointer data)
{
	ps_service_t *service = data;

	dbg("Entered");
	if (!service) {
		dbg("Service is Null");
		return;
	}

	/*Need to remove the compelete hash table*/
	g_slist_free(service->contexts);

	/*Need to UNexport and Unref the master Object */
	if (service->if_obj) {
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(service->if_obj));
		g_object_unref(service->if_obj);
		service->if_obj = NULL;
	}

	/*Need to free the memory of the internal structure*/
	g_free(service->path);
	g_free(service);

	dbg("Exiting");
	return;
}

static void __ps_service_emit_property_changed_signal(ps_service_t *service)
{
	GVariant *gv = NULL;
	GVariantBuilder property;

	ps_dbg_ex_co(_ps_service_ref_co_network(service), "get service properties");

	gv = _ps_service_get_properties(service, &property);
	packet_service_service_emit_property_changed(service->if_obj, gv);

	dbg("Exiting");
	return;
}

static void __ps_service_emit_context_added_signal(ps_service_t *service, gpointer context)
{
	GVariant *gv = NULL;
	GVariantBuilder property;

	ps_dbg_ex_co(_ps_service_ref_co_network(service), "get service properties");

	gv = _ps_context_get_properties(context, &property);
	packet_service_service_emit_context_added(service->if_obj, gv);

	dbg("Exiting");
	return;
}

static void __ps_service_emit_context_removed_signal(ps_service_t *service, gpointer context)
{
	ps_context_t *pscontext = context;

	dbg("Entered");
	packet_service_service_emit_context_removed(service->if_obj, pscontext->path);

	dbg("Exiting");
	return;
}

static char *__ps_service_act2string(enum telephony_network_access_technology act)
{
	switch (act) {
	case NETWORK_ACT_GSM:
	case NETWORK_ACT_GPRS:
	case NETWORK_ACT_EGPRS:
	case NETWORK_ACT_UMTS:
	case NETWORK_ACT_GSM_UTRAN:
		return "GSM";

	case NETWORK_ACT_IS95A:
	case NETWORK_ACT_IS95B:
	case NETWORK_ACT_CDMA_1X:
	case NETWORK_ACT_EVDO_REV0:
	case NETWORK_ACT_CDMA_1X_EVDO_REV0:
	case NETWORK_ACT_EVDO_REVA:
	case NETWORK_ACT_CDMA_1X_EVDO_REVA:
	case NETWORK_ACT_EVDV:
		return "CDMA";

	case NETWORK_ACT_LTE:
		return "LTE";

	case NETWORK_ACT_UNKNOWN:
	default:
		return "unknown";
	}

	return NULL;
}

static gboolean __ps_service_check_connection_option(gpointer object, gpointer context)
{
	gboolean b_connect = TRUE;
	gboolean power = FALSE, sim = FALSE, data = FALSE, flight = FALSE, nw_ops = FALSE;
	gboolean profile_reset = FALSE;

	enum co_context_role role = CONTEXT_ROLE_UNKNOWN;
	gint ps_mode;
	guchar hook_flag;

	ps_service_t *service = object;
	ps_modem_t *modem = _ps_service_ref_modem(service);
	ps_context_t *ps_context = context;
	CoreObject *co_context = (CoreObject *)_ps_context_ref_co_context(context);
	CoreObject *co_network = _ps_service_ref_co_network(service);

	if (TRUE != _ps_context_get_profile_enable(ps_context)) {
		ps_warn_ex_co(co_network, "Profile is disabled.");
		return FALSE;
	}

	role = tcore_context_get_role(co_context);
	if (service->roaming)
		b_connect &= _ps_modem_get_data_roaming_allowed(modem);

	sim = _ps_modem_get_sim_init(modem);
	data = _ps_modem_get_data_allowed(modem);
	flight = _ps_modem_get_flght_mode(modem);
	hook_flag = _ps_modem_get_hook_flag(modem);
	profile_reset = _ps_modem_get_reset_profile(modem);
	if (hook_flag != PS_NO_PENDING_REQUEST)
		nw_ops = TRUE;
	if (PS_MODEM_STATE_ONLINE == _ps_modem_get_power(modem))
		power = TRUE;

	b_connect &= power;
	b_connect &= sim;

	if (role == CONTEXT_ROLE_IMS || role == CONTEXT_ROLE_IMS_EMERGENCY) {
		dbg("Do not check data allowed value in case of IMS type");
	} else {
#if defined(TIZEN_SUPPORT_MMS_CONNECT_FORCE)
		ps_dbg_ex_co(co_network, "csc runtime feature enabled");
		if (role != CONTEXT_ROLE_MMS && role != CONTEXT_ROLE_PREPAID_MMS) {
			b_connect &= data;
		} else {
			char *tmp_apn = NULL;
			tmp_apn = tcore_context_get_apn(co_context);
			dbg("csc runtime feature is enabled: apn[%s]", tmp_apn);
			if (ps_feature_get_bool(PS_FEATURE_OPERATOR_SKT)) {
				if (data)
					tcore_context_set_apn(co_context, "web.sktelecom.com");
				else
					tcore_context_set_apn(co_context, "mmsonly.sktelecom.com");
			}
		}
#else
		ps_dbg_ex_co(co_network, "csc runtime feature disabled");
		b_connect &= data;
#endif
	}

	b_connect &= !flight;
	b_connect &= !nw_ops;
	b_connect &= !service->restricted;
	b_connect &= !profile_reset;
#ifndef TIZEN_PS_FORCE_ATTACH_DETACH
	b_connect &= service->ps_attached;
#endif
	ps_mode = _ps_modem_get_psmode(modem);

	/*
	 * Kiran PLM P141017-05702
	 * Problem: PDP retry when Wifi connected.
	 * Reproducible cases:
	 * 1) Wifi auto connected but PDP has never been connected.
	 * 2) Quick switch Wifi On -> Off before PDP establishment is complete.
	 * 2nd exceptional case is handled by Connmand but 1st case should be
	 * taken care of by telephony.
	 * Solution: Do not PDP retry until initial PDP connection when Wifi connected.
	 */
	if (service->initial_pdp_conn == FALSE) {
		int wifi_state = PS_WIFI_STATE_OFF;
		Server *s = NULL;
		Storage *strg = NULL;

		s = tcore_plugin_ref_server(service->plg);
		strg = tcore_server_find_storage(s, "vconf");
		wifi_state = tcore_storage_get_int(strg, KEY_WIFI_STATE);
		if (wifi_state == PS_WIFI_STATE_CONNECTED) {
			if (service->wifi_connected_checked == FALSE) {
				ps_dbg_ex_co(co_network, "DO NOT set PDP retry timer when WiFi connected but PDP never been connected yet.");
				b_connect &= FALSE;
				service->wifi_connected_checked = TRUE;
			} else {
				ps_dbg_ex_co(co_network, "Wifi connected state was already checked.");
			}
		}
	}
	ps_dbg_ex_co(co_network, "b_connect(%d), power(%d), sim init(%d), data allowed(%d), flight mode(%d) restricted(%d) ps_attached(%d), ps_mode(%d), fook_flag(%d)",
		b_connect, power, sim, data, flight, service->restricted, service->ps_attached, ps_mode, hook_flag);

	/* In case of ME in LCD off & UPS mode.
	 * Do not allow PDP activation.
	 */
	if (b_connect) {
		if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
			Storage *strg_vconf = tcore_server_find_storage(tcore_plugin_ref_server(service->plg), "vconf");
			gint pm_state = tcore_storage_get_int(strg_vconf, STORAGE_KEY_PM_STATE);
			if (pm_state == 3) {
				ps_warn_ex_co(co_network, "PDP activation is not allowed in LCD off & UPS mode.");
				b_connect = FALSE;
			}
		}
	}
	return b_connect;
}

static int __ps_service_connetion_timeout_handler(alarm_id_t alarm_id, void *context)
{
	int rv = 0;
	ps_service_t *service = _ps_context_ref_service(context);
	if (service == NULL) {
		err("service is NULL!!!");
		return rv;
	}

	if (service->timer_src > 0) {
		dbg("remove connection retry timer (%d)", service->timer_src);
		alarmmgr_remove_alarm(service->timer_src);
		service->timer_src = 0;
	}
	rv = _ps_service_activate_context(service, context);
	ps_dbg_ex_co(_ps_service_ref_co_network(service), "return rv(%d)", rv);
	return rv;
}

void _ps_service_set_attach_apn(ps_service_t *service)
{
	unsigned int index;
	int default_data_subs;
	ps_subs_type subs_type;
	Server *s = NULL;
	Storage *strg;
	ps_modem_t *modem = NULL;

	if (service->attach_apn_complete == TRUE) {
		info("Attach APN already completed");
		return;
	}

	modem = _ps_service_ref_modem(service);

	/* Check for default data subscription value if matchs for modem then only attach */
	subs_type = _ps_modem_get_subs_type(modem);
	s = tcore_plugin_ref_server(_ps_service_ref_plugin(service));
	strg = tcore_server_find_storage(s, "vconf");
	default_data_subs = tcore_storage_get_int(strg, STORAGE_KEY_TELEPHONY_DUALSIM_DEFAULT_DATA_SERVICE_INT);
	if (default_data_subs != (int)subs_type) {
		warn("Ignore Attach APN for non-dds sim [DDS - SIM%d]", default_data_subs + 1);
		return;
	}

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer ps_context = g_slist_nth_data(service->contexts, index);
		CoreObject *co_context = _ps_context_ref_co_context(ps_context);
		gboolean attach_apn = tcore_context_get_attach_apn(co_context);
		if (attach_apn) {
			TReturn rv;
			char *apn = tcore_context_get_apn(co_context);
			dbg("'Attach APN' [%s]", apn);
			rv = tcore_ps_define_context(service->co_ps, co_context, NULL);
			if (rv != TCORE_RETURN_SUCCESS)
				err("fail to define context");

			g_free(apn);
			/* Attach APN is only one. */
			break;
		}
	}
}

gpointer _ps_service_create_service(GDBusConnection *conn, TcorePlugin *p, gpointer p_modem,
		CoreObject *co_network, CoreObject *co_ps, gchar *path)
{
	PacketServiceService *service;
	GError *error = NULL;
	ps_service_t *new_service;

	ps_dbg_ex_co(co_network, "service object create");
	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(p_modem != NULL, NULL);

	/*creating the master object for the interface com.tcore.ps.modem*/
	service = packet_service_service_skeleton_new();

	/*Initializing the modem list for internal referencing*/
	new_service = g_try_malloc0(sizeof(ps_service_t));
	if (NULL == new_service) {
		ps_err_ex_co(co_network, "Unable to allocate memory for master");
		goto FAILURE;
	}

	new_service->conn = conn;
	new_service->plg = p;
	new_service->p_modem = p_modem;
	new_service->co_network = co_network;
	new_service->co_ps = co_ps;
	new_service->path = g_strdup(path);
	new_service->if_obj = service;

	/*exporting the interface object to the path mention for master*/
	if (TRUE != g_dbus_interface_skeleton_export((G_DBUS_INTERFACE_SKELETON(service)), conn, path, &error)) {
		ps_err_ex_co(co_network, "Failed to export interaface with message [%s] & ID[%d] ", error->message, error->code);
		goto FAILURE;
	}

	_ps_hook_co_network_event(new_service);
	_ps_get_co_network_values(new_service);
	_ps_hook_co_ps_event(new_service);

	/*Setting up the interface for the service */
	_ps_service_setup_interface(service, new_service);

	new_service->connection_timeout = TIMEOUT_DEFAULT;
	ps_dbg_ex_co(co_network, "Successfully Created the service");
	return new_service;

FAILURE:
	/*To Do: Handle failure case*/
	g_free(new_service);
	g_assert_no_error(error);
	return NULL;
}

gboolean _ps_service_ref_context(gpointer object, gpointer context)
{
	gpointer tmp = NULL;
	ps_service_t *service = object;
	CoreObject *co_network = NULL;

	dbg("service refer to context");
	g_return_val_if_fail(service != NULL, FALSE);

	co_network = _ps_service_ref_co_network(service);
	tmp = g_slist_find(service->contexts, context);
	if (tmp != NULL) {
		ps_dbg_ex_co(co_network, "context(%p) already existed", tmp);
		return FALSE;
	}

	_ps_context_set_service(context, service);
	tcore_ps_add_context(service->co_ps, (CoreObject *) _ps_context_ref_co_context(context));
	service->contexts = g_slist_append(service->contexts, context);

	ps_dbg_ex_co(co_network, "context(%p) insert to linked-list", context);
	__ps_service_emit_context_added_signal(service, context);

	return TRUE;
}

gboolean _ps_service_ref_contexts(gpointer object, GSList *contexts, gchar *operator)
{
	ps_service_t *service = object;
	gboolean ret = TRUE;
	int rv;
	unsigned int index, count;
	CoreObject *co_network = NULL;
	g_return_val_if_fail(service != NULL, FALSE);
	count = g_slist_length(contexts);
	ps_dbg_ex_co(co_network, "service refer to contexts: count(%d)", count);
	co_network = _ps_service_ref_co_network(service);
	for (index = 0; index < count; index++) {
		gpointer tmp = NULL, value = NULL;

		value = g_slist_nth_data(contexts, index);
		tmp = g_slist_find(service->contexts, value);
		if (tmp != NULL) {
			ps_dbg_ex_co(co_network, "context(%p) already existed", tmp);
			continue;
		}

		_ps_context_set_service(value, service);
		tcore_ps_add_context(service->co_ps, (CoreObject *) _ps_context_ref_co_context(value));
		service->contexts = g_slist_append(service->contexts, value);

		ps_dbg_ex_co(co_network, "context(%p) path(%s) insert to linked-list", value, _ps_context_ref_path(value));
		__ps_service_emit_context_added_signal(service, value);

		if (service->ps_attached) {
			gboolean f_awo = _ps_context_get_alwayson_enable(value);
			if (f_awo) {
				rv = _ps_service_define_context(service, value);
				ps_dbg_ex_co(co_network, "Always-on started, return rv(%d)", rv);
			}
		}
	}
	ps_dbg_ex_co(co_network, "service->contexts: count(%d)", g_slist_length(service->contexts));
	_ps_update_cellular_state_key(service);
	return ret;
}

gboolean _ps_service_get_properties_handler(gpointer object, GVariantBuilder *properties)
{
	ps_service_t *service = object;

	dbg("get service properties");
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	g_variant_builder_open(properties, G_VARIANT_TYPE("a{ss}"));
	g_variant_builder_add(properties, "{ss}", "path", service->path);
	g_variant_builder_add(properties, "{ss}", "ps_attached", BOOL2STRING(service->ps_attached));
	g_variant_builder_add(properties, "{ss}", "roaming", BOOL2STRING(service->roaming));
	g_variant_builder_add(properties, "{ss}", "act", __ps_service_act2string(service->act));
	g_variant_builder_close(properties);

	dbg("Exiting");
	return TRUE;
}

GVariant *_ps_service_get_properties(gpointer object, GVariantBuilder *properties)
{
	ps_service_t *service = object;

	dbg("get service properties ");
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	g_variant_builder_init(properties, G_VARIANT_TYPE("a{ss}"));

	g_variant_builder_add(properties, "{ss}", "path", service->path);
	g_variant_builder_add(properties, "{ss}", "ps_attached", BOOL2STRING(service->ps_attached));
	g_variant_builder_add(properties, "{ss}", "roaming", BOOL2STRING(service->roaming));
	g_variant_builder_add(properties, "{ss}", "act", __ps_service_act2string(service->act));

	dbg("Exiting");
	return g_variant_builder_end(properties);
}

gchar *_ps_service_ref_path(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->path;
}

gpointer _ps_service_ref_plugin(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->plg;
}

gpointer _ps_service_ref_co_network(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->co_network;
}

gpointer _ps_service_ref_co_ps(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->co_ps;
}

gpointer _ps_service_ref_modem(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->p_modem;
}

gboolean _ps_service_set_context_devinfo(gpointer object, struct tnoti_ps_pdp_ipconfiguration *devinfo)
{
	GSList *contexts = NULL;
	ps_service_t *service = object;
	CoreObject *co_context = NULL;

	dbg("set context info");
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(devinfo != NULL, FALSE);

	contexts = tcore_ps_ref_context_by_id(service->co_ps, devinfo->context_id);
	if (!contexts) {
		ps_dbg_ex_co(_ps_service_ref_co_network(service), "fail to ref context by cid.");
		return FALSE;
	}




	while (contexts) {
		ps_modem_t *modem = NULL;
		int role = CONTEXT_ROLE_UNKNOWN;

		co_context = contexts->data;
		if (!co_context) {
			contexts = contexts->next;
			continue;
		}

		/*TEMP*/
		role = tcore_context_get_role(co_context);
		modem = _ps_service_ref_modem(service);
		if (g_strcmp0(modem->operator, "45005") == 0 && role == CONTEXT_ROLE_IMS) {
			ps_dbg_ex_co(_ps_service_ref_co_network(service), "SKT INS PDN");
			/*IPv4*/
			if (devinfo->pcscf_ipv4_count == 0) {
				char ipv4[16];
				snprintf(ipv4, 16, "%d.%d.%d.%d",
					devinfo->ip_address[0], devinfo->ip_address[1],
					devinfo->ip_address[2], devinfo->ip_address[3]);
				if (!g_str_equal(ipv4, "0.0.0.0")) {
					devinfo->pcscf_ipv4_count = 1;
					devinfo->pcscf_ipv4 = g_try_malloc0(sizeof(char *) * devinfo->pcscf_ipv4_count);
					if (devinfo->pcscf_ipv4 != NULL)
						devinfo->pcscf_ipv4[0] = g_strdup("220.103.220.10");
				}
			}
			/*IPv6*/
			if (devinfo->pcscf_ipv6_count == 0) {
				if (devinfo->ipv6_address != NULL) {
					devinfo->pcscf_ipv6_count = 1;
					devinfo->pcscf_ipv6 = g_try_malloc0(sizeof(char *) * devinfo->pcscf_ipv6_count);
					if (devinfo->pcscf_ipv6 != NULL)
						devinfo->pcscf_ipv6[0] = g_strdup("2001:2d8:00e0:0220::10");
				}
			}
		}

		tcore_context_set_devinfo(co_context, devinfo);

		contexts = contexts->next;
	}

	return TRUE;
}

gboolean _ps_service_set_context_bearerinfo(gpointer object, struct tnoti_ps_dedicated_bearer_info *bearer_info)
{
	GSList *contexts = NULL;
	ps_service_t *service = object;
	CoreObject *co_context = NULL;

	dbg("set context info");
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(bearer_info != NULL, FALSE);

	contexts = tcore_ps_ref_context_by_id(service->co_ps, bearer_info->primary_context_id);
	if (!contexts) {
		ps_dbg_ex_co(_ps_service_ref_co_network(service), "fail to ref context by cid.");
		return FALSE;
	}

	while (contexts) {
		unsigned int index;

		co_context = contexts->data;
		if (!co_context) {
			contexts = contexts->next;
			continue;
		}

		for (index = 0; index < g_slist_length(service->contexts); index++) {
			gpointer value = g_slist_nth_data(service->contexts, index);
			if (co_context == _ps_context_ref_co_context(value)) {
				_ps_context_set_bearer_info(value, bearer_info);
				break;
			}
		}

		contexts = contexts->next;
	}

	return TRUE;
}

int _ps_service_define_context(gpointer object, gpointer context)
{
	ps_service_t *service = object;
	CoreObject *co_context = NULL;
	gboolean b_connect = TRUE;

	dbg("define context(%p)", context);
	g_return_val_if_fail(service != NULL, FALSE);

	b_connect = __ps_service_check_connection_option(service, context);
	if (!b_connect)
		return TCORE_RETURN_EPERM;

	return tcore_ps_define_context(service->co_ps, co_context, NULL);
}

int _ps_service_activate_context(gpointer object, gpointer context)
{
	ps_service_t *service = object;
	ps_modem_t *modem = NULL;
	CoreObject *co_context = NULL;
	gboolean b_connect = TRUE;
	gboolean ps_defined;
	int ret = TCORE_RETURN_SUCCESS;
	int default_data_subs = 1;
	ps_subs_type subs_type = 1;
	Server *s = NULL;
	Storage *strg;
	CoreObject *co_network = NULL;

	dbg("activate context(%p)", context);
	g_return_val_if_fail(service != NULL, FALSE);

	s = tcore_plugin_ref_server(_ps_service_ref_plugin(service));
	strg = tcore_server_find_storage(s, "vconf");

	co_network = _ps_service_ref_co_network(service);
	modem = _ps_service_ref_modem(service);
	ps_defined = _ps_context_get_ps_defined(context);
	co_context = (CoreObject *)_ps_context_ref_co_context(context);
	if (modem->hook_flag != PS_NO_PENDING_REQUEST) {
		ps_dbg_ex_co(co_network, "Pending request present in queue with flag %x", modem->hook_flag);
		ret = TCORE_RETURN_FAILURE;
		goto EXIT;
	}

	/* Check for default data subscription value if matchs for modem then only activate */
	subs_type = _ps_modem_get_subs_type(modem);
	default_data_subs = tcore_storage_get_int(strg, STORAGE_KEY_TELEPHONY_DUALSIM_DEFAULT_DATA_SERVICE_INT);
	if ((default_data_subs != -1) && (default_data_subs != (int)subs_type)) {
		ps_warn_ex_co(co_network, "activation  for only [SIM%d] selected by Setting", default_data_subs + 1);
		ret = TCORE_RETURN_FAILURE;
		goto EXIT;
	}

	b_connect = __ps_service_check_connection_option(service, context);
	if (!b_connect) {
		ret = TCORE_RETURN_EPERM;
		goto EXIT;
	}

	if (!ps_defined) {
		ps_dbg_ex_co(co_network, "pdp profile is not defined yet, define first. ");
		ret = tcore_ps_define_context(service->co_ps, co_context, NULL);
	} else {
		ps_dbg_ex_co(co_network, "pdp profile is defined, activate context. ");
		ret = tcore_ps_activate_context(service->co_ps, co_context, NULL);
	}
EXIT:
	if (ret != TCORE_RETURN_SUCCESS) {
		if (ps_defined) {
			/*
			 * CONTEXT_STATE_ACTIVATING : Never be happen.
			 * CONTEXT_STATE_ACTIVATED : Never be happen.
			 * CONTEXT_STATE_DEACTIVATING: Do not clear resources.
			 */
			if (CONTEXT_STATE_DEACTIVATED == tcore_context_get_state(co_context)) {
				ps_warn_ex_co(co_network, "fail to activate context after PDP define complete, clear resources.");
				_ps_context_set_ps_defined(context, FALSE);
				tcore_ps_clear_context_id(service->co_ps, co_context);
			} else {
				ps_err_ex_co(co_network, "invalid context state.");
			}
		}
	}
	return ret;
}

gboolean _ps_service_deactivate_context(gpointer object, gpointer context)
{
	ps_service_t *service = object;
	CoreObject *co_context = NULL;

	g_return_val_if_fail(service != NULL, FALSE);
	dbg("deactivate context(%p)", context);
	co_context = (CoreObject *)_ps_context_ref_co_context(context);

	return tcore_ps_deactivate_context(service->co_ps, co_context, NULL);
}

void _ps_service_set_retry_timeout_value(gpointer object, int value)
{
	ps_service_t *service = object;
	g_return_if_fail(service != NULL);

	service->connection_timeout = value;
	dbg("current timeout (%d)", service->connection_timeout);
	return;
}

void _ps_service_connection_timer(gpointer object, gpointer context)
{
	int result = 0;
	gboolean f_awo = FALSE;
	ps_service_t *service = object;
	int ret;

	g_return_if_fail(service != NULL);

	f_awo = _ps_context_get_alwayson_enable(context);
	if (!f_awo)
		return;

	if (service->timer_src > 0) {
		warn("remove connection retry timer (%d)", service->timer_src);
		alarmmgr_remove_alarm(service->timer_src);
		service->timer_src = 0;
	}

	/* Acquire lock */
	ret = device_power_request_lock(POWER_LOCK_CPU, 0);
	if (ret < 0)
		err("ret : (0x%x)", ret);

	result = alarmmgr_add_alarm_withcb(ALARM_TYPE_VOLATILE, (time_t)(service->connection_timeout),
			0, __ps_service_connetion_timeout_handler, context, &(service->timer_src));

	/* Release lock */
	device_power_release_lock(POWER_LOCK_CPU);
	if (ret < 0)
		err("ret : (0x%x)", ret);

	if (result != ALARMMGR_RESULT_SUCCESS) {
		err("Failed to add alarm(%d)", result);
		return;
	}

	dbg("cellular service timer started timer src(%d), timeout(%d)", service->timer_src, service->connection_timeout);
	service->connection_timeout = (service->connection_timeout)*2;
	if (service->connection_timeout >= TIMEOUT_MAX)
		service->connection_timeout = TIMEOUT_MAX;

	return;
}

void _ps_service_reset_connection_timer(gpointer context)
{
	gboolean f_awo = FALSE;
	ps_service_t *service = NULL;

	f_awo = _ps_context_get_alwayson_enable(context);
	if (!f_awo)
		return;

	service = _ps_context_ref_service(context);
	if (service == NULL) {
		err("service is NULL!!!");
		return;
	}
	service->connection_timeout = TIMEOUT_DEFAULT;

	if (service->timer_src > 0) {
		warn("remove connection retry timer (%d)", service->timer_src);
		alarmmgr_remove_alarm(service->timer_src);
		service->timer_src = 0;
	}
	return;
}

gboolean _ps_service_unref_context(gpointer object, gpointer context)
{
	ps_service_t *service = object;
	ps_modem_t *modem = _ps_service_ref_modem(service);
	ps_context_t *pscontext = context;

	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(modem != NULL, FALSE);
	g_return_val_if_fail(context != NULL, FALSE);
	g_return_val_if_fail(pscontext->path != NULL, FALSE);
	dbg("service unref context (%s)", pscontext->path);

	_ps_service_deactivate_context(service, context);
	/* remove context from the list (modem, service) */
	modem->contexts = g_slist_remove(modem->contexts, pscontext);
	__ps_service_emit_context_removed_signal(service, pscontext);
	return TRUE;
}

void _ps_service_remove_contexts(gpointer object)
{
	unsigned int index;
	ps_service_t *service = object;
	guint count;

	g_return_if_fail(service != NULL);
	count = g_slist_length(service->contexts);
	ps_dbg_ex_co(_ps_service_ref_co_network(service), "service remove all contexts: count(%d)", count);

	for (index = 0; index < count; index++) {
		gpointer value = NULL;
		value = g_slist_nth_data(service->contexts, index);
		ps_dbg_ex_co(_ps_service_ref_co_network(service), "path(%s), value(%p) context", _ps_context_ref_path(value), value);
		_ps_service_unref_context(service, value);
		_ps_context_remove_context(value);
	}
	g_slist_free(service->contexts);
	service->contexts = NULL;
	return;
}

void _ps_service_disconnect_contexts(gpointer object)
{
	unsigned int index;
	ps_service_t *service = object;

	dbg("service disconnect all contexts");
	g_return_if_fail(service != NULL);

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer value = g_slist_nth_data(service->contexts, index);
		_ps_service_reset_connection_timer(value);
		_ps_service_deactivate_context(service, value);
	}

	return;
}

void _ps_service_disconnect_internet_mms_contexts(gpointer object)
{
	unsigned int index;
	ps_service_t *service = object;
	CoreObject *co_context = NULL;
	enum co_context_role role = CONTEXT_ROLE_UNKNOWN;

	dbg("Service disconnect Internet/MMS contexts");
	g_return_if_fail(service != NULL);

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer value = g_slist_nth_data(service->contexts, index);
		co_context = (CoreObject *)_ps_context_ref_co_context(value);
		role = tcore_context_get_role(co_context);

		/*
		 * Deactivate following type of contexts -
		 *	- INTERNET
		 *	- INTERNET_PREPAID
		 *	- MMS
		 *	- MMS_PREPAID
		 */
		switch (role) {
		case CONTEXT_ROLE_INTERNET:
		case CONTEXT_ROLE_MMS:
		case CONTEXT_ROLE_PREPAID_INTERNET:
		case CONTEXT_ROLE_PREPAID_MMS:
			_ps_service_reset_connection_timer(value);
			_ps_service_deactivate_context(service, value);
		break;

		default: {
			dbg("Need not deactivate for %d PDN type", role);
			continue;
		}
		}
	}
}

int _ps_service_connect_default_context(gpointer object)
{
	int rv = 0;
	unsigned int index;
	ps_service_t *service = object;

	dbg("service connect default context");
	g_return_val_if_fail(service != NULL, TCORE_RETURN_FAILURE);

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer value = g_slist_nth_data(service->contexts, index);
		gboolean f_awo = FALSE;
		f_awo = _ps_context_get_alwayson_enable(value);

		if (f_awo) {
			/*  FIX: Kiran PLM P141111-07502 */
			/* _ps_service_reset_connection_timer(value); */
			rv = _ps_service_activate_context(service, value);
			ps_dbg_ex_co(_ps_service_ref_co_network(service), "return rv(%d)", rv);
			break;
		}
	}

	return rv;
}

gpointer _ps_service_return_default_context(gpointer object, int svc_cat_id)
{
	unsigned int index;
	ps_service_t *service = object;

	g_return_val_if_fail(service != NULL, NULL);

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer value = g_slist_nth_data(service->contexts, index);
		gboolean b_default = FALSE;
		b_default = _ps_context_get_default_context(value, svc_cat_id);

		if (b_default)
			return value;
	}

	return NULL;
}

gpointer _ps_service_return_non_default_context(gpointer object, int svc_cat_id)
{
	unsigned int index;
	ps_service_t *service = object;

	g_return_val_if_fail(service != NULL, NULL);

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer ps_context = g_slist_nth_data(service->contexts, index);
		gboolean b_non_default;
		b_non_default = _ps_context_get_non_default_context(ps_context, svc_cat_id);

		if (b_non_default)
			return ps_context;
	}

	return NULL;
}

int _ps_service_update_roaming_apn(gpointer object)
{
	int rv = 0;
	ps_service_t *service = object;
	ps_modem_t *modem = _ps_service_ref_modem(object);
	gboolean p_from = FALSE; /* default <FLASE> : Home newtwork */
	GSList *contexts = NULL;

	g_return_val_if_fail(service != NULL, TCORE_RETURN_FAILURE);
	g_return_val_if_fail(modem != NULL, TCORE_RETURN_FAILURE);

	dbg("roaming status: %d", service->roaming);
	/* 1) Remove all contexts
	 * 2) Load Home/Roaming profiles from database.

	 * Home -> Roaming network:
	 * 3-2) If Any roaming profile is not provided by service provider,
	 *      load Home profiles from database.
	 */

	p_from = _ps_modem_get_roaming_apn_support(modem);
	if (p_from) {
		_ps_service_remove_contexts(object);
		contexts = _ps_context_create_hashtable((gpointer)modem, service->roaming);
		if (contexts != NULL) {
			rv = _ps_service_set_number_of_pdn_cnt(object, modem->operator);
			rv = _ps_service_ref_contexts(object, contexts, modem->operator);
		}
	}
	dbg("rv: %d", rv);
	return rv;
}

gboolean _ps_service_processing_network_event(gpointer object, gboolean ps_attached, gboolean roaming)
{
	ps_service_t *service = object;
	CoreObject *co_network = NULL;
	gboolean prev_roaming_status;
	g_return_val_if_fail(service != NULL, FALSE);


	prev_roaming_status = _ps_service_get_roaming(service);

	co_network = _ps_service_ref_co_network(service);
	_ps_service_set_ps_attached(service, ps_attached);
	_ps_service_set_roaming(service, roaming);
	_ps_update_cellular_state_key(service);

	if (prev_roaming_status != _ps_service_get_roaming(service)) {
		gboolean roaming_allowed = FALSE;
		roaming_allowed = _ps_modem_get_data_roaming_allowed(service->p_modem);
		_ps_service_update_roaming_apn(object);

		if (!roaming_allowed && roaming) {
			ps_dbg_ex_co(co_network, "Roaming allowed (%d), Roaming status (%d)", roaming_allowed, roaming);
			_ps_service_disconnect_contexts(service);
			return TRUE;
		}
	}

	if (service->ps_attached)
		_ps_service_connect_default_context(service);

	return TRUE;
}

gboolean _ps_service_set_connected(gpointer object, gpointer cstatus, gboolean enabled)
{
	unsigned int index;
	gboolean def_awo = FALSE, b_def_conn = FALSE;
	gpointer def_conn = NULL;
	gpointer requested_conn = NULL;

	ps_service_t *service = NULL;
	struct tnoti_ps_call_status *call_status = NULL;
	CoreObject *co_network;

	service = (ps_service_t *) object;
	co_network = _ps_service_ref_co_network(service);
	call_status = (struct tnoti_ps_call_status *)cstatus;

	if (enabled && service->initial_pdp_conn == FALSE) {
		ps_dbg_ex_co(co_network, "Initial PDP connection.");
		service->initial_pdp_conn = TRUE;
	}

	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer value = g_slist_nth_data(service->contexts, index);
		int tmp_cid;
		gboolean b_tmp_def = FALSE;
		CoreObject *context = NULL;
		gpointer b_user_data = NULL;

		context = _ps_context_ref_co_context(value);
		tmp_cid = tcore_context_get_id(context);

		if (tmp_cid != call_status->context_id) continue;

		/* if there is default context in disconnected cid, it has to retry auto connection */
		b_tmp_def = _ps_context_get_default_context(value, CONTEXT_ROLE_INTERNET);
		if (!b_def_conn)
			b_def_conn = b_tmp_def;

		/* if disconnected connection has the user data, it is a priority connection. */
		b_user_data = _ps_context_get_user_data(value);
		if (b_user_data) {
			def_conn = value;
			requested_conn = b_user_data;
		}

		if (!enabled) {
			gchar *ipv4 = NULL;

			ps_dbg_ex_co(co_network, "Reset socket connections.");

			tcore_ps_clear_context_id(service->co_ps, context);
			ipv4 = tcore_context_get_ipv4_addr(context);
			tcore_util_reset_ipv4_socket(tcore_context_get_ipv4_devname(context), (const char *)ipv4);
		}

		_ps_context_set_connected(value, enabled);
	}

	/* connect to request profile */
	if (!enabled && requested_conn) {
		ps_dbg_ex_co(co_network, "connect to request profile (%p)", requested_conn);
		_ps_connection_hdlr(requested_conn);
		_ps_service_reset_connection_timer(def_conn);
		_ps_context_reset_user_data(def_conn);
		return TRUE;
	}

	/* default context and always on is true. - request to connect */
	if (!enabled) {
		gint ps_mode = 0;
		def_conn = _ps_service_return_default_context(service, CONTEXT_ROLE_INTERNET);
		def_awo = _ps_context_get_alwayson_enable(def_conn);

		if (!def_awo) {
			ps_dbg_ex_co(co_network, "there is no always on connection");
			return TRUE;
		}

		/* In case of ME in LCD off & UPS mode.
		 * Do not enable PDP retry timer for default internet context.
		 */
		ps_mode = _ps_modem_get_psmode(_ps_service_ref_modem(service));
		if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
			Storage *strg_vconf = tcore_server_find_storage(_ps_service_ref_plugin(service), "vconf");
			gint pm_state = tcore_storage_get_int(strg_vconf, STORAGE_KEY_PM_STATE);
			if (pm_state == 3) {
				ps_warn_ex_co(co_network, "PDP retry timer is not allowed in LCD off & UPS mode.");
				return TRUE;
			}
		}

		/* always on TRUE and default connection - NORMAL RETRY */
		/* always on TRUE and no default connection - WAIT 5 Secs for retry from application */
		if (b_def_conn) {
			/* retry connection */
			_ps_service_connection_timer(service, def_conn);
		} else {

			/* disconnect from user intention */
#if defined(CONNECT_DEFAULT_CONNECTION_WITHOUT_TIMER)
			if (call_status->result == 2000) {
				ps_dbg_ex_co(co_network, "user intended disconnect / connect default connection without timer");
				__ps_service_connetion_timeout_handler(service->timer_src, def_conn);
				return TRUE;
			}
#endif
			/* with unexpected disconnection from network/me */
			_ps_service_set_retry_timeout_value(service, TIMEOUT_DEFAULT);
			_ps_service_connection_timer(service, def_conn);
		}

	}

	/* To send deactivation request of default profile */
	if (enabled && requested_conn) {
		ps_dbg_ex_co(co_network, "Send deactivation to default profile and connect to request profile (%p)", requested_conn);
		return FALSE;
	}

	return TRUE;
}

void _ps_service_set_ps_defined(gpointer *object, gboolean value, int cid)
{
	ps_service_t *service = (ps_service_t *)object;
	unsigned int index;
	CoreObject *co_network;

	g_return_if_fail(service != NULL);

	co_network = _ps_service_ref_co_network(service);
	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer ps_context = g_slist_nth_data(service->contexts, index);
		CoreObject *co_context = _ps_context_ref_co_context(ps_context);
		unsigned char context_id = tcore_context_get_id(co_context);
		if (context_id == cid) {
			gboolean attach_apn = tcore_context_get_attach_apn(co_context);
			gboolean proceed_activation = TRUE;

			/* Check attach apn complete */
			if (value && attach_apn && !service->attach_apn_complete) {
				int role = tcore_context_get_role(co_context);
				ps_dbg_ex_co(co_network, "Initial define of attach APN is complete for profile role(%d)", role);
				service->attach_apn_complete = TRUE;

				if(TRUE == _ps_context_get_default_context(ps_context, CONTEXT_ROLE_INTERNET) && service->ps_attached)
					proceed_activation = TRUE;
				else {
					proceed_activation = FALSE;
					tcore_ps_clear_context_id(service->co_ps, co_context);
				}
			}
			proceed_activation &= value;

			/* Set 'ps_defined' */
			_ps_context_set_ps_defined(ps_context, proceed_activation);

			if (proceed_activation) {
				/* Activate if define is completed */
				ps_dbg_ex_co(co_network, "define is complete, activate context for cid(%d)", cid);
				if (_ps_service_activate_context(service, ps_context) == TCORE_RETURN_SUCCESS) {
					dbg("Successful activate context");
					tcore_ps_set_cid_active(service->co_ps, cid, TRUE);
				}
			}
			break;
		}
	}

	return;
}

gboolean _ps_service_set_ps_attached(gpointer object, gboolean value)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	service->ps_attached = value;
	ps_dbg_ex_co(_ps_service_ref_co_network(service), "service(%p) ps_attached(%d)", service, service->ps_attached);
	__ps_service_emit_property_changed_signal(service);

	return TRUE;
}

gboolean _ps_service_get_restricted(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	return service->restricted;
}

gboolean _ps_service_set_restricted(gpointer object, gboolean value)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	service->restricted = value;
	ps_dbg_ex_co(_ps_service_ref_co_network(service), "service(%p) restricted(%d)", service, service->restricted);

	_ps_update_cellular_state_key(service);
	return TRUE;
}

gboolean _ps_service_set_number_of_pdn_cnt(gpointer object, gchar *operator)
{
	int rv = 0;
	int num_of_pdn = 0;
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, FALSE);
	ps_dbg_ex_co(_ps_service_ref_co_network(service), "Entered");
	num_of_pdn = _ps_context_get_number_of_pdn(operator, _ps_modem_ref_cp_name(_ps_service_ref_modem(object)));
	rv = tcore_ps_set_num_of_pdn(service->co_ps, num_of_pdn);

	if (rv != TCORE_RETURN_SUCCESS)
		ps_dbg_ex_co(_ps_service_ref_co_network(service), "error to get maximum number of pdn");

	dbg("Exiting");
	return TRUE;
}

gboolean _ps_service_get_roaming(gpointer object)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	return service->roaming;
}

gboolean _ps_service_set_roaming(gpointer object, gboolean value)
{
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	service->roaming = value;
	ps_dbg_ex_co(_ps_service_ref_co_network(service), "service(%p) roaming(%d)", service, service->roaming);
	__ps_service_emit_property_changed_signal(service);

	return TRUE;
}

static void _indicator_cb_dns_reply(GObject *src, GAsyncResult *res, gpointer user_data)
{
	GList *list, *cur;
	GInetAddress *addr;
	gchar *str_addr;
	GError *error = NULL;

	list = g_resolver_lookup_by_name_finish((GResolver *)src, res, &error);
	if (!list) {
		dbg("fail to get dns resolving");
		if (error) {
			dbg("error:%d, %s", error->code, error->message);
			g_error_free(error);
		}
		return;
	}

	for (cur = list; cur; cur = cur->next) {
		addr = cur->data;
		str_addr = g_inet_address_to_string(addr);
		if (!str_addr)
			continue;
		dbg("addr(%s)", str_addr);

		g_free(str_addr);
		g_object_unref(cur->data);
		break;
	}

	g_object_unref(src);
	g_list_free(list);
	return;
}

gboolean _ps_service_set_access_technology(gpointer object,
		enum telephony_network_access_technology value)
{
	ps_service_t *service = object;
	CoreObject *co_network = NULL;
	enum telephony_network_access_technology p_act = 0;
	g_return_val_if_fail(service != NULL, FALSE);

	co_network = _ps_service_ref_co_network(service);
	p_act = service->act;
	service->act = value;
	ps_dbg_ex_co(co_network, "service(%p) P ACT(%d) Access Technology(%d)", service, p_act, service->act);

	if (p_act == NETWORK_ACT_LTE && (service->act >= NETWORK_ACT_GSM && service->act < NETWORK_ACT_LTE)) {
		GResolver *r = NULL;

		ps_dbg_ex_co(co_network, "send the dns pkt for keeping connection");

		r = g_resolver_get_default();
		g_resolver_lookup_by_name_async(r, "www.google.com", NULL, _indicator_cb_dns_reply, NULL);
	}

	if (service->act > NETWORK_ACT_UNKNOWN && service->act < NETWORK_ACT_NOT_SPECIFIED) {
		_ps_update_cellular_state_key(service);
		_ps_service_connect_default_context(service);
	}

	return TRUE;
}

enum telephony_ps_state _ps_service_check_cellular_state(gpointer object)
{
	gboolean state = FALSE;
	ps_service_t *service = object;
	g_return_val_if_fail(service != NULL, TELEPHONY_PS_NO_SERVICE);

	state = _ps_modem_get_flght_mode(service->p_modem);
	if (state)
		return TELEPHONY_PS_FLIGHT_MODE;

	state = _ps_modem_get_power(service->p_modem);
	if (!state)
		return TELEPHONY_PS_NO_SERVICE;

	state = _ps_modem_get_sim_init(service->p_modem);
	if (!state)
		return TELEPHONY_PS_NO_SERVICE;

	if (service->restricted)
		return TELEPHONY_PS_RESTRICTED_SERVICE;

	if (!service->ps_attached)
		return TELEPHONY_PS_NO_SERVICE;

	state = _ps_modem_get_data_allowed(service->p_modem);
	if (!state)
		return TELEPHONY_PS_3G_OFF;

	state = _ps_modem_get_data_roaming_allowed(service->p_modem);
	if (service->roaming && !state)
		return TELEPHONY_PS_ROAMING_OFF;

	return TELEPHONY_PS_ON;
}
static gboolean on_service_get_properties(PacketServiceService *obj_service,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariant *gv = NULL;
	GVariantBuilder property;
	ps_dbg_ex_co(_ps_service_ref_co_network(user_data), "get service properties");

	gv = _ps_service_get_properties(user_data, &property);
	packet_service_service_complete_get_properties(obj_service, invocation, gv);
	return TRUE;
}

static gboolean
on_service_get_context(PacketServiceService *obj_service,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariantBuilder b_context;
	GVariant *contexts;

	unsigned int index;
	ps_service_t *service = user_data;
	CoreObject *co_network = _ps_service_ref_co_network(service);

	ps_dbg_ex_co(co_network, "modem get contexts interface");

	if (service->contexts == NULL) {
		ps_err_ex_co(co_network, "No context present for service");
		FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
		return TRUE;
	}

	g_variant_builder_init(&b_context, G_VARIANT_TYPE("a{sa{ss}}"));
	for (index = 0; index < g_slist_length(service->contexts); index++) {
		gpointer value = g_slist_nth_data(service->contexts, index);
		gchar *path = NULL;
		g_variant_builder_open(&b_context, G_VARIANT_TYPE("{sa{ss}}"));
		path = _ps_service_ref_path(value);

		g_variant_builder_add(&b_context, "s", g_strdup(path));
		if (FALSE == _ps_context_get_properties_handler(value, &b_context)) {
			ps_err_ex_co(co_network, "Failed to get property");
			g_variant_builder_close(&b_context);
			FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
			return TRUE;
		}
		g_variant_builder_close(&b_context);

	}

	contexts = g_variant_builder_end(&b_context);
	packet_service_service_complete_get_contexts(obj_service, invocation, contexts);
	return TRUE;
}

static void _ps_service_setup_interface(PacketServiceService *service, ps_service_t *service_data)
{
	dbg("Entered");
	g_signal_connect(service,
			"handle-get-properties",
			G_CALLBACK(on_service_get_properties),
			service_data);

	g_signal_connect(service,
			"handle-get-contexts",
			G_CALLBACK(on_service_get_context),
			service_data);

	dbg("Exiting");
}

