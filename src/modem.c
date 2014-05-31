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

#include "ps-modem.h"

#include "ps.h"
#include "ps-error.h"

#include <server.h>
#include <plugin.h>
#include <core_object.h>

#define PROP_DEFAULT	FALSE
#define PROP_DEFAULT_STR   NULL
#define BOOL2STRING(a)	((a==TRUE) ? ("TRUE"):("FALSE"))

/*Properties*/

enum {
	PROP_MODEM_O,

	PROP_MODEM_PATH,
	PROP_MODEM_MASTER,
	PROP_MODEM_PLUGIN,
	PROP_MODEM_COMODEM,
	PROP_MODEM_CONN,
};

enum {
	SIG_MODEM_SERVICE_ADDED,
	SIG_MODEM_SERVICE_REMOVED,
	SIG_MODEM_PROPERTY_CHANGED,
	SIG_MODEM_LAST
};

static guint32 signals[SIG_MODEM_LAST] = { 0, };

struct PsModemClass {
	GObjectClass parent;

	//method and signals
	void (*service_added)(PsModem *modem, gchar *service_path);
	void (*service_removed)(PsModem *modem, gchar *service_path);
	void (*property_changed)(PsModem *modem, GHashTable *modem_property);
};

struct PsModem {
	GObject parent;

	gchar* path;
	gpointer p_master;
	TcorePlugin *plg;
	CoreObject *co_modem;
	DBusGConnection *conn;

	/*Value from modem*/
	gchar* operator;
	gboolean powered;
	gboolean sim_init;
	gboolean flight_mode;
	gboolean roaming_allowed;
	gboolean data_allowed;

	GHashTable *services;
};

G_DEFINE_TYPE(PsModem, ps_modem, G_TYPE_OBJECT);

/*Function Declaration*/
static void __ps_modem_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void __ps_modem_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);

gboolean ps_iface_modem_get_properties(PsModem *modem, DBusGMethodInvocation *context);
gboolean ps_iface_modem_get_services(PsModem *modem, DBusGMethodInvocation *context);

static void __ps_modem_emit_property_changed_signal(PsModem *modem);
static void __ps_modem_emit_service_added_signal(PsModem *modem, gpointer service);
/*static void __ps_modem_emit_service_removed_signal(PsModem *modem, gpointer service);*/

static void __remove_service(gpointer data);
static void __ps_modem_create_service(DBusGConnection *conn, TcorePlugin *p,
		gpointer modem, CoreObject *co_modem);
static void __ps_modem_remove_service(PsModem *modem, gpointer service);
static gboolean __ps_modem_set_powered(PsModem *modem, gboolean value);
static gboolean __ps_modem_set_sim_complete(PsModem *modem, gboolean value, gchar *operator);
static void __ps_modem_get_ps_setting_from_storage(GObject *object);
static void __ps_modem_processing_modem_event(gpointer object);

#include "ps-iface-modem-glue.h"

static void ps_modem_init(PsModem *modem)
{
	dbg("modem initialize");

	modem->path = PROP_DEFAULT_STR;
	modem->p_master = NULL;
	modem->plg = NULL;
	modem->co_modem = NULL;
	modem->conn = NULL;

	modem->operator = PROP_DEFAULT_STR;
	modem->powered = PROP_DEFAULT;
	modem->sim_init = PROP_DEFAULT;
	modem->flight_mode = PROP_DEFAULT;
	modem->roaming_allowed = PROP_DEFAULT;
	modem->data_allowed = PROP_DEFAULT;

	modem->services = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, __remove_service);
	return;
}

