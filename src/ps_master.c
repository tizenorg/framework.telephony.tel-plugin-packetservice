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

#include <unistd.h>

#include "ps_common.h"
#include "generated-code.h"

#include <server.h>
#include <plugin.h>
#include <storage.h>
#include <hal.h>
#include <user_request.h>
#include <co_context.h>

#define PS_MASTER_PATH	"/"
#define PROP_DEFAULT		FALSE
#define PROP_DEFAULT_STR	NULL

/* [Wearable][sap-stack] SAPInterface.h */
#define SAP_CONN_TYPE_ALL	0x00
#define SAP_CONN_TYPE_BT	0x01
#define SAP_CONN_TYPE_MOBILE	0x10 /* scs */

static void __ps_master_emit_modem_added_signal(ps_master_t *master, gpointer modem);
/*static void __ps_master_emit_modem_removed_signal(ps_master_t *master, gpointer modem);*/
static void _ps_master_setup_interface(PacketServiceMaster *master, ps_master_t *master_data);

static void __ps_master_register_key_callback(gpointer master, enum tcore_storage_key key);
static void __ps_master_storage_key_callback(enum tcore_storage_key key, void *value, void *user_data);

static void __ps_master_handle_ups_mode(gpointer object, gchar *request)
{
	ps_modem_t *modem = object;
	GSList *contexts = NULL;
	unsigned int index;

	if (modem == NULL)
		return;

	dbg("send dbus %s requeset", request);
	contexts = modem->contexts;
	if (contexts == NULL) {
		err("no profiles");
		return;
	}

	for (index = 0; index < g_slist_length(contexts); index++) {
		gchar *s_path = NULL;
		gpointer value = g_slist_nth_data(contexts, index);
		ps_context_t *pscontext = (ps_context_t *)value;
		int role = tcore_context_get_role(pscontext->co_context);

		s_path = _ps_context_ref_path(value);
		dbg("value(%p), path(%s)", value, s_path);

		if (role == CONTEXT_ROLE_INTERNET && pscontext->is_default) {
			if (!g_strcmp0(request, "IfaceDown"))
				_ps_context_handle_ifacedown(value);
			else if (!g_strcmp0(request, "IfaceUp"))
				_ps_context_handle_ifaceup(value);
		} else if (role == CONTEXT_ROLE_UNKNOWN || role == CONTEXT_ROLE_USER_DEFINED) {
			warn("Not supported profile type: %d", role);
			continue;
		} else {
			if (CONTEXT_STATE_ACTIVATED != tcore_context_get_state(pscontext->co_context))
				continue;

			if (!g_strcmp0(request, "InterfaceDown")) {
				warn("disconnect context request.");
				_ps_service_deactivate_context(pscontext->p_service, pscontext);
			}
		}
	}
	return;
}

void __remove_master(gpointer data, gpointer user_data)
{
	ps_master_t *master = data;

	dbg("Entered");

	/*Deinit alarm*/
	alarmmgr_fini();

	/*Need to remove the compelete hash table*/
	g_hash_table_remove_all(master->modems);

	/*Need to UNexport and Unref the master Object */
	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(master->if_obj));

	g_object_unref(master->if_obj);

	/*Need to free the memory allocated for the members of the master*/
	g_free(master->path);
	g_free(master);

	dbg("Exiting");
	return;
}

static void __ps_master_emit_modem_added_signal(ps_master_t *master, gpointer modem)
{
	GVariant *gv = NULL;
	GVariantBuilder properties;

	dbg("get modem properties");

	gv = _ps_modem_get_properties(modem, &properties);
	packet_service_master_emit_modem_added(master->if_obj, gv);

	dbg("Exiting");
}

#if 0
static void __ps_master_emit_modem_removed_signal(ps_master_t *master, gpointer modem)
{
	g_signal_emit(master, signals[SIG_MASTER_MODEM_REMOVED], 0, _ps_modem_ref_path(modem));
	dbg("master (%p) emit the modem(%p) removed signal", master, modem);
}
#endif

static void __ps_master_register_key_callback(gpointer object, enum tcore_storage_key key)
{
	ps_master_t *master = (ps_master_t *) object;
	Server *s = tcore_plugin_ref_server(master->plg);
	static Storage *strg;

	strg = tcore_server_find_storage(s, "vconf");
	tcore_storage_set_key_callback(strg, key, __ps_master_storage_key_callback, object);

	return;
}

