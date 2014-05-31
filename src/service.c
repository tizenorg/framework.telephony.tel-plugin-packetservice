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

#include "ps-service.h"

#include "ps.h"
#include "ps-error.h"

#include <core_object.h>
#include <co_ps.h>

#define PROP_DEFAULT	FALSE
#define PROP_DEFAULT_STR   NULL
#define BOOL2STRING(a)	((a==TRUE) ? ("TRUE"):("FALSE"))

#define TIMEOUT_DEFAULT		5
#define TIMEOUT_MAX			1280

guint connection_timeout;
guint timer_src;

/*Properties*/

enum {
	PROP_SERVICE_O,

	PROP_SERVICE_PATH,
	PROP_SERVICE_PLUGIN,
	PROP_SERVICE_CONN,
	PROP_SERVICE_P_MODEM,
	PROP_SERVICE_CO_NETWORK,
	PROP_SERVICE_CO_PS
};

enum {
	SIG_SERVICE_CONTEXT_ADDED,
	SIG_SERVICE_CONTEXT_REMOVED,
	SIG_SERVICE_PROPERTY_CHANGED,
	SIG_SERVICE_LAST
};

static guint32 signals[SIG_SERVICE_LAST] = {0,};

struct PsServiceClass {
	GObjectClass parent;

	//method and signals
	void (*context_added)(PsService *service, gchar *context_path);
	void (*context_removed)(PsService *service, gchar *context_path);
	void (*property_changed)(PsService *service, GHashTable *service_property);
};

struct PsService {
	GObject parent;

	gchar *path;
	TcorePlugin *plg;
	DBusGConnection *conn;
	gpointer p_modem;
	CoreObject *co_network;
	CoreObject *co_ps;

	gboolean ps_attached;
	gboolean roaming;
	enum telephony_network_access_technology act;

	GHashTable *contexts;
};

G_DEFINE_TYPE(PsService, ps_service, G_TYPE_OBJECT);

/*Function Declaration*/
static void __ps_service_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void __ps_service_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);

gboolean ps_iface_service_get_properties(PsService *service, DBusGMethodInvocation *context);
gboolean ps_iface_service_get_contexts(PsService *service, DBusGMethodInvocation *context);

static void __ps_service_emit_property_changed_signal(PsService *service);
static void __ps_service_emit_context_added_signal(PsService *service, gpointer context);
static void __ps_service_emit_context_removed_signal(PsService *service, gpointer context);

static void __remove_context(gpointer data);
static char *__ps_service_act2string(enum telephony_network_access_technology act);
static gboolean __ps_service_check_connection_option(gpointer service);
static gboolean __ps_service_connetion_timeout_handler(gpointer user_data);

#include "ps-iface-service-glue.h"

static void ps_service_init(PsService *service)
{
	dbg("service initialize");

	service->path = PROP_DEFAULT_STR;
	service->plg = NULL;
	service->conn = NULL;
	service->p_modem = NULL;
	service->co_network = NULL;
	service->co_ps = NULL;

	service->ps_attached = PROP_DEFAULT;
	service->roaming = PROP_DEFAULT;
	service->act = NETWORK_ACT_UNKNOWN;

	service->contexts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, __remove_context);
	return;
}

