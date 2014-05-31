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

#include <unistd.h>

#include "ps-master.h"

#include "ps.h"
#include "ps-error.h"

#include <server.h>
#include <plugin.h>
#include <storage.h>
#include <hal.h>

#define PS_MASTER_PATH	"/"
#define PROP_DEFAULT		FALSE
#define PROP_DEFAULT_STR	NULL
#define BOOL2STRING(a)		((a==TRUE) ? ("TRUE"):("FALSE"))

/*Properties*/

enum {
	PROP_MASTER_O,
	PROP_MASTER_PLUGIN,
	PROP_MASTER_CONN,
	PROP_MASTER_PATH
};

enum {
	 SIG_MASTER_MODEM_ADDED,
	 SIG_MASTER_MODEM_REMOVED,
	 SIG_MASTER_LAST
};

static guint32 signals[SIG_MASTER_LAST] = {0,};

struct PsMasterClass {
	GObjectClass parent;

	void (*modem_added)(PsMaster *master, gchar *modem_path);
	void (*modem_removed)(PsMaster *master, gchar *modem_path);
};

struct PsMaster {
	GObject parent;

	//member variable
	gchar *path;
	TcorePlugin *plg;
	DBusGConnection *conn;
	GHashTable *modems;
};

G_DEFINE_TYPE(PsMaster, ps_master, G_TYPE_OBJECT);

/*Function Declaration*/
static void __ps_master_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void __ps_master_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);

gboolean ps_iface_master_get_modems(PsMaster *master, DBusGMethodInvocation *context);
gboolean ps_iface_master_get_profile_list(PsMaster *master, DBusGMethodInvocation *context);
gboolean ps_iface_master_add_profile(PsMaster *master, GHashTable *profile_property, gboolean *result, GError **error);
gboolean ps_iface_master_reset_profile(PsMaster *master, gboolean *result, GError **error);

static void __ps_master_emit_modem_added_signal(PsMaster *master, gpointer modem);
/*static void __ps_master_emit_modem_removed_signal(PsMaster *master, gpointer modem);*/

static void __remove_modem(gpointer data);
static void __ps_master_register_key_callback(gpointer master, enum tcore_storage_key key);
static void __ps_master_storage_key_callback(enum tcore_storage_key key, void *value, void *user_data);

#include "ps-iface-master-glue.h"

static void ps_master_init(PsMaster *master)
{
	dbg("ps master init");
	master->plg = NULL;
	master->conn = NULL;
	master->path = PROP_DEFAULT_STR;
	master->modems = g_hash_table_new_full(g_str_hash,g_str_equal, g_free, __remove_modem);
	return;
}

