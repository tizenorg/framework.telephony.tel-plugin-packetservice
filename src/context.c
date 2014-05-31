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

#include "ps-context.h"

#include "ps.h"
#include "ps-error.h"

#include <tcore.h>
#include <plugin.h>
#include <server.h>
#include <storage.h>
#include <core_object.h>
#include <co_context.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <iniparser.h>

#define PROP_DEFAULT		FALSE
#define PROP_DEFAULT_STR	NULL
#define BOOL2STRING(a)		((a==TRUE) ? ("TRUE"):("FALSE"))
#define DATABASE_PATH		"/opt/dbspace/.dnet.db"

/*Properties*/
enum {
	PROP_CONTEXT_O,

	PROP_CONTEXT_PATH,
	PROP_CONTEXT_CONN,
	PROP_CONTEXT_PLUGIN,
	PROP_CONTEXT_MCCMNC
};

enum {
	SIG_CONTEXT_PROPERTY_CHANGED,
	SIG_CONTEXT_LAST
};

static guint32 signals[SIG_CONTEXT_LAST] = { 0, };

struct PsContextClass {
	GObjectClass parent;

	//method and signals
	void (*property_changed)(PsContext *context, GHashTable *context_property);
};

struct PsContext {
	GObject parent;

	gchar* path;
	gchar* mccmnc;
	DBusGConnection *conn;
	TcorePlugin *plg;

	gboolean alwayson;
	gboolean default_internet;
	gboolean hidden;
	gboolean editable;
	gboolean ps_defined;
	gpointer p_service;
	int profile_id;
	CoreObject *co_context;
};

static Storage *strg_db;
static gpointer handle;
static GHashTable *contexts;

G_DEFINE_TYPE(PsContext, ps_context, G_TYPE_OBJECT);

static void     __ps_context_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void     __ps_context_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

gboolean        ps_iface_context_get_properties(PsContext *pscontext, DBusGMethodInvocation *context);
gboolean        ps_iface_context_get_profile(PsContext *pscontext, DBusGMethodInvocation *context);
gboolean        ps_iface_context_activate(PsContext *pscontext, DBusGMethodInvocation *context);
gboolean        ps_iface_context_deactivate(PsContext *pscontext, DBusGMethodInvocation *context);
gboolean        ps_iface_context_set_default_connection(PsContext *pscontext, gboolean* result, GError **error);
gboolean        ps_iface_context_modify_profile(PsContext *pscontext, GHashTable *profile_property, gboolean* result, GError **error);
gboolean        ps_iface_context_remove_profile(PsContext *pscontext, gboolean* result, GError **error);

static void     __ps_context_emit_property_changed_signal(PsContext *context);

static void     __remove_context(gpointer data);
static gboolean __remove_contexts(gpointer key, gpointer value, gpointer user_data);
static gboolean __ps_context_remove_context(gpointer context);
static gboolean __ps_context_create_storage_handle(gpointer plugin);
static gboolean __ps_context_create_context_hash(void);
static gchar*   __ps_context_create_path(char *profile_name, int profile_id, int svc_ctg_id);
static gboolean __ps_context_create_co_context(gpointer context, GHashTable *property);
static gboolean __ps_context_update_profile(PsContext *context, GHashTable *property);
static gboolean __ps_context_update_database(PsContext *context);
static gboolean __ps_context_update_default_internet_to_db(PsContext *context, gboolean enabled);
static gboolean __ps_context_remove_database(PsContext *context);
static int      __ps_context_insert_network_id_to_database(gchar *mccmnc);
static int      __ps_context_load_network_id_from_database(gchar *mccmnc);
static gchar*   __ps_context_load_network_name_from_database(int network_id);
static int      __ps_context_load_profile_id_from_database(void);
static gboolean __ps_context_insert_profile_tuple(dictionary *dic, int index);
static int      __ps_context_insert_profile_to_database(GHashTable *property, int network_id);
static int      __ps_context_get_network_id(gchar *mccmnc);
static gboolean __ps_context_get_profile_properties(gpointer context, GHashTable *properties);
static gboolean __ps_context_set_default_connection_enable(gpointer object, gboolean enabled);

#include "ps-iface-context-glue.h"

static void ps_context_init(PsContext *context)
{
	dbg("context initialize");

	context->path = NULL;
	context->mccmnc = NULL;
	context->conn = NULL;
	context->plg = NULL;

	context->alwayson = PROP_DEFAULT;
	context->default_internet = PROP_DEFAULT;
	context->hidden = PROP_DEFAULT;
	context->editable = PROP_DEFAULT;
	context->p_service = NULL;
	context->profile_id = 0;
	context->co_context = NULL;

	return;
}