static void ps_service_class_init(PsServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	//class init
	dbg("class init");

	object_class->get_property = __ps_service_get_property;
	object_class->set_property = __ps_service_set_property;

	//dbus register
	dbus_g_object_type_install_info(PS_TYPE_SERVICE, &dbus_glib_ps_iface_service_object_info);

	//add properties
	g_object_class_install_property(
			object_class,
			PROP_SERVICE_PATH,
			g_param_spec_string("path", "PATH", "Technology Path", NULL,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_SERVICE_P_MODEM,
			g_param_spec_pointer("p_modem", "MODEM", "Parent Modem Object",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_SERVICE_PLUGIN,
			g_param_spec_pointer("plg", "PLUGIN", "Plug in Object",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_SERVICE_CO_NETWORK,
			g_param_spec_pointer("co_network", "COREOBJECT NETWORK", "CoreObject of Network",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_SERVICE_CO_PS,
			g_param_spec_pointer("co_ps", "COREOBJECTPS", "CoreObject of PS",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_SERVICE_CONN,
			g_param_spec_boxed("conn", "CONNECTION", "DBus connection", DBUS_TYPE_G_CONNECTION,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	//add signal handler
	signals[SIG_SERVICE_CONTEXT_ADDED] = g_signal_new("context-added", G_OBJECT_CLASS_TYPE(klass),
			G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(PsServiceClass, context_added), NULL, NULL,
			g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, DBUS_TYPE_G_STRING_STRING_HASHTABLE);

	signals[SIG_SERVICE_CONTEXT_REMOVED] = g_signal_new("context-removed",
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST,
			G_STRUCT_OFFSET(PsServiceClass, context_removed), NULL, NULL,
			g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, DBUS_TYPE_G_OBJECT_PATH);

	signals[SIG_SERVICE_PROPERTY_CHANGED] = g_signal_new("property-changed",
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST,
			G_STRUCT_OFFSET(PsServiceClass, property_changed), NULL, NULL,
			g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, DBUS_TYPE_G_STRING_STRING_HASHTABLE);

	return;
}

static void __ps_service_get_property(GObject *object, guint prop_id, GValue *value,
		GParamSpec *pspec)
{
	return;
}

static void __ps_service_set_property(GObject *object, guint prop_id, const GValue *value,
		GParamSpec *pspec)
{
	PsService *service = PS_SERVICE(object);

	switch (prop_id) {
		case PROP_SERVICE_PATH: {
			if (service->path) g_free(service->path);
			service->path = g_value_dup_string(value);
			msg("service(%p) set path(%s)", service, service->path);
		}
			break;
		case PROP_SERVICE_P_MODEM: {
			service->p_modem = g_value_get_pointer(value);
			msg("service(%p) set modem(%p)", service, service->p_modem);
		}
			break;
		case PROP_SERVICE_PLUGIN: {
			service->plg = g_value_get_pointer(value);
			msg("service(%p) set plg(%p)", service, service->plg);
		}
			break;
		case PROP_SERVICE_CO_NETWORK: {
			service->co_network = g_value_get_pointer(value);
			msg("service(%p) set co_network(%p)", service, service->co_network);
		}
			break;
		case PROP_SERVICE_CO_PS: {
			service->co_ps = g_value_get_pointer(value);
			msg("service(%p) set co_ps(%p)", service, service->co_ps);
		}
			break;
		case PROP_SERVICE_CONN: {
			service->conn = g_value_get_boxed(value);
			msg("service(%p) set conn(%p)", service, service->conn);
		}
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	} //swtich end

	return;
}

gboolean ps_iface_service_get_properties(PsService *service, DBusGMethodInvocation *context)
{
	GError *error = NULL;
	gboolean rv = FALSE;
	GHashTable *properties = NULL;

	dbg("get service properties");

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	rv = _ps_service_get_properties(service, properties);
	if (rv != TRUE) {
		g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "fail to get properties service(%p)",
				service);
		dbus_g_method_return_error(context, error);
		g_hash_table_destroy(properties);
		return FALSE;
	}

	dbus_g_method_return(context, properties);
	g_hash_table_destroy(properties);
	return TRUE;
}

gboolean ps_iface_service_get_contexts(PsService *service, DBusGMethodInvocation *context)
{
	GError *error = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *contexts;

	dbg("service get contexts interface");

	if (service->contexts == NULL) {
		g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "service(%p) does not have contexts",
				service);
		dbus_g_method_return_error(context, error);
		return FALSE;
	}

	contexts = g_hash_table_new_full(g_direct_hash, g_str_equal, g_free,
			(GDestroyNotify) g_hash_table_destroy);

	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gboolean rv = FALSE;
		gchar *path = NULL;
		GHashTable *properties = NULL;

		properties = g_hash_table_new(g_str_hash, g_str_equal);
		rv = _ps_context_get_properties(value, properties);
		if (rv != TRUE) {
			g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "fail to get properties context(%p)",
					value);
			dbus_g_method_return_error(context, error);
			g_hash_table_destroy(properties);
			g_hash_table_destroy(contexts);
			return FALSE;
		}

		path = _ps_context_ref_path(value);
		g_hash_table_insert(contexts, g_strdup(path), properties);
		dbg("service (%p) inserted into hash", value);
	}

	dbus_g_method_return(context, contexts);
	g_hash_table_destroy(contexts);

	return TRUE;
}

static void __ps_service_emit_property_changed_signal(PsService *service)
{
	GHashTable *properties = NULL;

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_service_get_properties(service, properties);
	g_signal_emit(service, signals[SIG_SERVICE_PROPERTY_CHANGED], 0, properties);
	dbg("service (%p) emit property changed signal", service);
	g_hash_table_destroy(properties);

	return;
}

static void __ps_service_emit_context_added_signal(PsService *service, gpointer context)
{
	GHashTable *properties = NULL;

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_context_get_properties(context, properties);
	g_signal_emit(service, signals[SIG_SERVICE_CONTEXT_ADDED], 0, properties);
	dbg("service (%p) emit the context(%p) added signal", service, context);
	g_hash_table_destroy(properties);
	return;
}

static void __ps_service_emit_context_removed_signal(PsService *service, gpointer context)
{
	g_signal_emit(service, signals[SIG_SERVICE_CONTEXT_REMOVED], 0, _ps_context_ref_path(context));
	dbg("service (%p) emit the context(%p) removed signal", service, context);
	return;
}

static void __remove_context(gpointer data)
{
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

static gboolean __ps_service_check_connection_option(gpointer object)
{
	gboolean b_connect = TRUE;
	gboolean power, sim, data, flight;
	PsService *service = object;

	if(service->roaming){
		b_connect &=_ps_modem_get_data_roaming_allowed(service->p_modem);
	}

	power = _ps_modem_get_power(service->p_modem);
	sim = _ps_modem_get_sim_init(service->p_modem);
	data = _ps_modem_get_data_allowed(service->p_modem);
	flight = _ps_modem_get_flght_mode(service->p_modem);
	b_connect &= power;
	b_connect &= sim;
	b_connect &= data;
	b_connect &= !flight;
	dbg("power(%d), sim init(%d), data allowed(%d), flight mode(%d)", power, sim, data, flight);

	return b_connect;
}

static gboolean __ps_service_connetion_timeout_handler(gpointer context)
{
	int rv = 0;
	PsService *service = NULL;

	service = _ps_context_ref_service(context);
	rv = _ps_service_activate_context(service, context);
	dbg("return rv(%d)", rv);

	return FALSE;
}

gpointer _ps_service_create_service(DBusGConnection *conn, TcorePlugin *p, gpointer p_modem,
		CoreObject *co_network, CoreObject *co_ps, gchar* path)
{
	guint rv = 0;
	GObject *object;
	DBusGProxy *proxy;
	GError *error = NULL;

	dbg("Create SERVICE object - Path: [%s]", path);
	g_return_val_if_fail(conn != NULL, NULL);
	g_return_val_if_fail(p_modem != NULL, NULL);

	/* Create new Proxy */
	proxy = dbus_g_proxy_new_for_name(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus");

	if (!dbus_g_proxy_call(proxy, "RequestName", &error, G_TYPE_STRING, PS_DBUS_SERVICE,
			G_TYPE_UINT, 0, G_TYPE_INVALID, G_TYPE_UINT, &rv, G_TYPE_INVALID)) {
		err("Failed to acquire context(%s) error(%s)", PS_DBUS_SERVICE, error->message);
		return NULL;
	}
	dbg("Acquired context: [%s]", PS_DBUS_SERVICE);

	/* Creating new Service object */
	object = g_object_new(PS_TYPE_SERVICE, "conn", conn, "plg", p, "p_modem", p_modem, "co_network",
			co_network, "co_ps", co_ps, "path", path, NULL);

	_ps_hook_co_network_event(object);
	_ps_get_co_network_values(object);
	_ps_hook_co_ps_event(object);

	dbus_g_connection_register_g_object(conn, path, object);
	msg("service(%p) register dbus path(%s)", object, path);

	return object;
}

gboolean _ps_service_ref_context(gpointer object, gpointer context)
{
	gpointer tmp = NULL;
	gchar *s_path = NULL;
	PsService *service = object;

	dbg("service refer to context");
	g_return_val_if_fail(service != NULL, FALSE);

	s_path = _ps_context_ref_path(context);
	tmp = g_hash_table_lookup(service->contexts, s_path);
	if (tmp != NULL) {
		dbg("context(%p) already existed", tmp);
		return FALSE;
	}

	_ps_context_set_service(context, service);
	tcore_ps_add_context(service->co_ps, (CoreObject *) _ps_context_ref_co_context(context));
	g_hash_table_insert(service->contexts, g_strdup(s_path), context);

	dbg("context(%p) insert to hash", context);
	__ps_service_emit_context_added_signal(service, context);

	//_ps_service_connect_default_context(service);
	return TRUE;
}

gboolean _ps_service_ref_contexts(gpointer object, GHashTable *contexts, gchar *operator)
{
	GHashTableIter iter;
	gpointer key, value;
	PsService *service = object;
	gboolean ret = TRUE;
	int rv;

	dbg("Service refer to Contexts");

	g_return_val_if_fail(service != NULL, FALSE);

	g_hash_table_iter_init(&iter, contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *s_path = NULL;
		gpointer tmp = NULL;
		gboolean f_awo = FALSE;

		s_path = _ps_context_ref_path(value);
		dbg("Path: [%s]", s_path);

		/* Hash lookup */
		tmp = g_hash_table_lookup(service->contexts, s_path);
		if (tmp != NULL) {
			dbg("context [0x%x] already existed", tmp);
			continue;
		}

		/* Setting service */
		_ps_context_set_service(value, service);

		/* Add Context to PS Core object */
		tcore_ps_add_context(service->co_ps, (CoreObject *)_ps_context_ref_co_context(value));

		/* Insert conetxt to Hash Table */
		g_hash_table_insert(service->contexts, g_strdup(s_path), value);

		dbg("Inserted context to Hash table - context [0x%x]", value);

		/* Emit Context added signal */
		__ps_service_emit_context_added_signal(service, value);

		f_awo = _ps_context_get_alwayson_enable(value);
		dbg("Always ON: [%s]", (f_awo ? "YES" : "NO"));
		if(f_awo) {
			dbg("Define Context");
			rv = _ps_service_define_context(service, value);
			dbg("return rv(%d)", rv);
		}
	}

	/* Update cellular state key */
	_ps_update_cellular_state_key(service);
	//_ps_service_connect_default_context(service);

	return ret;
}

gboolean _ps_service_unref_context(gpointer object, gpointer context)
{
	PsService *service = object;

	dbg("service unref contexts");
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(context != NULL, FALSE);

	dbg("remove context(%p) from service(%p)", context, service);
	tcore_ps_remove_context(service->co_ps, (CoreObject *) _ps_context_ref_co_context(context));
	g_hash_table_remove(service->contexts, _ps_context_ref_path(context));
	__ps_service_emit_context_removed_signal(service, context);

	return TRUE;
}

gboolean _ps_service_get_properties(gpointer object, GHashTable *properties)
{
	PsService *service = object;

	dbg("get service properties");
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	g_hash_table_insert(properties, "path", g_strdup(service->path));
	g_hash_table_insert(properties, "ps_attached", BOOL2STRING(service->ps_attached));
	g_hash_table_insert(properties, "roaming", BOOL2STRING(service->roaming));
	g_hash_table_insert(properties, "act", __ps_service_act2string(service->act));

	return TRUE;
}

gchar* _ps_service_ref_path(gpointer object)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->path;
}

gpointer _ps_service_ref_plugin(gpointer object)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->plg;
}

gpointer _ps_service_ref_co_network(gpointer object)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->co_network;
}

gpointer _ps_service_ref_co_ps(gpointer object)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, NULL);

	return service->co_ps;
}