static void ps_master_class_init(PsMasterClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	dbg("class_init");

	//set property
	object_class->get_property = __ps_master_get_property;
	object_class->set_property = __ps_master_set_property;

	//register class to dbus
	dbus_g_object_type_install_info(PS_TYPE_MASTER, &dbus_glib_ps_iface_master_object_info);

	//add properties
	g_object_class_install_property(
			object_class,
			PROP_MASTER_PLUGIN,
			g_param_spec_pointer("plg", "PLUGIN", "Plug in Object",
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_MASTER_CONN,
			g_param_spec_boxed("conn", "CONNECTION", "DBus connection", DBUS_TYPE_G_CONNECTION,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property(
			object_class,
			PROP_MASTER_PATH,
			g_param_spec_string("path", "Path", "Object path", NULL,
					G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	//add signal handler
	signals[SIG_MASTER_MODEM_ADDED] = g_signal_new("modem-added", G_OBJECT_CLASS_TYPE(klass),
			G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(PsMasterClass, modem_added), NULL, NULL,
			g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, DBUS_TYPE_G_STRING_STRING_HASHTABLE);

	signals[SIG_MASTER_MODEM_REMOVED] = g_signal_new("modem-removed", G_OBJECT_CLASS_TYPE(klass),
			G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(PsMasterClass, modem_removed), NULL, NULL,
			g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, DBUS_TYPE_G_OBJECT_PATH);

	return;
}

static void __ps_master_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	return;
}

static void __ps_master_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	PsMaster *master = PS_MASTER(object);

	switch (prop_id) {
		case PROP_MASTER_PLUGIN: {
			master->plg = g_value_get_pointer(value);
			msg("	master(%p) set plg(%p)", master, master->plg);
		}
			break;
		case PROP_MASTER_CONN: {
			master->conn = g_value_get_boxed(value);
			msg("	master(%p) set conn(%p)", master, master->conn);
		}
			break;
		case PROP_MASTER_PATH: {
			if (master->path) g_free(master->path);
			master->path = g_value_dup_string(value);
			msg("	master(%p) set path(%s)", master, master->path);
		}
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	} //swtich end

	return;
}

gboolean ps_iface_master_get_modems(PsMaster *master, DBusGMethodInvocation *context)
{
	GError *error = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GHashTable *modems;

	dbg("master get modems interface");

	if (master->modems == NULL) {
		g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "master(%p) does not have modems", master);
		dbus_g_method_return_error(context, error);
		return FALSE;
	}

	modems = g_hash_table_new_full(g_direct_hash, g_str_equal, g_free,
			(GDestroyNotify) g_hash_table_destroy);

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *path = NULL;
		GHashTable *properties = NULL;
		gboolean rv = FALSE;

		properties = g_hash_table_new(g_str_hash, g_str_equal);
		rv = _ps_modem_get_properties(value, properties);
		if (rv != TRUE) {
			g_set_error(&error, PS_ERROR, PS_ERR_INTERNAL, "fail to get properties modem(%p)",
					value);
			dbus_g_method_return_error(context, error);
			g_hash_table_destroy(properties);
			g_hash_table_destroy(modems);
			return TRUE;
		}

		path = _ps_modem_ref_path(value);
		g_hash_table_insert(modems, g_strdup(path), properties);
		dbg("modem (%p) inserted into hash", value);
	}

	dbus_g_method_return(context, modems);
	g_hash_table_destroy(modems);

	return TRUE;
}

gboolean ps_iface_master_get_profile_list(PsMaster *master, DBusGMethodInvocation *context)
{
	int index = 0;
	GError *error = NULL;
	GHashTableIter iter;
	gpointer key, value;

	guint len =0;
	gchar **strv = NULL;
	GHashTable *contexts = NULL;
	GSList *profiles = NULL;

	contexts = _ps_context_ref_hashtable();
	if (contexts == NULL) {
		err("no profiles");
		g_set_error(&error, PS_ERROR, PS_ERR_NO_PROFILE, "profile does not exists");
		dbus_g_method_return_error(context, error);
		return TRUE;
	}

	g_hash_table_iter_init(&iter, contexts);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		gchar *s_path = NULL;

		s_path = _ps_context_ref_path(value);
		dbg("key(%s), value(%p), path(%s)", (gchar *)key, value, s_path);
		if(s_path)
			profiles = g_slist_append(profiles, g_strdup((const gchar*)s_path));
	}

	if (profiles == NULL) {
		err("no profiles");
		g_set_error(&error, PS_ERROR, PS_ERR_NO_PROFILE, "profile does not exists");
		dbus_g_method_return_error(context, error);
		return TRUE;
	}

	len = g_slist_length(profiles);
	strv = g_new(gchar *, len+1);

	do{
		strv[index] = g_strdup(profiles->data);
		index++;
	}while(  (profiles = g_slist_next(profiles)) );
	strv[index] = NULL;

	dbus_g_method_return(context, strv);
	g_strfreev(strv);
	profiles = g_slist_nth(profiles, 0);
	g_slist_free_full(profiles, g_free);
	return TRUE;
}

gboolean ps_iface_master_add_profile(PsMaster *master, GHashTable *profile_property,
		gboolean *result, GError **error)
{
	GHashTableIter iter;
	gpointer key, value;
	gboolean rv = FALSE;
	gchar *operator = NULL;

	dbg("add profile request");

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		operator = _ps_modem_ref_operator(value);
		if(operator)
			break;
	}

	if(!operator){
		dbg("there is no active modem");
		g_set_error(error, PS_ERROR, PS_ERR_INTERNAL,"fail to add profile");
		*result = FALSE;
		return TRUE;
	}

	rv = _ps_context_add_context(value, operator, profile_property);
	if(rv != TRUE){
		g_set_error(error, PS_ERROR, PS_ERR_INTERNAL,"fail to add profile");
		*result = FALSE;
		return TRUE;
	}

	dbg("success to add profile");
	*result = TRUE;

	return TRUE;
}