static void ps_context_class_init(PsContextClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	//class init
	dbg("context class init");

	object_class->get_property = __ps_context_get_property;
	object_class->set_property = __ps_context_set_property;

	//dbus register
	dbus_g_object_type_install_info(PS_TYPE_CONTEXT, &dbus_glib_ps_iface_context_object_info);

	//property add
	g_object_class_install_property(
			object_class,
			PROP_CONTEXT_CONN,
			g_param_spec_boxed("conn", "CONNECTION", "DBus connection", DBUS_TYPE_G_CONNECTION,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property(
			object_class,
			PROP_CONTEXT_PATH,
			g_param_spec_string("path", "PATH", "Context Path", PROP_DEFAULT_STR,
					G_PARAM_READWRITE));
	g_object_class_install_property(
			object_class,
			PROP_CONTEXT_MCCMNC,
			g_param_spec_string("mccmnc", "MCCMNC", "Profile Country and Context Provider Code",
					PROP_DEFAULT_STR, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property(
			object_class,
			PROP_CONTEXT_PLUGIN,
			g_param_spec_pointer("plg", "PLUGIN", "Plug in Object",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	//add signal handler
	signals[SIG_CONTEXT_PROPERTY_CHANGED] = g_signal_new("property-changed",
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST,
			G_STRUCT_OFFSET(PsContextClass, property_changed), NULL, NULL,
			g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, DBUS_TYPE_G_STRING_STRING_HASHTABLE);

	return;
}

static void __ps_context_set_property(GObject *object, guint prop_id, const GValue *value,
		GParamSpec *pspec)
{
	PsContext *context = PS_CONTEXT(object);

	switch (prop_id) {
		case PROP_CONTEXT_CONN: {
			context->conn = g_value_get_boxed(value);
			msg("context (%p) set conn(%p)", context, context->conn);
		}
			break;
		case PROP_CONTEXT_PLUGIN: {
			context->plg = g_value_get_pointer(value);
			msg("context (%p) set plg(%p)", context, context->plg);
		}
			break;
		case PROP_CONTEXT_PATH: {
			if (context->path) {
				g_free(context->path);
			}
			context->path = g_value_dup_string(value);
			msg("context (%p) path(%s)", context, context->path);
		}
			break;
		case PROP_CONTEXT_MCCMNC: {
			if (context->mccmnc) {
				g_free(context->mccmnc);
			}
			context->mccmnc = g_value_dup_string(value);
			msg("context (%p) mccmnc(%s)", context, context->mccmnc);
		}
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	} //end of switch

	return;
}

static void __ps_context_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	return;
}

static void __ps_context_emit_property_changed_signal(PsContext *context)
{
	GHashTable *property;

	property = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_context_get_properties(context, property);
	g_signal_emit(context, signals[SIG_CONTEXT_PROPERTY_CHANGED], 0, property);
	g_hash_table_destroy(property);
	dbg("context (%p) emit the context property changed signal", context);
	return;
}

gboolean ps_iface_context_get_properties(PsContext *pscontext, DBusGMethodInvocation *context)
{
	GHashTable *property;

	dbg("Get properties of context(%s)", _ps_context_ref_path(pscontext));
	property = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_context_get_properties(pscontext, property);

	dbus_g_method_return(context, property);
	g_hash_table_destroy(property);
	return TRUE;
}

gboolean ps_iface_context_get_profile(PsContext *pscontext, DBusGMethodInvocation *context)
{
	GHashTable *profile;

	dbg("Get profile properties of context(%s)", _ps_context_ref_path(pscontext));
	profile = g_hash_table_new(g_str_hash, g_str_equal);
	__ps_context_get_profile_properties(pscontext, profile);

	dbus_g_method_return(context, profile);
	g_hash_table_destroy(profile);
	return TRUE;
}

gboolean ps_iface_context_activate(PsContext *pscontext, DBusGMethodInvocation *context)
{
	int rv = 0;
	int context_state = 0;
	GError *error = NULL;

	dbg("Activate context - Path: [%s]", _ps_context_ref_path(pscontext));

	/* Support Always ON connection */
	_ps_context_set_alwayson_enable(pscontext, TRUE);

	/* Reset connection timer */
	_ps_service_reset_connection_timer(pscontext);

	/* Service Activate context */
	rv = _ps_service_activate_context(pscontext->p_service, pscontext);
	if (rv != TCORE_RETURN_SUCCESS) {
		err("Context Activation - FAIL");
		g_set_error(&error, PS_ERROR, PS_ERR_TRASPORT, "fail to activate context err(%d)", rv);
		goto FAIL;
	}
	dbg("Context Activation - SUCCESS");

	dbus_g_method_return(context, pscontext->path);

	/* Get context State, Set connected if it is already Activated */
	context_state = tcore_context_get_state(pscontext->co_context);
	if (context_state == CONTEXT_STATE_ACTIVATED) {
		dbg("Conetxt State - Already connected");
		_ps_context_set_connected(pscontext, TRUE);
	}

	return TRUE;

FAIL:
	dbus_g_method_return_error(context, error);
	return TRUE;
}

gboolean ps_iface_context_deactivate(PsContext *pscontext, DBusGMethodInvocation *context)
{
	int rv = 0;
	int context_state = 0;
	GError *error = NULL;

	dbg("deactivate context(%s)", _ps_context_ref_path(pscontext));

	_ps_service_reset_connection_timer(pscontext);
	_ps_context_set_alwayson_enable(pscontext, FALSE);
	rv = _ps_service_deactivate_context(pscontext->p_service, pscontext);
	if (rv != TCORE_RETURN_SUCCESS) {
		dbg("fail to deactivate context connection");
		g_set_error(&error, PS_ERROR, PS_ERR_TRASPORT, "fail to deactivate context err(%d)", rv);
		goto FAIL;
	}

	dbg("success to deactivate context");
	dbus_g_method_return(context, pscontext->path);

	context_state =  tcore_context_get_state(pscontext->co_context);
	if (context_state == CONTEXT_STATE_DEACTIVATED) {
		dbg("context is already disconnected");
		_ps_context_set_connected(pscontext, FALSE);
	}

	return TRUE;

FAIL:
	dbus_g_method_return_error(context, error);
	return TRUE;
}

gboolean ps_iface_context_set_default_connection(PsContext *pscontext, gboolean* result, GError **error)
{
/*
 * if current default and target profile is the same, just return success
 *
 * in different case, current connection should be diconnected, and trying to connection new one
 */
	int role = CONTEXT_ROLE_UNKNOWN;
	gpointer cur_default_ctx = NULL;
	gpointer service = NULL;

	role = tcore_context_get_role(pscontext->co_context);
	if(role != CONTEXT_ROLE_INTERNET){
		dbg("only internet profile type can be set to default internet profile");
		*result = FALSE;
		return TRUE;
	}

	service = pscontext->p_service;
	cur_default_ctx = _ps_service_return_default_context(service);

	dbg("default ctx(%p), request ctx(%p)", cur_default_ctx, pscontext);
	if(cur_default_ctx == pscontext){
		dbg("already default internet connection");
		*result = TRUE;
		return TRUE;
	}

	//unset current profile
	_ps_context_set_alwayson_enable(cur_default_ctx, FALSE);
	__ps_context_set_default_connection_enable(cur_default_ctx, FALSE);
	//disconnect connection
	_ps_service_deactivate_context(((PsContext *)cur_default_ctx)->p_service, cur_default_ctx);
	//db update - release default connection
	__ps_context_update_default_internet_to_db((PsContext *)cur_default_ctx, FALSE);

	//db update - set default connection
	__ps_context_update_default_internet_to_db(pscontext, TRUE);
	//set request profile
	__ps_context_set_default_connection_enable(pscontext, TRUE);
	_ps_context_set_alwayson_enable(pscontext, TRUE);
	//request to connect
	_ps_service_connect_default_context(pscontext->p_service);
	dbg("complete to change the default connection");

	*result = TRUE;
	return TRUE;
}

gboolean ps_iface_context_modify_profile(PsContext *context, GHashTable *profile_property,
		gboolean* result, GError **error)
{
	gboolean rv = FALSE;
	int context_state = 0;

	*result = TRUE;
	dbg("modify context's profile properties");

	rv = __ps_context_update_profile(context, profile_property);
	if (rv != TRUE) {
		g_set_error(error, PS_ERROR, PS_ERR_INTERNAL, "fail to modify profile");
		*result = FALSE;
		return TRUE;
	}

	context_state = tcore_context_get_state(context->co_context);
	if (context_state == CONTEXT_STATE_DEACTIVATED)
		return TRUE;

	_ps_service_deactivate_context(context->p_service, context);
	context_state = tcore_context_get_state(context->co_context);
	if (context_state == CONTEXT_STATE_DEACTIVATED) {
		dbg("context is already disconnected");
		_ps_context_set_connected(context, FALSE);
	}

	return TRUE;
}

gboolean ps_iface_context_remove_profile(PsContext *context, gboolean* result, GError **error)
{
	gchar* ctx_path = NULL;
	gboolean rv = FALSE;

	ctx_path = g_strdup(_ps_context_ref_path(context));

	__ps_context_remove_database(context);
	rv = __ps_context_remove_context(context);
	g_hash_table_remove(contexts, ctx_path);

	*result = rv;
	g_free(ctx_path);
	return TRUE;
}

static void __remove_context(gpointer data)
{
	dbg("context removed");
	return;
}

static gboolean __remove_contexts(gpointer key, gpointer value, gpointer user_data)
{
	gchar *context_path = (gchar *) key;
	dbg("context(%s) remove", context_path);
	__ps_context_remove_context(value);
	return TRUE;
}

gboolean _ps_context_remove_context(gpointer context)
{
	dbus_g_connection_unregister_g_object(((PsContext *)context)->conn, (GObject *) context);
	g_hash_table_remove(contexts, _ps_context_ref_path(context));
	g_object_unref(context);
	return TRUE;
}

static gboolean __ps_context_remove_context(gpointer context)
{
	dbg("remove context and profile");

	_ps_service_reset_connection_timer(context);

	dbus_g_connection_unregister_g_object(((PsContext *)context)->conn, (GObject *) context);

	_ps_context_set_alwayson_enable(context, FALSE);
	_ps_service_deactivate_context(((PsContext *)context)->p_service, context);
	_ps_context_set_connected(context, FALSE);
	_ps_service_unref_context(((PsContext *)context)->p_service, context);

	tcore_context_free(((PsContext *)context)->co_context);
	g_object_unref(context);

	return TRUE;
}

static gboolean __ps_context_create_storage_handle(gpointer plugin)
{
	TcorePlugin *p = plugin;
	Server *s = tcore_plugin_ref_server(p);
	strg_db = tcore_server_find_storage(s, "database");

	handle = tcore_storage_create_handle(strg_db, DATABASE_PATH);
	if (!handle)
		err("fail to create database handle");

	dbg("storage(%p) handle (%p)", strg_db, handle);
	return TRUE;
}

static gboolean __ps_context_create_context_hash()
{
	g_return_val_if_fail(contexts == NULL, FALSE);

	contexts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, __remove_context);
	if (contexts == NULL) {
		err("fail to create context hashtable");
		return FALSE;
	}

	dbg("context hashtable(%p)", contexts);
	return TRUE;
}

static gchar* __ps_context_create_path(char *profile_name, int profile_id, int svc_ctg_id)
{
	gchar **strv, *str, *path;
	gchar *delimiters = " !\"#$%&\'()*+,-./:;<=>?@[\\]^`{|}~";

	strv = g_strsplit_set(profile_name, delimiters, -1);
	str = g_strjoinv("_", strv);
	dbg("converted string %s", str);
	path = g_strdup_printf("/context/%s_%d_%d", str, profile_id, svc_ctg_id);
	g_strfreev(strv);
	g_free(str);
	dbg("path (%s)", path);

	return path;
}

static gboolean __ps_context_create_co_context(gpointer object, GHashTable *property)
{
	GHashTableIter iter;
	gpointer key, value;
	PsContext *context = NULL;
	CoreObject *co_context = NULL;

	gchar *path = NULL;
	int profile_id = 0;
	gchar *profile_name = NULL;
	gchar *apn = NULL;
	gchar *auth_id = NULL, *auth_pwd = NULL, *home_url = NULL, *proxy_addr = NULL;
	int auth_type = 0,svc_ctg_id = 0;
	gboolean hidden = FALSE, editable = FALSE, default_conn = FALSE;

	dbg("Create context");

	g_hash_table_iter_init(&iter, (GHashTable *) property);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		if (g_str_equal(key, "3") == TRUE) { /*Profile ID*/
			profile_id = atoi((const char*) value);
			dbg("profile id (%d)", profile_id);
		}
		else if (g_str_equal(key, "4") == TRUE) {
			profile_name = g_strdup((const char*) value);
			dbg("profile name (%s)", profile_name);
		}
		else if (g_str_equal(key, "5") == TRUE) {
			apn = g_strdup((const char*) value);
			dbg("APN (%s)", apn);
		}
		else if (g_str_equal(key, "6") == TRUE) {
			auth_type = atoi((const char*) value);
			dbg("auth type (%d)", auth_type);
		}
		else if (g_str_equal(key, "7") == TRUE) {
			auth_id = g_strdup((const char*) value);
			dbg("auth id (%s)", auth_id);
		}
		else if (g_str_equal(key, "8") == TRUE) {
			auth_pwd = g_strdup((const char*) value);
			dbg("auth pwd (%s)", auth_pwd);
		}
		else if (g_str_equal(key, "9") == TRUE) {
			proxy_addr = g_strdup((const char*) value);
			dbg("proxy addr (%s)", proxy_addr);
		}
		else if (g_str_equal(key, "10") == TRUE) {
			home_url = g_strdup((const char*) value);
			dbg("home url (%s)", home_url);
		}
		else if (g_str_equal(key, "19") == TRUE) {
			svc_ctg_id = atoi((const char*) value);
			dbg("context category type (%d)", svc_ctg_id);
		}
		else if (g_str_equal(key, "20") == TRUE) {
			hidden = atoi((const char*) value);
			dbg("hidden profile (%d)", hidden);
		}
		else if (g_str_equal(key, "21") == TRUE) {
			editable = atoi((const char*) value);
			dbg("editable profile (%d)", editable);
		}
		else if (g_str_equal(key, "22") == TRUE) {
			default_conn = atoi((const char*) value);
			dbg("default connection profile (%d)", default_conn);
		}
	}

	path = __ps_context_create_path(profile_name, profile_id, svc_ctg_id);

	context = (PsContext *) object;
	co_context = tcore_context_new(context->plg, NULL);
	tcore_context_set_state(co_context, CONTEXT_STATE_DEACTIVATED);
	tcore_context_set_role(co_context, svc_ctg_id);
	tcore_context_set_apn(co_context, apn);
	tcore_context_set_auth(co_context, auth_type);
	tcore_context_set_username(co_context, auth_id);
	tcore_context_set_password(co_context, auth_pwd);
	tcore_context_set_proxy(co_context, proxy_addr);
	tcore_context_set_mmsurl(co_context, home_url);
	tcore_context_set_profile_name(co_context, profile_name);

	context->profile_id = profile_id;
	context->hidden = hidden;
	context->editable = editable;
	context->default_internet = default_conn;
	context->path = g_strdup(path);
	context->co_context = co_context;

	msg("	Profile ID: [%d]", context->profile_id);
	msg("	Profile Hidden: [%d]", (context->hidden ? "YES" : "NO"));
	msg("	Profile Editable: [%d]", (context->editable ? "YES" : "NO"));
	msg("	Profile - Default Internet: [%s]",
			(context->default_internet ? "YES" : "NO"));
	msg("	Path: [%s]", context->path);
	msg("	Context: [0x%x]", context->co_context);

	/* Free memory */
	g_free(path);
	return TRUE;
}

static gpointer __ps_context_create_context(DBusGConnection *conn, TcorePlugin *p,
		gchar *mccmnc, GHashTable *property)
{
	guint rv = 0;
	GError *error = NULL;
	DBusGProxy *proxy;
	GObject *object = NULL;
	gchar *path = NULL;

	proxy = dbus_g_proxy_new_for_name(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus");

	if (!dbus_g_proxy_call(proxy, "RequestName", &error, G_TYPE_STRING, PS_DBUS_SERVICE,
			G_TYPE_UINT, 0, G_TYPE_INVALID, G_TYPE_UINT, &rv, G_TYPE_INVALID)) {
		err("Failed to acquire context(%s) error(%s)", PS_DBUS_SERVICE, error->message);
		return NULL;
	}

	object = g_object_new(PS_TYPE_CONTEXT, "conn", conn, "plg", p, "mccmnc", mccmnc);

	__ps_context_create_co_context(object, property);
	_ps_context_set_alwayson_enable(object, TRUE);
	path = _ps_context_ref_path(object);

	dbus_g_connection_register_g_object(conn, g_strdup(path), object);
	msg("context(%p) register dbus path(%s)", object, path);

	return object;
}

static gboolean __ps_context_update_profile(PsContext *context, GHashTable *property)
{
	CoreObject *co_context = NULL;
	GHashTableIter iter;
	gpointer key, value;

	co_context = context->co_context;
	if (!co_context)
		return FALSE;

	g_hash_table_iter_init(&iter, property);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		if (g_str_equal(key, "apn") == TRUE) {
			tcore_context_set_apn(co_context, (const char *) value);
		}
		else if (g_str_equal(key, "auth_type") == TRUE) {
			int i_tmp = 0;
			i_tmp = atoi((const char *) value);
			tcore_context_set_auth(co_context, i_tmp);
		}
		else if (g_str_equal(key, "auth_id") == TRUE) {
			tcore_context_set_username(co_context, (const char *) value);
		}
		else if (g_str_equal(key, "auth_pwd") == TRUE) {
			tcore_context_set_password(co_context, (const char *) value);
		}
		else if (g_str_equal(key, "proxy_addr") == TRUE) {
			tcore_context_set_proxy(co_context, (const char *) value);
		}
		else if (g_str_equal(key, "home_url") == TRUE) {
			tcore_context_set_mmsurl(co_context, (const char *) value);
		}
	}

	return __ps_context_update_database(context);
}

static gboolean __ps_context_update_default_internet_to_db(PsContext *context, gboolean enabled)
{
	gchar *s_id = NULL, *s_enabled = NULL;
	gboolean rv = FALSE;
	char szQuery[3000];

	GHashTable *in_param;
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	strcpy(szQuery, " update pdp_profile set ");
	strcat(szQuery, " default_internet_con = ?");
	strcat(szQuery, " where profile_id = ?");

	s_id = g_strdup_printf("%d", context->profile_id);
	s_enabled = g_strdup_printf("%d", enabled);

	g_hash_table_insert(in_param, "1", g_strdup(s_enabled));
	g_hash_table_insert(in_param, "2", g_strdup(s_id));

	rv = tcore_storage_update_query_database(strg_db, handle, szQuery, in_param);
	g_hash_table_destroy(in_param);

	g_free(s_id);
	g_free(s_enabled);

	return rv;
}

static gboolean __ps_context_update_database(PsContext *context)
{
	gchar *s_id = NULL, *s_authtype = NULL;
	gchar *s_apn = NULL, *s_username = NULL, *s_pwd = NULL, *s_proxy = NULL, *s_mms = NULL;
	gboolean rv = FALSE;
	char szQuery[3000];

	GHashTable *in_param;
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	strcpy(szQuery, " update pdp_profile set ");
	strcat(szQuery, " apn = ?, auth_type = ?, auth_id = ?, auth_pwd = ?, ");
	strcat(szQuery, " proxy_ip_addr = ?, home_url = ?");
	strcat(szQuery, " where profile_id = ?");

	s_id = g_strdup_printf("%d", context->profile_id);
	s_authtype = g_strdup_printf("%d", tcore_context_get_auth(context->co_context));

	s_apn = tcore_context_get_apn(context->co_context);
	s_username = tcore_context_get_username(context->co_context);
	s_pwd = tcore_context_get_password(context->co_context);
	s_proxy = tcore_context_get_proxy(context->co_context);
	s_mms = tcore_context_get_mmsurl(context->co_context);

	g_hash_table_insert(in_param, "1", g_strdup(s_apn));
	g_hash_table_insert(in_param, "2", g_strdup(s_authtype));
	g_hash_table_insert(in_param, "3", g_strdup(s_username));
	g_hash_table_insert(in_param, "4", g_strdup(s_pwd));
	g_hash_table_insert(in_param, "5", g_strdup(s_proxy));
	g_hash_table_insert(in_param, "6", g_strdup(s_mms));
	g_hash_table_insert(in_param, "7", g_strdup(s_id));

	rv = tcore_storage_update_query_database(strg_db, handle, szQuery, in_param);
	g_hash_table_destroy(in_param);

	g_free(s_id);
	g_free(s_authtype);
	g_free(s_apn);
	g_free(s_username);
	g_free(s_pwd);
	g_free(s_proxy);
	g_free(s_mms);

	return rv;
}

static gboolean __ps_context_remove_database(PsContext *context)
{
	gchar *s_id = NULL;
	gboolean rv = FALSE;
	char szQuery[1000];

	GHashTable *in_param;
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery, " delete from pdp_profile where profile_id = ? ");

	s_id = g_strdup_printf("%d", context->profile_id);
	g_hash_table_insert(in_param, "1", g_strdup(s_id));

	rv = tcore_storage_remove_query_database(strg_db, handle, szQuery, in_param);
	g_free(s_id);
	g_hash_table_destroy(in_param);

	return rv;
}

static int __ps_context_insert_network_id_to_database(gchar *mccmnc)
{
	char szQuery[5000];
	int network_id = 0;
	gboolean rv = FALSE;
	gchar *insert_key = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *in_param, *out_param;

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
				(GDestroyNotify) g_hash_table_destroy);

	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery,"select max(network_info_id) as network_id from network_info");

	tcore_storage_read_query_database(strg_db, handle, szQuery, NULL, out_param, 1);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if(value){
			g_hash_table_iter_init(&iter2, (GHashTable *) value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				dbg("key2(%s) value2(%s)",key2, value2);
				if (g_str_equal(key2, "0") == TRUE) {
					if (!value2 || g_strcmp0((const char*) value2, "") == 0 ) {
						network_id = 0;
					}
					else{
						network_id = atoi((const char*) value2);
					}
				}
			}
			break;
		}
	}

	g_hash_table_destroy(out_param);
	network_id++;


	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery," insert into network_info( network_info_id, network_name, mccmnc) values( ?, ?, ?) ");

	insert_key = g_strdup_printf("%d", network_id);
	g_hash_table_insert(in_param, "1", g_strdup(insert_key));
	g_hash_table_insert(in_param, "2", "TEMP_NETWORK");
	g_hash_table_insert(in_param, "3", g_strdup(mccmnc));

	rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
	if(!rv)
		return 0;

	g_free(insert_key);
	return network_id;
}