gboolean _ps_service_set_context_info(gpointer object, struct tnoti_ps_pdp_ipconfiguration *devinfo)
{
	GSList* contexts = NULL;
	PsService *service = object;

	dbg("Set context information");

	g_return_val_if_fail(service != NULL, FALSE);

	/* Refer context */
	dbg("Context ID: [%d]", devinfo->context_id);
	contexts = tcore_ps_ref_context_by_id(service->co_ps, devinfo->context_id);
	if (NULL == contexts) {
		dbg("Failed to refer context");
		return FALSE;
	}

	for (; contexts != NULL; contexts = g_slist_next(contexts)) {
		CoreObject *co_context = NULL;

		co_context = contexts->data;
		if (NULL == co_context) {
			dbg("Context is NULL");
			continue;
		}

		/* Set device information */
		tcore_context_set_devinfo(co_context, devinfo);
	}

	return TRUE;
}

int _ps_service_define_context(gpointer object, gpointer context)
{
	PsService *service = object;
	CoreObject *co_context = NULL;
	gboolean b_connect = TRUE;

	dbg("define context(%p)", context);
	g_return_val_if_fail(service != NULL, FALSE);

	co_context = (CoreObject *)_ps_context_ref_co_context(context);

	b_connect = __ps_service_check_connection_option(service);
	if(!b_connect)
		return TCORE_RETURN_EPERM;

	return tcore_ps_define_context(service->co_ps, co_context, NULL);
}