static void __ps_master_storage_key_callback(enum tcore_storage_key key, void *value, void *user_data)
{
	GVariant *tmp = NULL;
	GHashTableIter iter;
	gpointer h_key, h_value;
	gboolean type_check = FALSE;
	ps_master_t *master = (ps_master_t *)user_data;

	dbg("storage key(%d) callback", key);
	g_return_if_fail(master != NULL);

	tmp = (GVariant *)value;
	if (!tmp) {
		err("value is null");
		return;
	}

	switch (key) {
	case KEY_3G_ENABLE:
	case KEY_DATA_ROAMING_SETTING:
	case KEY_NETWORK_RESTRICT_MODE:
	case KEY_TELEPHONY_READY:
		type_check = g_variant_is_of_type(tmp, G_VARIANT_TYPE_BOOLEAN);
		if (!type_check) {
			err("wrong variant data type");
			g_variant_unref(tmp);
			return;
		}
	break;

	case KEY_POWER_SAVING_MODE:
	case KEY_PM_STATE:
#ifdef POWER_SAVING_FEATURE_WEARABLE
	case KEY_WECONN_ALL_CONNECTED:
#endif
		type_check = g_variant_is_of_type(tmp, G_VARIANT_TYPE_INT32);
		if (!type_check) {
			dbg("wrong variant data type");
			g_variant_unref(tmp);
			return;
		}
	break;

	default:
		warn("unknown key (0x%x)", key);
		return;
	}

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &h_key, &h_value) == TRUE) {
		if (key == KEY_3G_ENABLE) {
			gboolean data_allowed = g_variant_get_boolean(tmp);
			msg("[PSINFO] data_allowed [%d] changed", data_allowed);
#if defined(TIZEN_PS_FORCE_ATTACH_DETACH)
			if (_ps_master_get_storage_value_int(master, KEY_WECONN_ALL_CONNECTED) > 0) {
				if (data_allowed) {
					int sap_conn_type = SAP_CONN_TYPE_ALL;
					sap_conn_type = _ps_master_get_storage_value_int(master, KEY_SAP_CONNECTION_TYPE);
					if (sap_conn_type == SAP_CONN_TYPE_BT) {
						dbg("[Companion mode] ignore data_allowed.");
						return;
					}
				}
			}
#endif
			_ps_modem_set_data_allowed(h_value, data_allowed);
		} else if (key == KEY_DATA_ROAMING_SETTING) {
			gboolean roaming_allowed = g_variant_get_boolean(tmp);
			msg("[PSINFO] roaming_allowed [%d] changed.", roaming_allowed);
			_ps_modem_set_data_roaming_allowed(h_value, roaming_allowed);
		} else if (key == KEY_POWER_SAVING_MODE) {
			gint ps_mode = g_variant_get_int32(tmp);

			if (ps_mode == POWER_SAVING_MODE_NORMAL && ps_mode != _ps_modem_get_psmode(h_value))
				warn("[PSINFO] UPS mode change: On -> Off ");

			_ps_modem_set_psmode(h_value, ps_mode);
		} else if (key == KEY_PM_STATE) {
			gint pm_state = g_variant_get_int32(tmp);
			gint ps_mode = _ps_modem_get_psmode(h_value);
			dbg("current power saving mode: %d, pm_state: %d", ps_mode, pm_state);
			if (ps_mode & POWER_SAVING_MODE_FMM) {
				warn("UPS by FMM: Do not change data network state.");
				return;
			}
			if (pm_state == 3) {/* LCD Off */
				if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
					msg("[PSINFO] LCD OFF. Start Deactivate with power saving [%d]", ps_mode);
					/* send dbus request pdp context deactivation. */
					__ps_master_handle_ups_mode(h_value, "IfaceDown");
				}
			} else { /* LCD On or dimming */
				if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
					msg("[PSINFO] pm_state(%d) changed. Start activate with power saving [%d]", pm_state, ps_mode);
					/* send dbus request pdp context activation. */
					__ps_master_handle_ups_mode(h_value, "IfaceUp");
				}
			}
		} else if (key == KEY_NETWORK_RESTRICT_MODE) {
			gboolean b_network_restrict = g_variant_get_boolean(tmp);
			if (b_network_restrict) {
				msg("[PSINFO] network restricted mode on");
				_ps_modem_set_data_allowed(h_value, FALSE);
			} else {
				gboolean key_3g_enable = FALSE;
				msg("[PSINFO] network restricted mode off");
				key_3g_enable = _ps_master_get_storage_value_bool(master, KEY_3G_ENABLE);
				_ps_modem_set_data_allowed(h_value, key_3g_enable);
			}
#ifdef STORAGE_KEY_WECONN_ALL_CONNECTED
		} else if (key == KEY_WECONN_ALL_CONNECTED) {
			int b_wms_connected = g_variant_get_int32(tmp);
			if (b_wms_connected) {
				int sap_conn_type = SAP_CONN_TYPE_ALL;
				sap_conn_type = _ps_master_get_storage_value_int(master, KEY_SAP_CONNECTION_TYPE);
				if (sap_conn_type == SAP_CONN_TYPE_BT) {
					msg("[PSINFO][Wearable] Companinon mode. set data allowed FALSE");
					_ps_modem_set_data_allowed(h_value, FALSE);
				}
			} else {
				gboolean key_3g_enable = FALSE;
				key_3g_enable = _ps_master_get_storage_value_bool(master, KEY_3G_ENABLE);
				msg("[PSINFO][Wearable] Standalone mode. set data allowed (%d)", key_3g_enable);
				_ps_modem_set_data_allowed(h_value, key_3g_enable);
			}
#endif
		} else if (key == KEY_TELEPHONY_READY) {
			gboolean b_telephony_ready = _ps_master_get_storage_value_bool(master, KEY_TELEPHONY_READY);
			if (b_telephony_ready)
				__ps_master_emit_modem_added_signal(master, h_value);
		}
	}

	return;
}