static void ps_modem_class_init(PsModemClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	//class init
	dbg("class init");

	object_class->get_property = __ps_modem_get_property;
	object_class->set_property = __ps_modem_set_property;

	//dbus register
	dbus_g_object_type_install_info(PS_TYPE_MODEM, &dbus_glib_ps_iface_modem_object_info);

	//add properties
	g_object_class_install_property(
			object_class,
			PROP_MODEM_PATH,
			g_param_spec_string("path", "PATH", "Modem Path", NULL,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_MODEM_MASTER,
			g_param_spec_pointer("p_master", "MASTER", "Master Object",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_MODEM_PLUGIN,
			g_param_spec_pointer("plg", "PLUGIN", "Plug in Object",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_MODEM_COMODEM,
			g_param_spec_pointer("co_modem", "COREOBJECTMODEM", "CoreObject Modem",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_MODEM_CONN,
			g_param_spec_boxed("conn", "CONNECTION", "DBus connection", DBUS_TYPE_G_CONNECTION,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	//add signal handler
	signals[SIG_MODEM_SERVICE_ADDED] = g_signal_new("service-added", G_OBJECT_CLASS_TYPE(klass),
			G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(PsModemClass, service_added), NULL, NULL,
			g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, DBUS_TYPE_G_STRING_STRING_HASHTABLE);

	signals[SIG_MODEM_SERVICE_REMOVED] = g_signal_new("service-removed", G_OBJECT_CLASS_TYPE(klass),
			G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(PsModemClass, service_removed), NULL, NULL,
			g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, DBUS_TYPE_G_OBJECT_PATH);

	signals[SIG_MODEM_PROPERTY_CHANGED] = g_signal_new("property-changed",
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST,
			G_STRUCT_OFFSET(PsModemClass, property_changed), NULL, NULL,
			g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, DBUS_TYPE_G_STRING_STRING_HASHTABLE);

	return;
}

static void __ps_modem_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	return;
}

static void __ps_modem_set_property(GObject *object, guint prop_id, const GValue *value,
		GParamSpec *pspec)
{
	PsModem *modem = PS_MODEM(object);

	switch (prop_id) {
		case PROP_MODEM_PATH: {
			if (modem->path) g_free(modem->path);
			modem->path = g_value_dup_string(value);
			msg("	modem(%p) set path(%s)", modem, modem->path);
		}
			break;
		case PROP_MODEM_MASTER: {
			modem->p_master = g_value_get_pointer(value);
			msg("	modem(%p) set master(%p)", modem, modem->p_master);
		}
			break;
		case PROP_MODEM_PLUGIN: {
			modem->plg = g_value_get_pointer(value);
			msg("	modem(%p) set plg(%p)", modem, modem->plg);
		}
			break;
		case PROP_MODEM_COMODEM: {
			modem->co_modem = g_value_get_pointer(value);
			msg("	modem(%p) set coreobject modem(%p)", modem, modem->co_modem);
		}
			break;
		case PROP_MODEM_CONN: {
			modem->conn = g_value_get_boxed(value);
			msg("	modem(%p) set conn(%p)", modem, modem->conn);
		}
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	} //swtich end

	return;
}

gboolean ps_iface_modem_get_properties(PsModem *modem, DBusGMethodInvocation *context)
{
	GError *error = NULL;
	gboolean rv = FALSE;
	GHashTable *properties = NULL;

	dbg("get modem properties");

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	rv = _ps_modem_get_properties(modem, properties);
	if (rv != TRUE) {
		g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "fail to get properties modem(%p)", modem);
		dbus_g_method_return_error(context, error);
		g_hash_table_destroy(properties);
		return FALSE;
	}

	dbus_g_method_return(context, properties);
	g_hash_table_destroy(properties);

	return FALSE;
}

gboolean ps_iface_modem_get_services(PsModem *modem, DBusGMethodInvocation *context)
{
	GError *error = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *services;

	dbg("modem get service interface");

	if (modem->services == NULL) {
		g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "modem(%p) does not have services",
				modem);
		dbus_g_method_return_error(context, error);
		return FALSE;
	}

	services = g_hash_table_new_full(g_direct_hash, g_str_equal, g_free,
			(GDestroyNotify) g_hash_table_destroy);

	g_hash_table_iter_init(&iter, modem->services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gboolean rv = FALSE;
		gchar *path = NULL;
		GHashTable *properties = NULL;

		properties = g_hash_table_new(g_str_hash, g_str_equal);
		rv = _ps_service_get_properties(value, properties);
		if (rv != TRUE) {
			g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "fail to get properties service(%p)",
					value);
			dbus_g_method_return_error(context, error);
			g_hash_table_destroy(properties);
			g_hash_table_destroy(services);
			return FALSE;
		}

		path = _ps_service_ref_path(value);
		g_hash_table_insert(services, g_strdup(path), properties);
		dbg("service (%p) inserted into hash", value);
	}

	dbus_g_method_return(context, services);
	g_hash_table_destroy(services);

	return TRUE;
}

static void __ps_modem_emit_property_changed_signal(PsModem *modem)
{
	GHashTable *properties = NULL;

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_modem_get_properties(modem, properties);

	dbg("Emit signal - 'PROPERTY CHANGED' - modem: [0x%x]", modem);
	g_signal_emit(modem, signals[SIG_MODEM_PROPERTY_CHANGED], 0, properties);

	g_hash_table_destroy(properties);

	return;
}

static void __ps_modem_emit_service_added_signal(PsModem *modem, gpointer service)
{
	GHashTable *properties = NULL;

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_service_get_properties(service, properties);

	dbg("Emit signal - MODEM SERVICE ADDED' - modem: [0x%x] service: [0x%x]", modem, service);
	g_signal_emit(modem, signals[SIG_MODEM_SERVICE_ADDED], 0, properties);

	g_hash_table_destroy(properties);
	return;
}

/*static void __ps_modem_emit_service_removed_signal(PsModem *modem, gpointer service)
{
	g_signal_emit(modem, signals[SIG_MODEM_SERVICE_REMOVED], 0, _ps_service_ref_path(service));
	dbg("modem (%p) emit the service(%p) removed signal", modem, service);
	return;
}*/

static void __remove_service(gpointer data)
{
	return;
}

static void __ps_modem_create_service(DBusGConnection *conn, TcorePlugin *p,
		gpointer modem, CoreObject *co_modem)
{
	gchar *t_path = NULL;
	GObject *object = NULL;

	CoreObject *co_ps = NULL;
	CoreObject *co_network = NULL;
	TcorePlugin *target_plg = NULL;

	dbg("Create Modem Service - Path: [%s]", _ps_modem_ref_path(modem));

	target_plg = tcore_object_ref_plugin(co_modem);
	co_ps = tcore_plugin_ref_core_object(target_plg, CORE_OBJECT_TYPE_PS);
	if (!co_ps) {
		err("No PS Core object");
		return;
	}

	co_network = tcore_plugin_ref_core_object(target_plg, CORE_OBJECT_TYPE_NETWORK);
	if (!co_network) {
		err("No NETWORK Core object");
		return;
	}

	t_path = g_strdup_printf("%s_ps", _ps_modem_ref_path(modem));
	dbg("Service Path: [%s]", t_path);

	object = _ps_service_create_service(conn,p, modem, co_network, co_ps, t_path);
	dbg("Created Service: [0x%x]", object);

	g_hash_table_insert( ((PsModem *) modem)->services, g_strdup(t_path), object);

	/* Emit Service signal */
	__ps_modem_emit_service_added_signal((PsModem *) modem, object);

	/* Free memory */
	g_free(t_path);

	return;
}

static void __ps_modem_remove_service(PsModem *modem, gpointer service)
{
	//unregister dbus
	dbus_g_connection_unregister_g_object(modem->conn, (GObject *)service);
	//remove object from hash table
	g_hash_table_remove(modem->services, _ps_service_ref_path(service));
	g_object_unref(service);

	return;
}

static gboolean __ps_modem_set_powered(PsModem *modem, gboolean value)
{
	dbg("modem: [0x%x] Modem powered: [%s]", modem,(value ? "YES" : "NO"));
	g_return_val_if_fail(modem != NULL, FALSE);

	modem->powered = value;

	/* Emit Property change signal */
	__ps_modem_emit_property_changed_signal(modem);

	return TRUE;
}

static gboolean __ps_modem_set_sim_complete(PsModem *modem, gboolean value, gchar *operator)
{
	g_return_val_if_fail(modem != NULL, FALSE);

	modem->sim_init = value;
	if (value && operator != NULL && !modem->operator)
		modem->operator = g_strdup(operator);

	dbg("modem: [0x%x] SIM init: '%s' operator: [%s]",
				modem, (modem->sim_init ? "TRUE" : "FALSE"), modem->operator);

	/* Emit Property change signal */
	__ps_modem_emit_property_changed_signal(modem);

	return TRUE;
}

static gboolean __ps_modem_set_flight_mode(PsModem *modem, gboolean value)
{
	g_return_val_if_fail(modem != NULL, FALSE);

	modem->flight_mode = value;
	dbg("modem(%p) flight_mode(%d)", modem, modem->flight_mode);

	/* Emit Property change signal */
	__ps_modem_emit_property_changed_signal(modem);

	/* Process modem event */
	__ps_modem_processing_modem_event(modem);

	return TRUE;
}

static void __ps_modem_get_ps_setting_from_storage(GObject *object)
{
	gboolean key_3g_enable = FALSE, key_roaming_allowed = FALSE;
	PsModem *modem = NULL;

	dbg("Extract PS settings");

	modem = (PsModem *) object;
	key_3g_enable = _ps_master_get_storage_value(modem->p_master, KEY_3G_ENABLE);
	key_roaming_allowed = _ps_master_get_storage_value(modem->p_master, KEY_DATA_ROAMING_SETTING);
	msg("	Data allowed: [%s]", key_3g_enable ? "YES" : "NO");
	msg("	Roaming allowed: [%s]", key_roaming_allowed ? "YES" : "NO");

	/* Set Data allowed value */
	_ps_modem_set_data_allowed(modem, key_3g_enable);

	/* Set Roaming allowed value */
	_ps_modem_set_data_roaming_allowed(modem, key_roaming_allowed);
}

static void __ps_modem_processing_modem_event(gpointer object)
{
	PsModem * modem = object;
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

		if(!modem->powered){
			dbg("modem is not powered");
			_ps_service_remove_contexts(value);
			_ps_free_co_ps_event(value);
			_ps_free_co_network_event(value);
			__ps_modem_remove_service(modem, value);
			continue;
		}

		if(modem->flight_mode
				|| !modem->data_allowed
				|| (s_roaming && !modem->roaming_allowed) ){
			_ps_service_disconnect_contexts(value);
			continue;
		}

		/* Only available case */
		_ps_service_connect_default_context(value);
	}

	return;
}

gpointer _ps_modem_create_modem(DBusGConnection *conn, TcorePlugin *p, gpointer master,
		gchar* modem_name, gpointer co_modem)
{
	guint rv = 0;
	GObject *object;
	DBusGProxy *proxy;
	GError *error = NULL;

	dbg("Create modem object");

	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(master != NULL, NULL);

	/* Creating new Proxy */
	proxy = dbus_g_proxy_new_for_name(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus");

	if (!dbus_g_proxy_call(proxy, "RequestName", &error, G_TYPE_STRING, PS_DBUS_SERVICE,
			G_TYPE_UINT, 0, G_TYPE_INVALID, G_TYPE_UINT, &rv, G_TYPE_INVALID)) {
		err("Failed to acquire service(%s) error(%s)", PS_DBUS_SERVICE, error->message);
		return NULL;
	}
	dbg("Created Proxy - Path: [%s]", PS_DBUS_SERVICE);

	/* Creating new Object */
	object = g_object_new(PS_TYPE_MODEM, "path", modem_name, "p_master", master, "plg", p, "co_modem",
			co_modem, "conn", conn, NULL);

	/* Extract PS setting */
	__ps_modem_get_ps_setting_from_storage(object);

	/* Hook modem & SIM events */
	_ps_hook_co_modem_event(object);

	/* Extract modem values */
	_ps_get_co_modem_values(object);

	dbus_g_connection_register_g_object(conn, modem_name, object);
	msg("	modem(%p) register dbus path(%s)", object, modem_name);

	return object;
}

gboolean _ps_modem_processing_flight_mode(gpointer object, gboolean enable)
{
	PsModem * modem = object;

	dbg("Process Flight mode - Flight mode: [%s]", (enable ? "ON" : "OFF"));

	g_return_val_if_fail(modem != NULL, FALSE);

	if (modem->flight_mode == enable) {
		dbg("No change in Flight mode: [%s]",
			(modem->flight_mode ? "ON" : "OFF"));
		return TRUE;
	}

	/* Set Flight mode */
	__ps_modem_set_flight_mode(modem, enable);

	return TRUE;
}

gboolean _ps_modem_processing_power_enable(gpointer object, gboolean enable)
{
	PsModem * modem = object;

	dbg("Process Power - Enable: [%s]", (enable ? "ENABLE" : "DISABLE"));

	g_return_val_if_fail(modem != NULL, FALSE);

	if (modem->powered == enable) {
		dbg("No change in Modem Power: [%s]",
			(modem->powered ? "ENABLE" : "DISABLE"));
		return TRUE;
	}

	/* Set Modem Power */
	__ps_modem_set_powered(modem, enable);

	if (enable) {
		dbg("Create Service");
		__ps_modem_create_service(modem->conn, modem->plg, modem, modem->co_modem);
	} else {
		dbg("Process modem event");
		__ps_modem_processing_modem_event(modem);
	}

	return TRUE;
}

gboolean _ps_modem_processing_sim_complete(gpointer object, gboolean complete, gchar *operator)
{
	PsModem * modem = object;
	GHashTable *contexts = NULL;
	dbg("SIM init: '%s' Operator: [%s]", (complete ? "YES" : "NO"), operator);

	g_return_val_if_fail(modem != NULL, FALSE);

	if (modem->sim_init == complete) {
		dbg("No change in SIM init state - SIM INIT COMPLETE: [%s]",
			(modem->sim_init ? "YES" : "NO"));
		return TRUE;
	}

	/* Set SIM init */
	__ps_modem_set_sim_complete(modem, complete, operator);
	if (modem->sim_init == TRUE && modem->operator != NULL)
		contexts = _ps_context_create_hashtable(modem->conn, modem->plg, modem->operator);

	if (contexts != NULL) {
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, modem->services);

		while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
			_ps_service_ref_contexts(value, contexts, modem->operator);
		}
	}

	return TRUE;
}