int _ps_service_activate_context(gpointer object, gpointer context)
{
	PsService *service = object;
	CoreObject *co_context = NULL;
	gboolean b_connect = TRUE;
	gboolean ps_defined;
	int ret = TCORE_RETURN_FAILURE;

	dbg("Activate context [0x%x]", context);
	g_return_val_if_fail(service != NULL, TCORE_RETURN_EINVAL);

	co_context = (CoreObject *)_ps_context_ref_co_context(context);

	/* Check for connection option */
	b_connect = __ps_service_check_connection_option(service);
	dbg("Service option - PS Attached: [%s]", (service->ps_attached ? "YES" : "NO"));

	b_connect &= service->ps_attached;
	dbg("Connect: [%s]", (b_connect ? "YES" : "NO"));
	if(!b_connect)
		return TCORE_RETURN_EPERM;

	ps_defined = _ps_context_get_ps_defined(context);
	if(!ps_defined) {
		dbg("PDP profile is NOT defined!!! Need to define it first...");
		ret = tcore_ps_define_context(service->co_ps, co_context, NULL);
	}
	else {
		dbg("PDP profile is defined!!! Activate context...");
		ret = tcore_ps_activate_context(service->co_ps, co_context, NULL);
	}

	return ret;
}

gboolean _ps_service_deactivate_context(gpointer object, gpointer context)
{
	PsService *service = object;
	CoreObject *co_context = NULL;

	dbg("deactivate context(%p)", context);
	g_return_val_if_fail(service != NULL, FALSE);

	co_context = (CoreObject *)_ps_context_ref_co_context(context);

	return tcore_ps_deactivate_context(service->co_ps, co_context, NULL);
}