gpointer _ps_master_create_master(GDBusConnection *conn, TcorePlugin *p)
{
	PacketServiceMaster *master = NULL;
	ps_master_t *new_master = NULL;
	GError *error = NULL;

	dbg("master object create");
	g_return_val_if_fail(conn != NULL, NULL);

	/*creating the master object for the interface com.tcore.ps.master*/
	master = packet_service_master_skeleton_new();
	g_return_val_if_fail(master != NULL, NULL);


	/*Initializing the master list for internal referencing*/
	new_master = g_try_malloc0(sizeof(ps_master_t));
	if (NULL == new_master) {
		err("Unable to allocate memory for master");
		goto FAILURE;
	}

	new_master->conn = conn;
	new_master->path = g_strdup(PS_MASTER_PATH);
	new_master->plg = p;
	new_master->if_obj = master;
	new_master->modems = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, __remove_modem_handler);

	/*Setting Up the call backs for the interface*/
	_ps_master_setup_interface(master, new_master);

	/*exporting the interface object to the path mention for master*/
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(master),
			conn,
			PS_MASTER_PATH,
			&error);

	g_assert_no_error(error);

	/*Registering the key callbacks for values in storage settings */
	__ps_master_register_key_callback(new_master, KEY_TELEPHONY_READY);
	__ps_master_register_key_callback(new_master, KEY_3G_ENABLE);
	__ps_master_register_key_callback(new_master, KEY_DATA_ROAMING_SETTING);
#if defined(TIZEN_UPS_ENABLED)
	__ps_master_register_key_callback(new_master, KEY_POWER_SAVING_MODE);
	__ps_master_register_key_callback(new_master, KEY_PM_STATE);
#endif
	/* __ps_master_register_key_callback(new_master, KEY_PM_STATE); */
#if defined(TIZEN_PS_FORCE_ATTACH_DETACH)
	__ps_master_register_key_callback(new_master, KEY_WECONN_ALL_CONNECTED);
#endif

	/*Alarm manager init*/
	dbg("init alarm manager.");
	if (alarmmgr_init("packetservice") != ALARMMGR_RESULT_SUCCESS) {
		err("Failed to init alarm manager");
		goto FAILURE;
	}
	/*Adding Hook for modem addition laters*/
	tcore_server_add_notification_hook(tcore_plugin_ref_server(p),
		TNOTI_SERVER_ADDED_MODEM_PLUGIN, __on_hook_modem_added, new_master);

	/*Adding Hook for modem removal laters*/
	tcore_server_add_notification_hook(tcore_plugin_ref_server(p),
		TNOTI_SERVER_REMOVED_MODEM_PLUGIN, __on_hook_modem_removed, new_master);

	dbg("Successfully created the master");
	return new_master;

FAILURE:
		err("Unable to create master");
		g_object_unref(master);
		return NULL;
}