gboolean _ps_modem_set_sim_enabled(gpointer object, gboolean value)
{
	PsModem * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->sim_init = value;
	dbg("modem(%p) sim_enabled(%d)", modem, modem->sim_init);
	return TRUE;
}

gboolean _ps_modem_set_data_allowed(gpointer object, gboolean value)
{
	PsModem * modem = object;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->data_allowed = value;
	dbg("modem(%p) data allowed(%d)", modem, modem->data_allowed);
	__ps_modem_emit_property_changed_signal(modem);
	__ps_modem_processing_modem_event(modem);

	return TRUE;
}

gboolean _ps_modem_get_data_allowed(gpointer object)
{
	PsModem * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->data_allowed;
}

gboolean _ps_modem_set_data_roaming_allowed(gpointer object, gboolean roaming_allowed)
{
	PsModem * modem = object;
	GHashTableIter iter;
	gpointer key, value;
	gboolean s_roaming = FALSE;

	g_return_val_if_fail(modem != NULL, FALSE);

	modem->roaming_allowed = roaming_allowed;
	dbg("modem(%p) roaming allowed(%d)", modem, modem->roaming_allowed);
	__ps_modem_emit_property_changed_signal(modem);

	if(!modem->services)
		return TRUE;

	g_hash_table_iter_init(&iter, modem->services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		s_roaming = _ps_service_get_roaming(value);
		break;
	}

	if(s_roaming)
		__ps_modem_processing_modem_event(modem);

	return TRUE;
}