void _ps_service_connection_timer(gpointer object, gpointer context)
{
	gboolean f_awo = FALSE;

	f_awo = _ps_context_get_alwayson_enable(context);
	if(!f_awo)
		return;

	timer_src = g_timeout_add_seconds(connection_timeout, __ps_service_connetion_timeout_handler, context);

	dbg("cellular service timer started timer src(%d), timeout(%d)", timer_src, connection_timeout);
	connection_timeout = connection_timeout*2;
	if(connection_timeout > TIMEOUT_MAX)
		connection_timeout = TIMEOUT_MAX;

	return;
}

void _ps_service_reset_connection_timer(gpointer context)
{
	gboolean f_awo = FALSE;

	f_awo = _ps_context_get_alwayson_enable(context);
	dbg("Always ON: [%s]", (f_awo ? "YES" : "NO"));
	if(f_awo == FALSE)
		return;

	connection_timeout = TIMEOUT_DEFAULT;

	if (timer_src != 0) {
		dbg("Remove connection Retry timer [%d]", timer_src);

		g_source_remove(timer_src);
		timer_src = 0;
	}
}

void _ps_service_remove_contexts(gpointer object)
{
	GHashTableIter iter;
	gpointer key, value;
	PsService *service = object;

	dbg("service remove all contexts");
	g_return_if_fail(service != NULL);

	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gpointer co_context = NULL;

		dbg("key(%s), value(%p) context", key, value);
		co_context = _ps_context_ref_co_context(value);

		_ps_service_reset_connection_timer(value);
		_ps_context_set_alwayson_enable(value, FALSE);
		_ps_service_deactivate_context(service, value);
		_ps_context_set_connected(value, FALSE);
		tcore_ps_remove_context(service->co_ps, co_context);
		tcore_context_free(co_context);

		__ps_service_emit_context_removed_signal(service, value);
		_ps_context_remove_context(value);
	}

	g_hash_table_remove_all(service->contexts);
	return;
}