static int __ps_context_insert_profile_to_database(GHashTable *property, int network_id)
{
	int profile_id = 0;
	char szQuery[5000];

	gboolean rv = FALSE;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *in_param;
	gchar *insert_key1 = NULL, *insert_key2 = NULL;
	gchar *profile_name=NULL, *apn=NULL, *auth_type=NULL, *auth_id = NULL, *auth_pwd = NULL;
	gchar *proxy_addr = NULL, *home_url = NULL, *svc_id = NULL, *keyword=NULL, *network_name= NULL;

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	g_hash_table_iter_init(&iter, property);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {

		if (g_str_equal(key, "apn") == TRUE) {
			apn = g_strdup(value);
		}
		else if (g_str_equal(key, "keyword") == TRUE) {
			keyword = g_strdup(value);
		}
		else if (g_str_equal(key, "auth_type") == TRUE) {
			auth_type = g_strdup(value);
		}
		else if (g_str_equal(key, "auth_id") == TRUE) {
			auth_id = g_strdup(value);
		}
		else if (g_str_equal(key, "auth_pwd") == TRUE) {
			auth_pwd = g_strdup(value);
		}
		else if (g_str_equal(key, "proxy_addr") == TRUE) {
			proxy_addr = g_strdup(value);
		}
		else if (g_str_equal(key, "home_url") == TRUE) {
			home_url = g_strdup(value);
		}
		else if (g_str_equal(key, "svc_ctg_id") == TRUE) {
			svc_id = g_strdup(value);
		}

	}

	/*
	 * Whether a profile is created with no proxy address and port,
	 * settings application is passing ':' as proxy_addr value.
	 * Checking proxy _address creation on application is needed, but
	 * address:port formatting is also double check in packet service
	 * telephony plugin.
	 */
	if (g_strcmp0(proxy_addr, ":") == 0) {
		dbg("Invalid proxy address, set it to NULL");
		g_free(proxy_addr);
		proxy_addr = NULL;
	}

	dbg("apn (%s), auth_type (%s), auth_id(%s), auth_pwd(%s), proxy_addr(%s), home_url(%s), svc_id(%s)",
		apn, auth_type, auth_id, auth_pwd, proxy_addr, home_url, svc_id);

	profile_id = __ps_context_load_profile_id_from_database();
	if(profile_id < 0){
		dbg("fail to get last profile id");
		return 0;
	}
	dbg("last profile id(%d)", profile_id);
	profile_id++;

	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery," insert into pdp_profile( ");
	strcat(szQuery," profile_id, profile_name, apn, auth_type, auth_id, auth_pwd, ");
	strcat(szQuery," pdp_protocol, proxy_ip_addr, home_url, linger_time, ");
	strcat(szQuery," network_info_id, svc_category_id, hidden, editable, default_internet_con) values( ");
	strcat(szQuery," ?, ?, ?, ?, ?, ?,");//1,2,3,4,5,6
	strcat(szQuery," 1, ?, ?, 300,");//7,8
	strcat(szQuery," ?, ?, 0, 1, ?)");//9,10,11

	insert_key1 = g_strdup_printf("%d", profile_id);
	insert_key2 = g_strdup_printf("%d", network_id);
	network_name = __ps_context_load_network_name_from_database(network_id);

	if(keyword){
		profile_name = g_strdup_printf("%s (%s)", network_name, keyword);
	}
	else{
		profile_name = g_strdup_printf("%s", network_name);
	}
	dbg("profile name (%s)", profile_name);

	g_hash_table_insert(in_param, "1", g_strdup(insert_key1));
	g_hash_table_insert(in_param, "2", g_strdup(profile_name));
	g_hash_table_insert(in_param, "3", g_strdup(apn));
	g_hash_table_insert(in_param, "4", g_strdup(auth_type));
	g_hash_table_insert(in_param, "5", g_strdup(auth_id));
	g_hash_table_insert(in_param, "6", g_strdup(auth_pwd));
	g_hash_table_insert(in_param, "7", g_strdup(proxy_addr));
	g_hash_table_insert(in_param, "8", g_strdup(home_url));
	g_hash_table_insert(in_param, "9", g_strdup(insert_key2));
	g_hash_table_insert(in_param, "10", g_strdup(svc_id));

	/* If profile received is Internet type */
	if (g_strcmp0(svc_id, "1") == 0) {
		dbg("Set new internet profile as default Internet connection");
		g_hash_table_insert(in_param, "11", g_strdup("1"));
	/* Profile is MMS or other type don't set it as default */
	} else
		g_hash_table_insert(in_param, "11", g_strdup("0"));

	g_free(insert_key1);g_free(insert_key2);g_free(profile_name);
	g_free(apn);g_free(auth_type);g_free(auth_id);g_free(auth_pwd);
	g_free(proxy_addr);g_free(home_url);g_free(svc_id);

	rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
	g_hash_table_destroy(in_param);

	if(!rv)
		return 0;

	return profile_id;
}