gboolean _ps_master_create_modems(gpointer object, TcorePlugin *modem_plg)
{

	gpointer modem = NULL, tmp = NULL;
	ps_master_t *master = NULL;
	CoreObject *co_modem = NULL;
	gchar *modem_name = NULL;
	gchar *cp_name = NULL;
	gboolean key_telephony_ready;

	dbg("create modem objects");
	g_return_val_if_fail(object != NULL, FALSE);

	master = (ps_master_t *)object;
	key_telephony_ready = _ps_master_get_storage_value_bool(master, KEY_TELEPHONY_READY);
	if (modem_plg) {
		cp_name = (gchar *)tcore_server_get_cp_name_by_plugin(modem_plg);
		modem_name = g_strdup_printf("/%s", cp_name);
		tmp = g_hash_table_lookup(master->modems, modem_name);
		if (tmp != NULL) {
			dbg("modem (%p) already existed", tmp);
			g_free(modem_name);
			return FALSE;
		}

		co_modem = tcore_plugin_ref_core_object(modem_plg, CORE_OBJECT_TYPE_MODEM);

		modem = _ps_modem_create_modem(master->conn, master->plg,
			master, modem_name, co_modem, cp_name);
		if (modem == NULL) {
			dbg("fail to create modem");
			g_free(modem_name);
			return FALSE;
		}

		g_hash_table_insert(master->modems, g_strdup(modem_name), modem);
		dbg("modem (%p) created at path %s", modem , modem_name);

		if (key_telephony_ready) {
			__ps_master_emit_modem_added_signal(master, modem);
		} else {
			dbg("Wait for telephony ready");
		}

		g_free(modem_name);
	} else {

		/*Need to walk through all modem if any present before packet service intialization*/
		Server *s;
		TcorePlugin *p = NULL;
		GSList *plist_head = NULL;
		GSList *plist = NULL;
		GSList *modemlist_head = NULL;
		GSList *modemlist = NULL;

		s = tcore_plugin_ref_server(master->plg);
		plist_head = tcore_server_get_modem_plugin_list(s);

		if (!plist_head) {
			dbg("Modem plugin is not present");
			return TRUE;
		}

		plist = plist_head;

		while (plist) {
			p = plist->data;
			modemlist_head = tcore_plugin_get_core_objects_bytype(p, CORE_OBJECT_TYPE_MODEM);
			if (!modemlist_head) {
				dbg("Found no modem core-objects");
				plist = plist->next;
				continue;
			}
			modemlist = modemlist_head;
			while (modemlist) {
				co_modem = modemlist->data;
				cp_name = (gchar *)tcore_server_get_cp_name_by_plugin(tcore_object_ref_plugin(co_modem));
				modem_name = g_strdup_printf("/%s", cp_name);
				tmp = g_hash_table_lookup(master->modems, modem_name);
				if (tmp != NULL) {
					dbg("modem (%p) already existed", tmp);
					modemlist = modemlist->next;
					g_free(modem_name);
					continue;
				}

				modem = _ps_modem_create_modem(master->conn, master->plg, master, modem_name, co_modem, cp_name);
				if (!modem) {
					dbg("Fail to create modem ");
					modemlist = modemlist->next;
					g_free(modem_name);
					continue;
				}

				g_hash_table_insert(master->modems, g_strdup(modem_name), modem);
				dbg("modem (%p) created at path %s", modem , modem_name);
				if (key_telephony_ready) {
					__ps_master_emit_modem_added_signal(master, modem);
				} else {
					dbg("Wait for telephony ready");
				}
				g_free(modem_name);
				modemlist = modemlist->next;
			}
			g_slist_free(modemlist_head);
			plist = plist->next;
		}

		g_slist_free(plist_head);
	}
	return TRUE;
}

gboolean _ps_master_destroy_modem(gpointer object, TcorePlugin *plugin)
{
	ps_master_t *master = NULL;

	gchar *modem_name;
	gpointer modem;

	dbg("destroy modem object");

	master = (ps_master_t *)object;

	modem_name = g_strdup_printf("/%s",
			tcore_server_get_cp_name_by_plugin(plugin));

	modem = g_hash_table_lookup(master->modems, modem_name);
	if (modem == NULL) {
		dbg("modem '%s' doesn't exists", modem_name);
		g_free(modem_name);
		return FALSE;
	}

	/*  Destroy Modem */
	dbg("Destroying modem object for '%s' modem[%p]", modem_name, modem);
	_ps_modem_destroy_modem(master->conn, modem);

	if (g_hash_table_remove(master->modems, modem_name) == TRUE)
		dbg("Removed modem '%s'", modem_name);

	g_free(modem_name);

	return TRUE;
}

gboolean _ps_master_get_storage_value_bool(gpointer object, enum tcore_storage_key key)
{
	Server *s = NULL;
	Storage *strg = NULL;
	ps_master_t *master = object;

	g_return_val_if_fail(master != NULL, FALSE);
	s = tcore_plugin_ref_server(master->plg);
	strg = tcore_server_find_storage(s, "vconf");

	return tcore_storage_get_bool(strg, key);;
}