void _ps_service_disconnect_contexts(gpointer object)
{
	GHashTableIter iter;
	gpointer key, value;
	PsService *service = object;

	dbg("service disconnect all contexts");
	g_return_if_fail(service != NULL);

	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		_ps_service_reset_connection_timer(value);
		_ps_service_deactivate_context(service, value);
	}

	return;
}

void _ps_service_connect_default_context(gpointer object)
{
	GHashTableIter iter;
	gpointer key, value;
	PsService *service = object;

	dbg("Connect to 'default' context");
	g_return_if_fail(service != NULL);

	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gboolean f_awo = FALSE;

		f_awo = _ps_context_get_alwayson_enable(value);
		dbg("Always ON: [%s]", (f_awo ? "YES" : "NO"));
		if(f_awo){
			int rv = 0;

			_ps_service_reset_connection_timer(value);

			/* Activate context */
			rv = _ps_service_activate_context(service, value);
			dbg("return rv(%d)", rv);
			break;
		}
	}

	return;
}

gpointer _ps_service_return_default_context(gpointer object)
{
	GHashTableIter iter;
	gpointer key, value;
	PsService *service = object;

	g_return_val_if_fail(service != NULL, NULL);

	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gboolean b_default = FALSE;
		b_default = _ps_context_get_default_internet(value);

		if(b_default){
			return value;
		}
	}

	return NULL;
}

gboolean _ps_service_processing_network_event(gpointer object, gboolean ps_attached, gboolean roaming)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	if(service->ps_attached == ps_attached && service->roaming == roaming)
		return TRUE;

	_ps_service_set_ps_attached(service, ps_attached);
	_ps_service_set_roaming(service, roaming);
	_ps_update_cellular_state_key(service);

	if(service->ps_attached)
		_ps_service_connect_default_context(service);

	return TRUE;
}