gboolean ps_iface_master_reset_profile(PsMaster *master, gboolean *result, GError **error)
{
	GHashTableIter iter;
	gpointer key, value;
	gboolean rv = FALSE;
	int b_check = 0;

	*result = TRUE;

	dbg("reset profile request");

	if (master->modems == NULL) {
		dbg("modem does not exist");
		g_set_error(error, PS_ERROR, PS_ERR_INTERNAL, "fail to get modem");
		*result = FALSE;
		return TRUE;
	}

	b_check = access("/opt/system/csc-default/data/csc-default-data-connection.ini", F_OK);
	if( b_check != 0 ){
		dbg("csc file was not there");
		g_set_error(error, PS_ERROR, PS_ERR_INTERNAL, "no csc data file");
		*result = FALSE;
		return TRUE;
	}

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		dbg("key(%s), value(%p) context", key, value);
		_ps_modem_processing_power_enable(value, FALSE);
		_ps_modem_set_sim_enabled(value, FALSE);
	}

	_ps_context_reset_hashtable();
	_ps_context_reset_profile_table();
	rv = _ps_context_fill_profile_table_from_ini_file();

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		_ps_get_co_modem_values(value);
	}

	if(!rv){
		dbg("csc data was wrong");
		g_set_error(error, PS_ERROR, PS_ERR_INTERNAL, "fail to load csc data");
		*result = FALSE;
	}

	return TRUE;
}

static void __ps_master_emit_modem_added_signal(PsMaster *master, gpointer modem)
{
	GHashTable *properties = NULL;

	properties = g_hash_table_new(g_str_hash, g_str_equal);
	_ps_modem_get_properties(modem, properties);
	g_signal_emit(master, signals[SIG_MASTER_MODEM_ADDED], 0, properties);
	dbg("master (%p) emit the modem(%p) added signal", master, modem);
	g_hash_table_destroy(properties);
	return;
}

/*static void __ps_master_emit_modem_removed_signal(PsMaster *master, gpointer modem)
{
	g_signal_emit(master, signals[SIG_MASTER_MODEM_REMOVED], 0, _ps_modem_ref_path(modem));
	dbg("master (%p) emit the modem(%p) removed signal", master, modem);
	return;
}*/

static void __remove_modem(gpointer data)
{
	dbg("remove modem (%p)", data);
	return;
}

static void __ps_master_register_key_callback(gpointer object, enum tcore_storage_key key)
{
	gpointer handle = NULL;
	PsMaster *master = (PsMaster *) object;
	Server *s = tcore_plugin_ref_server(master->plg);
	static Storage *strg;

	strg = tcore_server_find_storage(s, "vconf");
	handle = tcore_storage_create_handle(strg, "vconf");
	if (!handle)
		err("fail to create vconf handle");

	tcore_storage_set_key_callback(strg, key, __ps_master_storage_key_callback, object);

	return;
}

static void __ps_master_storage_key_callback(enum tcore_storage_key key, void *value, void *user_data)
{
	GVariant *tmp = NULL;
	GHashTableIter iter;
	gpointer h_key, h_value;
	gboolean type_check = FALSE;
	PsMaster *master = (PsMaster *)user_data;

	dbg("storage key(%d) callback", key);
	g_return_if_fail(master != NULL);

	tmp = (GVariant *)value;
	if(!tmp){
		err("value is null");
		return;
	}

	type_check = g_variant_is_of_type(tmp, G_VARIANT_TYPE_BOOLEAN);
	if(!type_check){
		err("wrong variant data type");
		g_variant_unref(tmp);
		return;
	}

	g_hash_table_iter_init(&iter, master->modems);
	while (g_hash_table_iter_next(&iter, &h_key, &h_value) == TRUE) {
		if(key == KEY_3G_ENABLE){
			gboolean data_allowed = g_variant_get_boolean(tmp);
			_ps_modem_set_data_allowed(h_value, data_allowed);
		}
		else if(key == KEY_DATA_ROAMING_SETTING){
			gboolean roaming_allowed = g_variant_get_boolean(tmp);
			_ps_modem_set_data_roaming_allowed(h_value, roaming_allowed);
		}
	}

	g_variant_unref(tmp);
	return;
}