static int __ps_context_load_network_id_from_database(gchar *mccmnc)
{
	char szQuery[5000];
	int network_id = 0;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *in_param, *out_param;

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery,"select network_info_id from network_info where mccmnc = ? ");

	g_hash_table_insert(in_param, "1", g_strdup(mccmnc));
	tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 1);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if(value){
			g_hash_table_iter_init(&iter2, (GHashTable *) value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					if (!value2 || g_strcmp0((const char*) value2, "") == 0) {
						network_id = 0;
					}
					else{
						network_id = atoi((const char*) value2);
					}
				}
			}
			break;
		}
	}

	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	return network_id;
}

static gchar* __ps_context_load_network_name_from_database(int network_id)
{
	char szQuery[5000];
	gchar *network_name = NULL;
	gchar *insert_key1 = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *in_param, *out_param;

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery,"select network_name from network_info where network_info_id = ? ");

	insert_key1 = g_strdup_printf("%d", network_id);
	g_hash_table_insert(in_param, "1", g_strdup(insert_key1));
	tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 1);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if(value){
			g_hash_table_iter_init(&iter2, (GHashTable *) value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					network_name = g_strdup(value2);
				}
			}
			break;
		}
	}

	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);
	g_free(insert_key1);

	return network_name;
}

static int __ps_context_load_profile_id_from_database(void)
{
	char szQuery[5000];
	int profile_id = 0;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *out_param;

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	memset(szQuery, 0, sizeof(szQuery));
	strcpy(szQuery,"select max(profile_id) as last_profile from pdp_profile");

	tcore_storage_read_query_database(strg_db, handle, szQuery, NULL, out_param, 1);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if(value){
			g_hash_table_iter_init(&iter2, (GHashTable *) value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					if(!value2 || g_strcmp0((const char*) value2, "") == 0){
						profile_id = 0;
					}
					else{
						profile_id = atoi((const char*) value2);
					}
				}
			}
			break;
		}
	}

	g_hash_table_destroy(out_param);
	return profile_id;
}