gboolean _ps_service_set_connected(gpointer object, int context_id, gboolean enabled)
{
	GHashTableIter iter;
	gpointer key, value;
	PsService *service = NULL;

	dbg("Set service - Context ID: [%d] State: [%s]",
			context_id, (enabled ? "CONNECTED" : "NOT CONNECTED"));

	service = (PsService *) object;
	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		CoreObject *context = NULL;
		int tmp_cid;

		context = _ps_context_ref_co_context(value);
		tmp_cid = tcore_context_get_id(context);

		if (tmp_cid != context_id) continue;

		if(!enabled) {
			dbg("Clear teh context ID");
			tcore_ps_clear_context_id(service->co_ps, context);
		}

		/* Set the state */
		_ps_context_set_connected(value, enabled);
	}

	return TRUE;
}

void _ps_service_set_ps_defined(gpointer *object, gboolean value, int cid)
{
	PsService *service = (PsService*)object;
	GHashTableIter iter;
	gpointer key, out;

	dbg("PS Defined - Context ID: [%d] Value: [%d]", cid, value);

	g_return_if_fail(service != NULL);

	g_hash_table_iter_init(&iter, service->contexts);
	while (g_hash_table_iter_next(&iter, &key, &out) == TRUE) {
		gboolean r_activate = 0;

		/* Set Context */
		r_activate = _ps_context_set_ps_defined(out, value, cid);
		r_activate &= value;

		dbg("Activate context: [%s]", (r_activate ? "YES" : "NO"));
		if(r_activate) {
			int rv;

			dbg("Activate context - Context ID: [%d]", cid);
			rv = _ps_service_activate_context(service, out);
			dbg("Activate context request - %s", (rv == TCORE_RETURN_SUCCESS ? "SUCCESS" : "FAIL"));
			break;
		}
	}

	return;
}

gboolean _ps_service_set_ps_attached(gpointer object, gboolean value)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	service->ps_attached = value;
	dbg("service(%p) ps_attached(%d)", service, service->ps_attached);

	return TRUE;
}

gboolean _ps_service_get_roaming(gpointer object)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	return service->roaming;
}

gboolean _ps_service_set_roaming(gpointer object, gboolean value)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	service->roaming = value;
	dbg("service(%p) roaming(%d)", service, service->roaming);
	__ps_service_emit_property_changed_signal(service);

	return TRUE;
}

gboolean _ps_service_set_access_technology(gpointer object,
		enum telephony_network_access_technology value)
{
	PsService *service = object;
	g_return_val_if_fail(service != NULL, FALSE);

	service->act = value;
	dbg("service(%p) Access Technology(%d)", service, service->act);

	if(service->act > NETWORK_ACT_UNKNOWN && service->act < NETWORK_ACT_NOT_SPECIFIED){
		_ps_update_cellular_state_key(service);
		_ps_service_connect_default_context(service);
	}

	return TRUE;
}

enum telephony_ps_state _ps_service_check_cellular_state(gpointer object)
{
	gboolean state = FALSE;
	PsService *service = object;
	g_return_val_if_fail(service != NULL, TELEPHONY_PS_NO_SERVICE);

	state = _ps_modem_get_power(service->p_modem);
	if(!state){
		return TELEPHONY_PS_NO_SERVICE;
	}

	state = _ps_modem_get_sim_init(service->p_modem);
	if(!state){
		return TELEPHONY_PS_NO_SERVICE;
	}

	state = _ps_modem_get_flght_mode(service->p_modem);
	if(state){
		return TELEPHONY_PS_FLIGHT_MODE;
	}

	if(!service->ps_attached){
		return TELEPHONY_PS_NO_SERVICE;
	}

	state = _ps_modem_get_data_allowed(service->p_modem);
	if(!state){
		return TELEPHONY_PS_3G_OFF;
	}

	state = _ps_modem_get_data_roaming_allowed(service->p_modem);
	if(service->roaming && !state){
		return TELEPHONY_PS_ROAMING_OFF;
	}

	return TELEPHONY_PS_ON;
}