gboolean _ps_modem_get_data_roaming_allowed(gpointer object)
{
	PsModem * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->roaming_allowed;
}

gboolean _ps_modem_get_flght_mode(gpointer object)
{
	PsModem * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->flight_mode;
}

gboolean _ps_modem_get_sim_init(gpointer object)
{
	PsModem * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->sim_init;
}

gboolean _ps_modem_get_power(gpointer object)
{
	PsModem * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->powered;
}

gchar* _ps_modem_ref_operator(gpointer object)
{
	PsModem * modem = object;
	g_return_val_if_fail(modem != NULL, FALSE);

	return modem->operator;
}

gboolean _ps_modem_get_properties(gpointer object, GHashTable *properties)
{
	PsModem *modem = object;

	dbg("Get modem properties");
	g_return_val_if_fail(modem != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	g_hash_table_insert(properties, "path", g_strdup(modem->path));
	g_hash_table_insert(properties, "operator", g_strdup(modem->operator));
	g_hash_table_insert(properties, "powered", BOOL2STRING(modem->powered));
	g_hash_table_insert(properties, "sim_init", BOOL2STRING(modem->sim_init));
	g_hash_table_insert(properties, "flight_mode", BOOL2STRING(modem->flight_mode));
	g_hash_table_insert(properties, "roaming_allowed", BOOL2STRING(modem->roaming_allowed));
	g_hash_table_insert(properties, "data_allowed", BOOL2STRING(modem->data_allowed));

	msg("	Path: [%s]", modem->path);
	msg("	Operator: [%s]", modem->operator);
	msg("	Powered: [%s]", modem->powered ? "YES" : "NO");
	msg("	SIM Init: [%s]", modem->sim_init ? "YES" : "NO");
	msg("	Flight mode: [%s]", modem->flight_mode ? "ON" : "OFF");
	msg("	Roaming allowed: [%s]", modem->roaming_allowed ? "YES" : "NO");
	msg("	Data allowed: [%s]", modem->data_allowed ? "YES" : "NO");

	return TRUE;
}

GHashTable* _ps_modem_ref_services(gpointer object)
{
	PsModem *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->services;
}

gchar* _ps_modem_ref_path(gpointer object)
{
	PsModem *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->path;
}

gpointer _ps_modem_ref_plugin(gpointer object)
{
	PsModem *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->plg;
}

gpointer _ps_modem_ref_dbusconn(gpointer object)
{
	PsModem *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->conn;
}

gpointer _ps_modem_ref_co_modem(gpointer object)
{
	PsModem *modem = object;
	g_return_val_if_fail(modem != NULL, NULL);

	return modem->co_modem;
}