static gboolean __ps_context_insert_profile_tuple(dictionary *dic, int index)
{
	gboolean rv = FALSE;
	GHashTable *in_param;

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	{//profile id
		gchar *profile_id;
		gchar* item_key = NULL;
		item_key = g_strdup_printf("connection:profile_id_%d", index);
		profile_id = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "1", g_strdup(profile_id));
		g_free(item_key);
	}

	{//profile name
		gchar *profile_name;
		gchar* item_key = NULL;
		item_key = g_strdup_printf("connection:profile_name_%d", index);
		profile_name = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "2", g_strdup(profile_name));
		g_free(item_key);
	}

	{//apn
		gchar *apn;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:apn_%d", index);
		apn = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "3", g_strdup(apn));
		g_free(item_key);
	}

	{//auth type
		gchar *auth_type;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:auth_type_%d", index);
		auth_type = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "4", g_strdup(auth_type));
		g_free(item_key);
	}

	{//auth id
		gchar *auth_id;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:auth_id_%d", index);
		auth_id = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "5", g_strdup(auth_id));
		g_free(item_key);
	}

	{//auth pwd
		gchar *auth_pwd;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:auth_pwd_%d", index);
		auth_pwd = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "6", g_strdup(auth_pwd));
		g_free(item_key);
	}

	{//pdp protocol
		gchar *pdp_protocol;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:pdp_protocol_%d", index);
		pdp_protocol = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "7", g_strdup(pdp_protocol));
		g_free(item_key);
	}

	{// proxy ip
		gchar *proxy_ip_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:proxy_ip_addr_%d", index);
		proxy_ip_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "8", g_strdup(proxy_ip_addr));
		g_free(section_key);
	}

	{//home url
		gchar *home_url;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:home_url_%d", index);
		home_url = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "9", g_strdup(home_url));
		g_free(section_key);
	}

	{//linger time
		gchar *linger_time;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:linger_time_%d", index);
		linger_time = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "10", g_strdup(linger_time));
		g_free(section_key);
	}

	{//traffic class
		gchar *traffic_class;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:traffic_class_%d", index);
		traffic_class = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "11", g_strdup(traffic_class));
		g_free(section_key);
	}

	{//is static ip address
		gchar *is_static_ip_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:is_static_ip_addr_%d", index);
		is_static_ip_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "12", g_strdup(is_static_ip_addr));
		g_free(section_key);
	}

	{//ip address if static ip is true
		gchar *ip_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:ip_addr_%d", index);
		ip_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "13", g_strdup(ip_addr));
		g_free(section_key);
	}

	{//is static dns address
		gchar *is_static_dns_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:is_static_dns_addr_%d", index);
		is_static_dns_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "14", g_strdup(is_static_dns_addr));
		g_free(section_key);
	}

	{//dns address 1
		gchar *dns_addr1;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:dns_addr1_%d", index);
		dns_addr1 = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "15", g_strdup(dns_addr1));
		g_free(section_key);
	}

	{//dns address 2
		gchar *dns_addr2;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:dns_addr2_%d", index);
		dns_addr2 = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "16", g_strdup(dns_addr2));
		g_free(section_key);
	}

	{//network info id
		gchar *network_info_id;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:network_info_id_%d", index);
		network_info_id = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "17", g_strdup(network_info_id));
		g_free(section_key);
	}

	{//service category id
		gchar *svc_category_id;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:svc_category_id_%d", index);
		svc_category_id = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "18", g_strdup(svc_category_id));
		g_free(section_key);
	}

	{//hidden
		gchar *hidden;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:hidden_%d", index);
		hidden = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "19", g_strdup(hidden));
		g_free(section_key);
	}

	{//editable
		gchar *editable;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:editable_%d", index);
		editable = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "20", g_strdup(editable));
		g_free(section_key);
	}

	{//default internet connection
		gchar *default_internet_con;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:default_internet_con_%d", index);
		default_internet_con = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "21", g_strdup(default_internet_con));
		g_free(section_key);
	}

	{//insert data into table
		char szQuery[5000];

		memset(szQuery, 0, 5000);
		strcpy(szQuery," insert into pdp_profile( ");
		strcat(szQuery," profile_id, profile_name, apn, auth_type, auth_id, auth_pwd, ");
		strcat(szQuery," pdp_protocol, proxy_ip_addr, home_url, linger_time,");
		strcat(szQuery," traffic_class, is_static_ip_addr, ip_addr, is_static_dns_addr,dns_addr1, dns_addr2,");
		strcat(szQuery," network_info_id, svc_category_id, hidden, editable, default_internet_con) values( ");
		strcat(szQuery," ?, ?, ?, ?, ?, ?,");//1,2,3,4,5,6(auth_pwd)
		strcat(szQuery," ?, ?, ?, ?,");//7,8,9,10(linger_time)
		strcat(szQuery," ?, ?, ?, ?, ?, ?,");//11,12,13,14,15,16(dns_addr2)
		strcat(szQuery," ?, ?, ?, ?, ?)");//17,18,19,20,21(default_internet_con)

		rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
		dbg("insert into pdp_profile result(%d)", rv);
		g_hash_table_destroy(in_param);
	}

	return rv;
}