gint _ps_master_get_storage_value_int(gpointer object, enum tcore_storage_key key)
{
	Server *s = NULL;
	Storage *strg = NULL;
	ps_master_t *master = object;

	g_return_val_if_fail(master != NULL, FALSE);
	s = tcore_plugin_ref_server(master->plg);
	strg = tcore_server_find_storage(s, "vconf");

	return tcore_storage_get_int(strg, key);;
}

gboolean _ps_master_set_storage_value_bool(gpointer object, enum tcore_storage_key key, gboolean value)
{
	Server *s = NULL;
	Storage *strg = NULL;
	ps_master_t *master = object;

	g_return_val_if_fail(master != NULL, FALSE);
	s = tcore_plugin_ref_server(master->plg);
	strg = tcore_server_find_storage(s, "vconf");

	return tcore_storage_set_bool(strg, key, value);
}

gboolean _ps_master_set_storage_value_int(gpointer object, enum tcore_storage_key key, gint value)
{
	Server *s = NULL;
	Storage *strg = NULL;
	ps_master_t *master = object;

	g_return_val_if_fail(master != NULL, FALSE);
	s = tcore_plugin_ref_server(master->plg);
	strg = tcore_server_find_storage(s, "vconf");

	return tcore_storage_set_int(strg, key, value);
}

gboolean _ps_master_set_always_on_control(gpointer user_data, gboolean enable)
{
	ps_master_t *master = user_data;
	GHashTableIter iter;
	gpointer key, value;

	g_return_val_if_fail(master != NULL, FALSE);
	g_return_val_if_fail(master->modems != NULL, FALSE);

	dbg("Entered");

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		unsigned int i;
		ps_modem_t *modem = value;

		if (modem == NULL)
			continue;
		if (modem->contexts == NULL)
			continue;

		for (i = 0; i < g_slist_length(modem->contexts); i++) {
			ps_context_t *context = (ps_context_t*)g_slist_nth_data(modem->contexts, i);
			gchar *path = _ps_context_ref_path(context);
			if (path) {
				dbg("context(%s)", path);
				_ps_context_set_alwayson_enable(context, enable);
			}
		}
	}
	return TRUE;
}

static gboolean on_master_get_modems(PacketServiceMaster *obj_master,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariantBuilder b_modem;
	GVariant *modems;

	GHashTableIter iter;
	gpointer key, value;
	ps_master_t *master = user_data;

	if (!ps_util_check_access_control(invocation, AC_PS_PUBLIC, "r"))
		return TRUE;

	dbg("Entered");

	if (master->modems == NULL) {
		err("No modem Present");
		FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
		return TRUE;
	}

	g_variant_builder_init(&b_modem, G_VARIANT_TYPE("a{sa{ss}}"));

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {

		gchar *path = NULL;
		path = _ps_modem_ref_path(value);
		dbg("modem path [%s]", path);

		g_variant_builder_open(&b_modem, G_VARIANT_TYPE("{sa{ss}}"));
		g_variant_builder_add(&b_modem, "s", g_strdup(path));
		if (FALSE == _ps_modem_get_properties_handler(value, &b_modem)) {
			err("Unable to get the modem properties");
			g_variant_builder_close(&b_modem);
			FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
			return TRUE;
		}
		g_variant_builder_close(&b_modem);
	}
	modems = g_variant_builder_end(&b_modem);

	packet_service_master_complete_get_modems(obj_master, invocation, modems);
	return TRUE;
}

static gboolean on_master_set_alwayson(PacketServiceMaster *obj_master,
		GDBusMethodInvocation *invocation,
		gboolean enable,
		gpointer user_data)
{
	gboolean result = FALSE;

	if (!ps_util_check_access_control(invocation, AC_PS_PRIVATE, "w"))
		return TRUE;

	dbg("Entered");	
	result = _ps_master_set_always_on_control(user_data, enable);
	if (result)
		packet_service_master_complete_set_alwayson(obj_master, invocation, result);
	else
		FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
	return TRUE;
}


static void _ps_master_setup_interface(PacketServiceMaster *master, ps_master_t *master_data)
{
	dbg("Entered");

	g_signal_connect(master,
			"handle-get-modems",
			G_CALLBACK(on_master_get_modems),
			master_data);

	g_signal_connect(master,
			"handle-set-alwayson",
			G_CALLBACK(on_master_set_alwayson),
			master_data);
	return;

}