gpointer _ps_master_create_master(DBusGConnection *conn, TcorePlugin *p)
{
	guint rv;
	GObject *object;
	DBusGProxy *proxy;
	GError *error = NULL;

	dbg("master object create");
	g_return_val_if_fail(conn != NULL, NULL);

	proxy = dbus_g_proxy_new_for_name(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus");

	if (!dbus_g_proxy_call(proxy, "RequestName", &error, G_TYPE_STRING, PS_DBUS_SERVICE,
			G_TYPE_UINT, 0, G_TYPE_INVALID, G_TYPE_UINT, &rv, G_TYPE_INVALID)) {
		err("Failed to acquire service(%s) error(%s)", PS_DBUS_SERVICE, error->message);
		return NULL;
	}

	object = g_object_new(PS_TYPE_MASTER, "plg", p, "conn", conn, "path", PS_MASTER_PATH, NULL);
	dbus_g_connection_register_g_object(conn, PS_MASTER_PATH, object);
	msg("	master(%p) register dbus path(%s)", object, PS_MASTER_PATH);

	__ps_master_register_key_callback(object, KEY_3G_ENABLE);
	__ps_master_register_key_callback(object, KEY_DATA_ROAMING_SETTING);

	return object;
}

gboolean _ps_master_create_modems(gpointer object)
{
	Server *s = NULL;
	GSList *plist = NULL;
	gpointer modem = NULL, tmp = NULL;
	PsMaster *master = NULL;
	gboolean ret = FALSE;

	dbg("create modem objects");
	g_return_val_if_fail(object != NULL, FALSE);

	master = (PsMaster *) object;
	s = tcore_plugin_ref_server(master->plg);
	plist = tcore_server_ref_plugins(s);

	if (NULL == plist) {
		dbg("fail to get plugin-in list.");
		return FALSE;
	}

	for (; plist != NULL; plist = g_slist_next(plist)) {
		TcorePlugin *p = NULL;
		CoreObject *co_modem = NULL;
		gchar *modem_name = NULL;

		p = plist->data;

		/* AT Standard Plug-in is not considered */
		if ((p == NULL)
				|| (strcmp(tcore_plugin_ref_plugin_name(p), "AT") == 0))
			continue;

		co_modem = tcore_plugin_ref_core_object(p, CORE_OBJECT_TYPE_MODEM);
		if (!co_modem)
			continue;

		modem_name = g_strdup_printf("/%s", tcore_server_get_cp_name_by_plugin(p));
		tmp = g_hash_table_lookup(master->modems, modem_name);
		if (tmp != NULL) {
			dbg("modem (%p) already existed", tmp);
			continue;
		}

		modem = _ps_modem_create_modem(master->conn, master->plg, master, modem_name, co_modem);
		if (modem == NULL) {
			dbg("fail to create modem");
			return FALSE;
		}

		g_hash_table_insert(master->modems, g_strdup(modem_name), modem);
		dbg("modem (%p) created", modem);

		__ps_master_emit_modem_added_signal(master, modem);

		g_free(modem_name);

		ret = TRUE;
	}

	return ret;
}

gboolean _ps_master_get_storage_value(gpointer object, enum tcore_storage_key key)
{
	Server *s = NULL;
	Storage *strg = NULL;
	PsMaster *master = object;

	g_return_val_if_fail(master != NULL, FALSE);
	s = tcore_plugin_ref_server(master->plg);
	strg = tcore_server_find_storage(s, "vconf");

	return tcore_storage_get_bool(strg, key);
}

gboolean _ps_master_set_storage_value(gpointer object, enum tcore_storage_key key, gboolean value)
{
	Server *s = NULL;
	Storage *strg = NULL;
	PsMaster *master = object;

	g_return_val_if_fail(master != NULL, FALSE);
	s = tcore_plugin_ref_server(master->plg);
	strg = tcore_server_find_storage(s, "vconf");

	return tcore_storage_set_bool(strg, key, value);
}