static int __ps_context_get_network_id(gchar *mccmnc)
{
	int network_id;

	network_id = __ps_context_load_network_id_from_database(mccmnc);
	dbg("network id(%d)", network_id);
	if(network_id > 0)
		return network_id;

	network_id = __ps_context_insert_network_id_to_database(mccmnc);
	if(network_id <= 0 )
		return -1;

	return network_id;
}

static gboolean __ps_context_get_profile_properties(gpointer object, GHashTable *properties)
{
	gchar *s_authtype = NULL, *s_role = NULL;
	PsContext *context = NULL;

	g_return_val_if_fail(object != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	context = (PsContext *) object;
	dbg("get profile properties");

	s_authtype = g_strdup_printf("%d", tcore_context_get_auth(context->co_context));
	s_role = g_strdup_printf("%d", tcore_context_get_role(context->co_context));

	g_hash_table_insert(properties, "path", g_strdup(context->path));
	g_hash_table_insert(properties, "apn", tcore_context_get_apn(context->co_context));
	g_hash_table_insert(properties, "auth_type", g_strdup(s_authtype));
	g_hash_table_insert(properties, "auth_id", tcore_context_get_username(context->co_context));
	g_hash_table_insert(properties, "auth_pwd", tcore_context_get_password(context->co_context));
	g_hash_table_insert(properties, "proxy_addr", tcore_context_get_proxy(context->co_context));
	g_hash_table_insert(properties, "home_url", tcore_context_get_mmsurl(context->co_context));
	g_hash_table_insert(properties, "svc_ctg_id", g_strdup(s_role));
	g_hash_table_insert(properties, "profile_name", tcore_context_get_profile_name(context->co_context));
	g_hash_table_insert(properties, "hidden", g_strdup(BOOL2STRING(context->hidden)));
	g_hash_table_insert(properties, "editable", g_strdup(BOOL2STRING(context->editable)));
	g_hash_table_insert(properties, "default_internet_conn", g_strdup(BOOL2STRING(context->default_internet)));

	g_free(s_authtype);
	g_free(s_role);

	return TRUE;
}

static gboolean __ps_context_set_default_connection_enable(gpointer object, gboolean enabled)
{
	PsContext *context = object;
	int role = CONTEXT_ROLE_UNKNOWN;
	g_return_val_if_fail(context != NULL, FALSE);

	role = tcore_context_get_role(context->co_context);
	if(role == CONTEXT_ROLE_INTERNET){
		context->default_internet = enabled;
	}

	return TRUE;
}

static gpointer __ps_context_add_context(gpointer modem, gchar *mccmnc, int profile_id)
{
	char szQuery[5000];
	DBusGConnection *conn = NULL;
	TcorePlugin *p = NULL;

	GHashTableIter iter;
	gpointer object = NULL;
	gpointer key, value;
	gchar *insert_key1 = NULL;
	GHashTable *in_param, *out_param;
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	dbg("create profile by profile id (%d)", profile_id);
	conn = _ps_modem_ref_dbusconn(modem);
	p = _ps_modem_ref_plugin(modem);

	memset(szQuery, '\0', 5000);
	strcpy(szQuery, "select");
	strcat(szQuery, " a.network_info_id, a.network_name, a.mccmnc,"); //0 , 1, 2
	strcat(szQuery, " b.profile_id, b.profile_name, b.apn, "); //3, 4, 5
	strcat(szQuery, " b.auth_type, b.auth_id, b.auth_pwd,"); //6, 7, 8
	strcat(szQuery, " b.proxy_ip_addr, b.home_url, b.pdp_protocol, "); //9, 10 , 11
	strcat(szQuery, " b.linger_time, b.traffic_class, b.is_static_ip_addr, b.ip_addr,"); //12, 13, 14, 15
	strcat(szQuery, " b.is_static_dns_addr, b.dns_addr1, b.dns_addr2, b.svc_category_id, b.hidden, b.editable, b.default_internet_con"); //16, 17, 18, 19, 20, 21, 22
	strcat(szQuery, " from network_info a, pdp_profile b");
	strcat(szQuery, " where b.profile_id = ? and a.network_info_id = b.network_info_id ");

	insert_key1 = g_strdup_printf("%d", profile_id);
	g_hash_table_insert(in_param, "1", g_strdup(insert_key1));
	tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 23);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *path = NULL;

		object = __ps_context_create_context(conn, p, mccmnc, (GHashTable *) value);
		path = _ps_context_ref_path(object);

		g_hash_table_insert(contexts, g_strdup(path), object);
		dbg("context (%p, %s) insert to hash", object, path);
	}

	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);
	g_free(insert_key1);

	return object;
}

gboolean _ps_context_initialize(gpointer plugin)
{
	gboolean rv = TRUE;

	dbg("global variable initialized");
	rv &=__ps_context_create_storage_handle(plugin);
	rv &=__ps_context_create_context_hash();

	return rv;
}

gboolean _ps_context_reset_profile_table(void)
{
	gboolean rv = FALSE;
	GHashTable *in_param;
	char szQuery[5000];

	memset(szQuery, '\0', 5000);
	strcat(szQuery, " delete from pdp_profile");

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	rv = tcore_storage_remove_query_database(strg_db, handle, szQuery, in_param);

	g_hash_table_destroy(in_param);
	return rv;
}

gboolean _ps_context_fill_profile_table_from_ini_file(void)
{
	int index = 1;
	int data_exist = 0;

	dictionary *dic = NULL;
	dic = iniparser_load("/opt/system/csc-default/data/csc-default-data-connection.ini");

	if(dic == NULL){
		dbg("fail to load the csc default file");
		return FALSE;
	}

	do{
		gchar *section_key = NULL;

		section_key = g_strdup_printf("connection:profile_id_%d", index);
		dbg("section key (%s)", section_key);
		data_exist = iniparser_find_entry(dic, section_key);
		if(!data_exist){
			g_free(section_key);
			iniparser_freedict(dic);
			dbg("no more data in ini");
			return TRUE;
		}

		__ps_context_insert_profile_tuple(dic, index);

		g_free(section_key);
		index++;

	}while(data_exist);

	return TRUE;
}

gboolean _ps_context_reset_hashtable(void)
{
	if(!contexts)
		return TRUE;

	g_hash_table_foreach_remove(contexts, __remove_contexts, NULL);
	return TRUE;
}

GHashTable* _ps_context_create_hashtable(DBusGConnection *conn, TcorePlugin *p, gchar *mccmnc)
{
	char szQuery[5000];
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *in_param, *out_param;
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	dbg("Create profile by mccmnc: [%s]", mccmnc);

	memset(szQuery, '\0', 5000);
	strcpy(szQuery, "select");
	strcat(szQuery, " a.network_info_id, a.network_name, a.mccmnc,"); //0 , 1, 2
	strcat(szQuery, " b.profile_id, b.profile_name, b.apn, "); //3, 4, 5
	strcat(szQuery, " b.auth_type, b.auth_id, b.auth_pwd,"); //6, 7, 8
	strcat(szQuery, " b.proxy_ip_addr, b.home_url, b.pdp_protocol, "); //9, 10 , 11
	strcat(szQuery, " b.linger_time, b.traffic_class, b.is_static_ip_addr, b.ip_addr,"); //12, 13, 14, 15
	strcat(szQuery, " b.is_static_dns_addr, b.dns_addr1, b.dns_addr2, b.svc_category_id, b.hidden, b.editable, b.default_internet_con"); //16,17, 18, 19, 20, 21, 22
	strcat(szQuery, " from network_info a, pdp_profile b");
	strcat(szQuery, " where a.mccmnc= ? and a.network_info_id = b.network_info_id ");

	g_hash_table_insert(in_param, "1", g_strdup(mccmnc));
	dbg("Inserted mccmnc to Hash Table");

	tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 23);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *path = NULL;
		gpointer object = NULL;

		object = __ps_context_create_context(conn, p, mccmnc, (GHashTable *) value);
		path = _ps_context_ref_path(object);

		g_hash_table_insert(contexts, g_strdup(path), object);
		dbg("Inserted to Hash Table - context: [%p] Path: [%s]", object, path);
	}

	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	return contexts;
}

GHashTable* _ps_context_ref_hashtable(void)
{
	g_return_val_if_fail(contexts != NULL, NULL);
	return contexts;
}

gboolean _ps_context_add_context(gpointer modem, gchar *operator, GHashTable *property)
{
	GHashTable *services = NULL;
	gpointer context = NULL;

	GHashTableIter iter;
	gpointer key, value;
	int network_id = 0;
	int profile_id = 0;

	network_id = __ps_context_get_network_id(operator);
	if(network_id <= 0){
		dbg("fail to add network info");
		return FALSE;
	}

	profile_id = __ps_context_insert_profile_to_database(property, network_id);
	if(profile_id <= 0){
		dbg("fail to insert profile info to database");
		return FALSE;
	}

	context = __ps_context_add_context(modem, operator, profile_id);
	if(!context)
		return FALSE;

	services = _ps_modem_ref_services(modem);
	if(!services)
		return FALSE;

	g_hash_table_iter_init(&iter, services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		_ps_service_ref_context(value, context);
	}

	return TRUE;
}

gboolean _ps_context_get_properties(gpointer object, GHashTable *properties)
{
	int context_state = 0;
	gboolean active = FALSE;
	PsContext *context = object;

	dbg("get context properties");
	g_return_val_if_fail(context != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	context_state =    tcore_context_get_state(context->co_context);
	if (context_state == CONTEXT_STATE_ACTIVATED)
		active = TRUE;

	g_hash_table_insert(properties, "path", g_strdup(context->path));
	g_hash_table_insert(properties, "active", g_strdup(BOOL2STRING(active)));
	g_hash_table_insert(properties, "ipv4_address", tcore_context_get_ipv4_addr(context->co_context));
	g_hash_table_insert(properties, "ipv4_gateway", tcore_context_get_ipv4_gw(context->co_context));
	g_hash_table_insert(properties, "ipv4_dns1", tcore_context_get_ipv4_dns1(context->co_context));
	g_hash_table_insert(properties, "ipv4_dns2", tcore_context_get_ipv4_dns2(context->co_context));
	g_hash_table_insert(properties, "ipv6_address", "::" );
	g_hash_table_insert(properties, "ipv6_gateway", "::" );
	g_hash_table_insert(properties, "ipv6_dns1", "::" );
	g_hash_table_insert(properties, "ipv6_dns2", "::" );
	g_hash_table_insert(properties, "proxy", tcore_context_get_proxy(context->co_context));
	g_hash_table_insert(properties, "dev_name", tcore_context_get_ipv4_devname(context->co_context));

	return TRUE;
}

gboolean _ps_context_set_alwayson_enable(gpointer object, gboolean enabled)
{
	PsContext *context = object;
	int role = CONTEXT_ROLE_UNKNOWN;

	dbg("Set Always ON: [%s]", (enabled ? "YES" : "NO"));

	g_return_val_if_fail(context != NULL, FALSE);

	role = tcore_context_get_role(context->co_context);
	dbg("Role: [%d] Default Internet: [%s]",
		role, (context->default_internet ? "YES" : "NO"));
	if(role == CONTEXT_ROLE_INTERNET && context->default_internet) {
		dbg("Setting Always ON: [%s]", (enabled ? "YES" : "NO"));
		context->alwayson = enabled;
	}

	return TRUE;
}

gboolean _ps_context_get_default_internet(gpointer object)
{
	PsContext *context = object;
	int role = CONTEXT_ROLE_UNKNOWN;
	g_return_val_if_fail(context != NULL, FALSE);

	role = tcore_context_get_role(context->co_context);
	if(role == CONTEXT_ROLE_INTERNET && context->default_internet){
		return TRUE;
	}

	return FALSE;
}

gboolean _ps_context_set_service(gpointer object, gpointer service)
{
	PsContext *context = object;

	dbg("Setting Service: [0x%x]", service);

	g_return_val_if_fail(context != NULL, FALSE);

	context->p_service = service;
	return TRUE;
}

gpointer _ps_context_ref_service(gpointer object)
{
	PsContext *context = object;
	g_return_val_if_fail(context != NULL, FALSE);

	return context->p_service;
}

gchar* _ps_context_ref_path(gpointer object)
{
	PsContext *context = object;
	g_return_val_if_fail(context != NULL, NULL);

	return context->path;
}

gboolean _ps_context_get_alwayson_enable(gpointer object)
{
	PsContext *context = object;
	g_return_val_if_fail(context != NULL, FALSE);

	return context->alwayson;
}

gpointer _ps_context_ref_co_context(gpointer object)
{
	PsContext *context = object;
	g_return_val_if_fail(context != NULL, NULL);

	return context->co_context;
}

gboolean _ps_context_set_connected(gpointer object, gboolean enabled)
{
	gchar *ipv4 = NULL;
	PsContext *context = object;
	g_return_val_if_fail(context != NULL, FALSE);

	dbg("Set Service State: [%s]",
			(enabled ? "CONNECTED" : "NOT CONNECTED"));

	/* Get IPv4 Address */
	ipv4 = tcore_context_get_ipv4_addr(context->co_context);
	dbg("IPv4 Address: [%s]", ipv4);

	if (enabled) {
		dbg("Set state - ACTIVATED");
		tcore_context_set_state(context->co_context, CONTEXT_STATE_ACTIVATED);

		/* If IP address is 0.0.0.0, deactivate the context */
		if( g_str_equal(ipv4, "0.0.0.0") == TRUE ){
			dbg("IP Address: [0.0.0.0] - Deactivate context!!!");
			_ps_service_deactivate_context(context->p_service, context);
			return TRUE;
		}

		/* Reset connection timer */
		_ps_service_reset_connection_timer(context);
	}
	else {
		dbg("Set state - DEACTIVATED");
		tcore_context_set_state(context->co_context, CONTEXT_STATE_DEACTIVATED);

		/* Reset device information */
		tcore_context_reset_devinfo(context->co_context);

		/* Reset connection timer */
		_ps_service_connection_timer(context->p_service, context);
	}

	/* Emit Property changed signal */
	__ps_context_emit_property_changed_signal(context);

	/* Free memory */
	g_free(ipv4);

	return TRUE;
}

gboolean _ps_context_set_ps_defined(gpointer *object, gboolean value, int cid)
{
	PsContext *context = (PsContext *)object;

	g_return_val_if_fail(context != NULL, FALSE);

	if(tcore_context_get_id(context->co_context) == (unsigned int)cid){
		context->ps_defined = value;
		dbg("Context: [0x%x] Context define: [%s]",
				context, (context->ps_defined ? "YES" : "NO"));
		return TRUE;
	}

	dbg("Context ID [%d] not found in Context: [0x%x]", cid, context);
	return FALSE;
}

gboolean _ps_context_get_ps_defined(gpointer *object)
{
	PsContext *context = (PsContext *)object;

	dbg("context(%p), ps_defined(%d)", context, context->ps_defined);

	return context->ps_defined;
}
