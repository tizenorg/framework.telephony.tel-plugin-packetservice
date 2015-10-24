/*
 * tel-plugin-packetservice
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: DongHoo Park <donghoo.park@samsung.com>
 *		Arun Shukla <arun.shukla@samsung.com>
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

#include <tcore.h>
#include <plugin.h>
#include <server.h>
#include <storage.h>
#include <core_object.h>
#include <co_ps.h>
#include <co_context.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <iniparser.h>

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#define PROP_DEFAULT		FALSE
#define PROP_DEFAULT_STR	NULL
#define DATABASE_PATH_0		"/opt/dbspace/.dnet.db"
#define DATABASE_PATH_1		"/opt/dbspace/.dnet2.db"
#define DELAY_TO_SIGNAL_EMIT 1

typedef struct {
	const char *mccmnc;
	const char *apn;
	enum co_context_type pdp_type;
	enum co_context_role role;
	const char *p_cscf_ipv4addr;
	const char *p_cscf_ipv6addr;
} OperatorTable;

#define FREE_AND_ASSIGN(ptr, value) do { \
	if (ptr) \
		g_free(ptr);\
	ptr = g_strdup(value); \
} while (0)

static Storage *strg_db;

/*FIXME*/
OperatorTable attach_apn_preference[] = {
	{"45005", "", CONTEXT_TYPE_IPV4V6, CONTEXT_ROLE_IMS, "220.103.220.10", "2001:2d8:00e0:0220::10"},
	{"45006", "", CONTEXT_TYPE_IPV4V6, CONTEXT_ROLE_IMS, "", ""},
	{"45008", "", CONTEXT_TYPE_IPV4V6, CONTEXT_ROLE_IMS, "", ""},
};

static void __ps_context_emit_dedicated_bearer_info_signal(ps_context_t *context);
static void __ps_context_emit_property_changed_signal(ps_context_t *context);
static void _ps_context_setup_interface(PacketServiceContext *context, ps_context_t *context_data);

static gboolean __ps_context_create_storage_handle(gpointer plugin);
static gchar *__ps_context_create_path(char *profile_name, int profile_id, int svc_ctg_id, gchar *cp_name);
static gboolean __ps_context_profile_is_attach_apn(CoreObject *co_context, const gchar *mccmnc);
static gboolean __ps_context_create_co_context(gpointer context, GHashTable *property, gchar *cp_name);
static gboolean __ps_context_update_profile(ps_context_t *context, GHashTable *property);
static gboolean __ps_context_update_database(ps_context_t *context);
static gboolean __ps_context_remove_database(ps_context_t *context);
static int __ps_context_insert_network_id_to_database(gchar *mccmnc, gchar *cp_name);
static int __ps_context_load_network_id_from_database(gchar *mccmnc, gchar *cp_name);
static gchar *__ps_context_load_network_name_from_database(int network_id, gchar *cp_name);
static int __ps_context_load_profile_id_from_database(gchar *cp_name);
static int __ps_context_load_num_of_pdn_from_database(gchar *mccmnc, gchar *cp_name);
static gboolean __ps_context_insert_profile_tuple(dictionary *dic, int index, gchar *cp_name);
static int __ps_context_insert_profile_to_database(GHashTable *property, int network_id, gchar *cp_name);
static int __ps_context_get_network_id(gchar *mccmnc, gchar *cp_name);
GVariant *__ps_context_get_profile_properties(gpointer context, GVariantBuilder *properties);
static gboolean __ps_context_set_default_connection_enable(gpointer object, gboolean enabled);

void __remove_context_handler(gpointer data)
{
	ps_context_t *context = data;

	dbg("Entered");

	if (!context) {
		dbg("Context is Null");
		return;
	}

	/*Need to UNexport and Unref the master Object */
	g_object_unref(context->if_obj);

	ps_dbg_ex_co(_ps_service_ref_co_network(_ps_context_ref_service(context)), "context removed for the path [%s]", context->path);

	g_free(context->path);
	g_free(context->mccmnc);
	g_free(context);

	dbg("Exiting");
	return;
}

static void __ps_context_emit_dedicated_bearer_info_signal(ps_context_t *context)
{
	GVariant *gv = NULL;
	CoreObject *co_network = NULL, *co_context = NULL;
	struct dedicated_bearer_info bearer_info;
	GVariantBuilder properties;
	guint i = 0;
	char *num_dedicated_bearer, *primary_context_id;
	char *secondary_context_id, *qci, *gbr_dl, *gbr_ul, *max_br_dl, *max_br_ul;

	g_return_if_fail(context != NULL);

	co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));
	co_context = _ps_context_ref_co_context(context);
	tcore_context_get_bearer_info(co_context, &bearer_info);

	if (bearer_info.num_dedicated_bearer == 0) {
		ps_warn_ex_co(co_network, "No dedicated bearer information");
		return;
	}

	g_variant_builder_init(&properties, G_VARIANT_TYPE("a{ss}"));

	primary_context_id = INT2STRING(tcore_context_get_id(co_context));
	g_variant_builder_add(&properties, "{ss}", "primary_context_id", primary_context_id);
	g_free(primary_context_id);

	num_dedicated_bearer = CHAR2STRING(bearer_info.num_dedicated_bearer);
	g_variant_builder_add(&properties, "{ss}", "num_dedicated_bearer", num_dedicated_bearer);
	g_free(num_dedicated_bearer);

	secondary_context_id = INT2STRING(bearer_info.secondary_context_id);
	g_variant_builder_add(&properties, "{ss}", "secondary_context_id", secondary_context_id);
	g_free(secondary_context_id);

	for (i = 0; i < bearer_info.num_dedicated_bearer ; i++) {
		char *buf;
		buf = g_strdup_printf("%s_%d", "qci", i);
		qci = CHAR2STRING(bearer_info.qos[i].qci);
		g_variant_builder_add(&properties, "{ss}", buf, qci);
		g_free(buf);
		g_free(qci);

		buf = g_strdup_printf("%s_%d", "gbr_dl", i);
		gbr_dl = INT2STRING(bearer_info.qos[i].gbr_dl);
		g_variant_builder_add(&properties, "{ss}", buf, gbr_dl);
		g_free(buf);
		g_free(gbr_dl);

		buf = g_strdup_printf("%s_%d", "gbr_ul", i);
		gbr_ul = INT2STRING(bearer_info.qos[i].gbr_ul);
		g_variant_builder_add(&properties, "{ss}", buf, gbr_ul);
		g_free(buf);
		g_free(gbr_ul);

		buf = g_strdup_printf("%s_%d", "max_br_dl", i);
		max_br_dl = INT2STRING(bearer_info.qos[i].max_br_dl);
		g_variant_builder_add(&properties, "{ss}", buf, max_br_dl);
		g_free(buf);
		g_free(max_br_dl);

		buf = g_strdup_printf("%s_%d", "max_br_ul", i);
		max_br_ul = INT2STRING(bearer_info.qos[i].max_br_ul);
		g_variant_builder_add(&properties, "{ss}", buf, max_br_ul);
		g_free(buf);
		g_free(max_br_ul);
	}
	gv = g_variant_builder_end(&properties);

	packet_service_context_emit_dedicated_bearer_info(context->if_obj, gv);
	ps_warn_ex_co(co_network, "context (%p) emit the dedicated bearer infomation signal", context);
}

static void __ps_context_emit_property_changed_signal(ps_context_t *context)
{
	GVariant *gv = NULL;
	GVariantBuilder property;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));

	ps_dbg_ex_co(co_network, "get context Properties");

	gv = _ps_context_get_properties(context, &property);
	packet_service_context_emit_property_changed(context->if_obj, gv);

	ps_dbg_ex_co(co_network, "context (%p) emit the context property changed signal", context);
	return;
}

/*	Funtion : _ps_context_remove_context
 *	Description : removes and unregister the interface for the context
 */
gboolean _ps_context_remove_context(gpointer context)
{
	ps_context_t *pscontext = context;
	ps_service_t *service = _ps_context_ref_service(pscontext);

	g_return_val_if_fail(pscontext != NULL, FALSE);
	g_return_val_if_fail(service != NULL, FALSE);
	g_return_val_if_fail(pscontext->path != NULL, FALSE);

	ps_dbg_ex_co(_ps_service_ref_co_network(service),
		"remove context (%s)", pscontext->path);

	/*Removing the context from the static list */
	_ps_service_reset_connection_timer(context);
	_ps_context_set_connected(context, FALSE);

	/* Unexport object */
	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(pscontext->if_obj));

	/* remove context from the list (modem, service) */
	tcore_ps_remove_context(service->co_ps,
		(CoreObject *)_ps_context_ref_co_context(context));
	tcore_context_free(pscontext->co_context);

	/* free allocated resources for context. */
	g_free(pscontext->mccmnc);
	g_free(pscontext->path);
	g_free(pscontext);

	dbg("Exiting");
	return TRUE;
}

static gboolean __ps_context_create_storage_handle(gpointer plugin)
{
	TcorePlugin *p = plugin;

	strg_db = tcore_server_find_storage(tcore_plugin_ref_server(p), "database");
	dbg("Storage: (%p)", strg_db);

	return TRUE;
}

static gchar *__ps_context_create_path(char *profile_name, int profile_id, int svc_ctg_id, gchar *cp_name)
{
	gchar *path = NULL, *in_path = NULL;
	int str_len = 0, context_index = 0;

	if (!profile_name) {
		dbg("profile_name is null");
		return NULL;
	}

	str_len = strlen(profile_name);
	in_path = g_strdup_printf("/%s%s", cp_name, "/context/");

	for (context_index = 0; context_index < str_len; context_index++) {
		gchar *buf = NULL, *tmp = NULL;
		buf = g_strdup_printf("%02x", profile_name[context_index]);
		tmp = g_strconcat(in_path, buf, NULL);

		g_free(buf);
		g_free(in_path);

		in_path = g_strdup(tmp);
		g_free(tmp);
	}

	dbg("converted name(%s) path(%s)", profile_name, in_path);

	path = g_strdup_printf("%s_%d_%d", in_path, profile_id, svc_ctg_id);
	dbg("path (%s)", path);

	g_free(in_path);
	return path;
}

static gboolean __ps_context_profile_is_attach_apn(CoreObject *co_context, const gchar *mccmnc)
{
	gboolean attach_apn = FALSE, default_conn = FALSE;
	int role = 0;

	if (co_context == NULL)
		goto EXIT;

	default_conn = tcore_context_get_default_profile(co_context);
	role = tcore_context_get_role(co_context);

	/*
	 * TODO: Read CSC Configuration routine.
	 * By default, default Ineternet profile will be used for PS attach.
	 * CSC will define APN for PS attach according to operator requirement.
	 */
	 if (default_conn) {
		int i, count;
		gboolean matched = FALSE;

		count = sizeof(attach_apn_preference) / sizeof(attach_apn_preference[0]);
		for (i = 0; i < count ; i++) {
			if (g_strcmp0(attach_apn_preference[i].mccmnc, mccmnc) == 0) {
				if (attach_apn_preference[i].role == role) {
					dbg("index[%d], attach_apn_preference[%d].role: %d, role: %d, apn: %s",
						i, i, attach_apn_preference[i].role, role, attach_apn_preference[i].apn);
					attach_apn = TRUE;
				}
				matched = TRUE;
				break;
			}
		}
		if (!matched) {
			if (role == CONTEXT_ROLE_INTERNET)
				attach_apn = TRUE;
		 }
	 }
EXIT:
	dbg("role(%d), attach_apn (%d)", role, attach_apn);
	return attach_apn;
}

static gboolean __ps_context_create_co_context(gpointer object, GHashTable *property, gchar *cp_name)
{
	GHashTableIter iter;
	gpointer key, value;
	ps_context_t *context = NULL;
	CoreObject *co_context = NULL;

	gchar *path = NULL;
	int profile_id = 0;
	gchar *profile_name = NULL;
	gchar *apn = NULL;
	gchar *auth_id = NULL, *auth_pwd = NULL, *home_url = NULL, *proxy_addr = NULL;
	int auth_type = 0, svc_ctg_id = 0, pdp_type = 0;
	gboolean hidden = FALSE, editable = FALSE, default_conn = FALSE, user_defined = FALSE, is_roaming_apn = FALSE, profile_enable = TRUE;

	g_hash_table_iter_init(&iter, (GHashTable *) property);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		if (g_str_equal(key, "3") == TRUE) { /*Profile ID*/
			if (value) {
				profile_id = atoi((const char *) value);
				dbg("profile id (%d)", profile_id);
			}
		} else if (g_str_equal(key, "4") == TRUE) {
			g_free(profile_name);
			profile_name = g_strdup((const char *) value);
			dbg("profile name (%s)", profile_name);
		} else if (g_str_equal(key, "5") == TRUE) {
			g_free(apn);
			apn = g_strdup((const char *) value);
			dbg("APN (%s)", apn);
		} else if (g_str_equal(key, "6") == TRUE) {
			if (value) {
				auth_type = atoi((const char *) value);
				dbg("auth type (%d)", auth_type);
			}
		} else if (g_str_equal(key, "7") == TRUE) {
			g_free(auth_id);
			auth_id = g_strdup((const char *) value);
			dbg("auth id (%s)", auth_id);
		} else if (g_str_equal(key, "8") == TRUE) {
			g_free(auth_pwd);
			auth_pwd = g_strdup((const char *) value);
			dbg("auth pwd (%s)", auth_pwd);
		} else if (g_str_equal(key, "9") == TRUE) {
			if (!value || g_strcmp0((const gchar *) value, "") == 0) {
				g_free(proxy_addr);
				proxy_addr = g_strdup((const char *) value);
			} else {
				gboolean b_regex = FALSE;
				b_regex = g_regex_match_simple("[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+:[0-9]*", (const gchar *) value, 0, 0);

				if (b_regex) {
					int port_num = 0;
					gchar **tmp_proxy = NULL;

					tmp_proxy = g_strsplit_set((const gchar *) value, ".:", -1);
					port_num = atoi(tmp_proxy[4]);

					if (port_num <= 0) {
						g_free(proxy_addr);
						proxy_addr = g_strdup_printf("%d.%d.%d.%d",
							atoi(tmp_proxy[0]), atoi(tmp_proxy[1]), atoi(tmp_proxy[2]), atoi(tmp_proxy[3]));
					} else {
						g_free(proxy_addr);
						proxy_addr = g_strdup_printf("%d.%d.%d.%d:%d",
							atoi(tmp_proxy[0]), atoi(tmp_proxy[1]), atoi(tmp_proxy[2]), atoi(tmp_proxy[3]), port_num);
					}
					g_strfreev(tmp_proxy);
				} else {
					g_free(proxy_addr);
					proxy_addr = g_strdup((const char *) value);
				} /* not in regular experssion */
			}

			dbg("proxy addr (%s)", proxy_addr);
		} else if (g_str_equal(key, "10") == TRUE) {
			g_free(home_url);
			home_url = g_strdup((const char *) value);
			dbg("home url (%s)", home_url);
		} else if (g_str_equal(key, "11") == TRUE) {
			if (value) {
				pdp_type = atoi((const char *) value);
				dbg("pdp type (%d)", pdp_type);
			}
		} else if (g_str_equal(key, "19") == TRUE) {
			if (value) {
				svc_ctg_id = atoi((const char *) value);
				dbg("context category type (%d)", svc_ctg_id);
			}
		} else if (g_str_equal(key, "20") == TRUE) {
			if (value) {
				hidden = atoi((const char *) value);
				dbg("hidden profile (%d)", hidden);
			}
		} else if (g_str_equal(key, "21") == TRUE) {
			if (value) {
				editable = atoi((const char *) value);
				dbg("editable profile (%d)", editable);
			}
		} else if (g_str_equal(key, "22") == TRUE) {
			if (value) {
				default_conn = atoi((const char*) value);
				dbg("default connection profile (%d)", default_conn);
			}
		} else if (g_str_equal(key, "23") == TRUE) {
			if (value) {
				user_defined = atoi((const char*) value);
				dbg("user defined profile (%d)", user_defined);
			}
		} else if (g_str_equal(key, "24") == TRUE) {
			if (value) {
				is_roaming_apn = atoi((const char*) value);
				dbg("roaming APN profile (%d)", is_roaming_apn);
			}
		} else if (g_str_equal(key, "25") == TRUE) {
			if (value) {
				profile_enable = atoi((const char*) value);
				dbg("profile enable (%d)", profile_enable);
			}
		}
	}

	path = __ps_context_create_path(profile_name, profile_id, svc_ctg_id, cp_name);

	context = (ps_context_t *)object;
	co_context = tcore_context_new(context->plg, path, NULL);
#ifdef TIZEN_PS_IPV4_ONLY
	dbg("pdp type has been changed (%d)", CONTEXT_TYPE_IP);
	tcore_context_set_type(co_context, CONTEXT_TYPE_IP);
#else
	tcore_context_set_type(co_context, pdp_type);
#endif
	tcore_context_set_state(co_context, CONTEXT_STATE_DEACTIVATED);
	tcore_context_set_role(co_context, svc_ctg_id);
	tcore_context_set_apn(co_context, apn);
	tcore_context_set_auth(co_context, auth_type);
	tcore_context_set_username(co_context, auth_id);
	tcore_context_set_password(co_context, auth_pwd);
	tcore_context_set_proxy(co_context, proxy_addr);
	tcore_context_set_mmsurl(co_context, home_url);
	tcore_context_set_profile_name(co_context, profile_name);
	tcore_context_set_default_profile(co_context, default_conn);
	tcore_context_set_attach_apn(co_context, __ps_context_profile_is_attach_apn(co_context, context->mccmnc));
	tcore_context_set_roaming_apn(co_context, is_roaming_apn);

	context->profile_id = profile_id;
	context->editable = editable;
	context->is_default = default_conn;
	_ps_context_set_profile_enable(context, profile_enable);
	context->path = path;
	context->co_context = co_context;
	switch(svc_ctg_id) {
	case CONTEXT_ROLE_INTERNET:
	case CONTEXT_ROLE_MMS:
	case CONTEXT_ROLE_PREPAID_INTERNET:
	case CONTEXT_ROLE_PREPAID_MMS:
		context->hidden = hidden;
		break;
	default:
		context->hidden = TRUE;
		break;
	}

	g_free(profile_name);
	g_free(apn);
	g_free(auth_id);
	g_free(auth_pwd);
	g_free(home_url);
	g_free(proxy_addr);

	return TRUE;
}

static gpointer __ps_context_create_context(GDBusConnection *conn, TcorePlugin *p,
		gchar *mccmnc, GHashTable *property, gchar *cp_name)
{
	PacketServiceContext *context;
	GError *error = NULL;
	ps_context_t *new_context;
	gchar *path = NULL;

	dbg("Entered");

	/*Initializing the master list for internal referencing*/
	new_context = g_try_malloc0(sizeof(ps_context_t));
	if (NULL == new_context) {
		err("Unable to allocate memory for context");
		goto FAILURE;
	}
	dbg("creating the skeleton object");
	context = packet_service_context_skeleton_new();
	if (NULL == context) {
		g_free(new_context);
		goto FAILURE;
	}

	dbg("Assigning the memory location for the internal data");
	new_context->conn = conn;
	new_context->plg = p;
	new_context->if_obj = context;
	new_context->mccmnc = g_strdup(mccmnc);

	__ps_context_create_co_context(new_context, property , cp_name);
	_ps_context_set_alwayson_enable(new_context, TRUE);
	path = _ps_context_ref_path(new_context);
	_ps_context_setup_interface(context, new_context);

	dbg("registering the interface object");

	dbg("exporting the interface object to the dbus connection");
	/*exporting the interface object to the path mention for master*/
	g_dbus_interface_skeleton_export((G_DBUS_INTERFACE_SKELETON(context)),
			conn,
			path,
			&error);

	g_assert_no_error(error);

	dbg("Successfully new object created for the interface for path [%s]", path);
	return new_context;

FAILURE:
	/*To do : handle failure */
	dbg("Unable to allocate memory for the new object");
	return NULL;
}

static gboolean __ps_context_update_profile(ps_context_t *context, GHashTable *property)
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
		} else if (g_str_equal(key, "profile_name") == TRUE) {
			tcore_context_set_profile_name(co_context, (const char *) value);
		} else if (g_str_equal(key, "auth_type") == TRUE) {
			int i_tmp = 0;
			if (value) {
				i_tmp = atoi((const char *) value);
				tcore_context_set_auth(co_context, i_tmp);
			}
		} else if (g_str_equal(key, "auth_id") == TRUE) {
			tcore_context_set_username(co_context, (const char *) value);
		} else if (g_str_equal(key, "auth_pwd") == TRUE) {
			tcore_context_set_password(co_context, (const char *) value);
		} else if (g_str_equal(key, "pdp_protocol") == TRUE) {
			int i_tmp = 0;
			if (value) {
				i_tmp = atoi((const char *) value);
				tcore_context_set_type(co_context, i_tmp);
			}
		} else if (g_str_equal(key, "proxy_addr") == TRUE) {
			tcore_context_set_proxy(co_context, (const char *) value);
		} else if (g_str_equal(key, "home_url") == TRUE) {
			tcore_context_set_mmsurl(co_context, (const char *) value);
		} else if (g_str_equal(key, "profile_enable") == TRUE) {
			if (value) {
				gboolean tmp = TRUE;
				if (g_str_equal(value, "FALSE") == TRUE)
					tmp = FALSE;
				_ps_context_set_profile_enable(context, tmp);
			}
		}
	}

	return __ps_context_update_database(context);
}

gboolean __ps_context_update_default_internet_to_db(ps_context_t *context, gboolean enabled)
{
	gpointer handle;
	GHashTable *in_param;
	char szQuery[3000];
	gboolean rv = FALSE;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));
	char *cp_name = _ps_modem_ref_cp_name(_ps_service_ref_modem(_ps_context_ref_service(context)));

	g_return_val_if_fail(context != NULL, FALSE);

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		ps_err_ex_co(co_network, "Failed to get Storage handle");
		return FALSE;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1",
			g_strdup_printf("%d", enabled));				/* Profile enabled/disabled */
	g_hash_table_insert(in_param, "2",
			g_strdup_printf("%d", context->profile_id));	/* Profile ID */

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		" update pdp_profile set \
		 default_internet_con = ? \
		 where profile_id = ?");

	rv = tcore_storage_update_query_database(strg_db, handle, szQuery, in_param);
	ps_dbg_ex_co(co_network, "Update Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

static gboolean __ps_context_update_database(ps_context_t *context)
{
	gpointer handle;
	GHashTable *in_param;
	char szQuery[3000];
	gboolean rv = FALSE;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));
	char *cp_name = _ps_modem_ref_cp_name(_ps_service_ref_modem(_ps_context_ref_service(context)));

	g_return_val_if_fail(context != NULL, FALSE);

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		ps_err_ex_co(co_network, "Failed to get Storage handle");
		return rv;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1",
			tcore_context_get_apn(context->co_context));						/* APN */
	g_hash_table_insert(in_param, "2",
			g_strdup_printf("%d", tcore_context_get_auth(context->co_context)));	/* Auth Type */
	g_hash_table_insert(in_param, "3",
			tcore_context_get_username(context->co_context));					/* Username */
	g_hash_table_insert(in_param, "4",
			tcore_context_get_password(context->co_context));					/* Password */
	g_hash_table_insert(in_param, "5",
			tcore_context_get_proxy(context->co_context));						/* Proxy */
	g_hash_table_insert(in_param, "6",
			tcore_context_get_mmsurl(context->co_context));						/* MMS URL */
	g_hash_table_insert(in_param, "7",
			tcore_context_get_profile_name(context->co_context));						/* Profile Name */
	g_hash_table_insert(in_param, "8",
			g_strdup_printf("%d", tcore_context_get_type(context->co_context))); /* PDP protocol */
	g_hash_table_insert(in_param, "9",
			g_strdup_printf("%d", _ps_context_get_profile_enable(context))); /* Profile Enable */
	g_hash_table_insert(in_param, "10",
			g_strdup_printf("%d", context->profile_id));						/* Profile ID */

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		" update pdp_profile set \
		 apn = ?, auth_type = ?, auth_id = ?, auth_pwd = ?, \
		 proxy_ip_addr = ?, home_url = ?, profile_name = ?, \
		 pdp_protocol = ?, profile_enable = ? where profile_id = ?");

	rv = tcore_storage_update_query_database(strg_db, handle, szQuery, in_param);
	ps_dbg_ex_co(co_network, "Update Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

static gboolean __ps_context_remove_database(ps_context_t *context)
{
	gpointer handle;
	GHashTable *in_param;
	char szQuery[1000];
	gboolean rv = FALSE;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));
	char *cp_name = _ps_modem_ref_cp_name(_ps_service_ref_modem(_ps_context_ref_service(context)));

	g_return_val_if_fail(context != NULL, FALSE);

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		ps_err_ex_co(co_network, "Failed to get Storage handle");
		return rv;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1",
			g_strdup_printf("%d", context->profile_id));	/* Profile ID */

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		" delete from pdp_profile where profile_id = ? ");

	rv = tcore_storage_remove_query_database(strg_db, handle, szQuery, in_param);
	ps_dbg_ex_co(co_network, "Remove from Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

static int __ps_context_insert_network_id_to_database(gchar *mccmnc, gchar *cp_name)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;
	int network_id = 0;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return rv;
	}

	/*
	 * Check the maximum Network ID that exists in database,
	 * if NONE exists, then 'Network ID' would be equal to 1
	 * else if there exists a valid maximum entry; 'Network ID' would be incremented value.
	 */
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
				(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select max(network_info_id) as network_id from network_info");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, NULL, out_param, 1);
	dbg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if (value) {
			g_hash_table_iter_init(&iter2, (GHashTable *)value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				dbg("key2(%s) value2(%s)", key2, value2);
				if (g_str_equal(key2, "0") == TRUE) {
					if (!value2 || g_strcmp0((const char *)value2, "") == 0)
						network_id = 0;
					else
						network_id = atoi((const char *)value2);

					/* TODO - Check this logic */
					break;
				}
			}
		}
	}

	/* Free Resources */
	g_hash_table_destroy(out_param);

	/* Increment Network ID */
	network_id++;

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		" insert into network_info(network_info_id, network_name, mccmnc) values(?, ?, ?) ");

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup_printf("%d", network_id));	/* Network ID */
	g_hash_table_insert(in_param, "2", g_strdup_printf("PLMN_%s", mccmnc));
	g_hash_table_insert(in_param, "3", g_strdup(mccmnc));

	rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
	if (rv == FALSE) {
		err("Failed to insert query to Storage");
		network_id = 0;
	}

	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return network_id;
}

static int __ps_context_insert_profile_to_database(GHashTable *property, int network_id, gchar *cp_name)
{
	gpointer handle;
	GHashTable *in_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	int profile_id = 0;
	gchar *profile_name = NULL, *apn = NULL, *auth_type = NULL;
	gchar *auth_id = NULL, *auth_pwd = NULL, *proxy_addr = NULL;
	gchar *home_url = NULL, *svc_id = NULL, *pdp_protocol = NULL;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return 0;
	}

	profile_id = __ps_context_load_profile_id_from_database(cp_name);
	if (profile_id < 0) {
		dbg("Failed to get last Profile ID");
		profile_id = 0;

		goto EXIT;
	}

	/* Increment Profile ID */
	profile_id++;

	g_hash_table_iter_init(&iter, property);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		if (g_str_equal(key, "profile_name") == TRUE) {			/* Profile Name */
			if (value != NULL) {
				FREE_AND_ASSIGN(profile_name, value);
			} else {
				if (profile_name)
					g_free(profile_name);

				profile_name = __ps_context_load_network_name_from_database(network_id, cp_name);
			}
		} else if (g_str_equal(key, "apn") == TRUE) {			/* APN */
			FREE_AND_ASSIGN(apn, value);
		} else if (g_str_equal(key, "auth_type") == TRUE) {	/* Auth Type */
			FREE_AND_ASSIGN(auth_type, value);
		} else if (g_str_equal(key, "auth_id") == TRUE) {		/* Auth ID */
			FREE_AND_ASSIGN(auth_id, value);
		} else if (g_str_equal(key, "auth_pwd") == TRUE) {	/* Auth Password */
			FREE_AND_ASSIGN(auth_pwd, value);
		} else if (g_str_equal(key, "proxy_addr") == TRUE) {	/* Proxy Address */
			FREE_AND_ASSIGN(proxy_addr, value);
		} else if (g_str_equal(key, "home_url") == TRUE) {	/* Home URL */
			FREE_AND_ASSIGN(home_url, value);
		} else if (g_str_equal(key, "svc_ctg_id") == TRUE) {	/* Service ID */
			FREE_AND_ASSIGN(svc_id, value);
		} else if (g_str_equal(key, "pdp_protocol") == TRUE) {	/* PDP protocol */
			FREE_AND_ASSIGN(pdp_protocol, value);
		}
	}

	/* Set default PDP protocol */
	if (pdp_protocol == NULL) {
		dbg("default pdp_protocol = IPv4");
		pdp_protocol = g_strdup_printf("%d", CONTEXT_TYPE_IP);
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	g_hash_table_insert(in_param, "1",
			g_strdup_printf("%d", profile_id));			/* Profile ID */
	g_hash_table_insert(in_param, "2", profile_name);	/* Profile Name */
	g_hash_table_insert(in_param, "3", apn);			/* APN */
	g_hash_table_insert(in_param, "4", auth_type);		/* Auth Type */
	g_hash_table_insert(in_param, "5", auth_id);		/* Auth ID */
	g_hash_table_insert(in_param, "6", auth_pwd);		/* Auth Password */
	g_hash_table_insert(in_param, "7", pdp_protocol);		/* PDP Protocol */
	g_hash_table_insert(in_param, "8", proxy_addr);		/* Proxy Address */
	g_hash_table_insert(in_param, "9", home_url);		/* Home URL */
	g_hash_table_insert(in_param, "10",
			g_strdup_printf("%d", network_id));			/* Network ID */
	g_hash_table_insert(in_param, "11", svc_id);		/* Service ID */

	dbg("Profile ID: [%d] Profile name: [%s] APN :[%s] Auth Type [%s] Auth ID: [%s] "
		"Auth Password: [%s] PDP Protocol: [%s] Proxy Address: [%s] Home URL: [%s] Service ID: [%s]",
		profile_id, profile_name, apn, auth_type, auth_id, auth_pwd, pdp_protocol, proxy_addr, home_url, svc_id);

	/* SQL Query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		" insert into pdp_profile(\
		 profile_id, profile_name, apn, auth_type, auth_id, auth_pwd, \
		 pdp_protocol, proxy_ip_addr, home_url, linger_time, \
		 network_info_id, svc_category_id, hidden, editable, \
		 default_internet_con, user_defined, is_roaming_apn, profile_enable) values(\
		 ?, ?, ?, ?, ?, ?, \
		 ?, ?, ?, 300, \
		 ?, ?, 0, 1, 0, 1, 0, 1)");

	rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
	if (rv == FALSE) {
		err("Failed to insert to Storage");
		profile_id = 0;
	}

	/* Free resources */
	g_hash_table_destroy(in_param);

EXIT:
	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return profile_id;
}

static int __ps_context_load_network_id_from_database(gchar *mccmnc, gchar *cp_name)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	int network_id = -1;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return network_id;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup(mccmnc));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL Query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select network_info_id from network_info where mccmnc = ? ");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 1);
	dbg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if (value) {
			g_hash_table_iter_init(&iter2, (GHashTable *)value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					if (!value2 || (g_strcmp0((const char *)value2, "") == 0))
						network_id = 0;
					else
						network_id = atoi((const char *)value2);

					/* TODO - Check this out */
					break;
				}
			}
		}
	}

	/* Free resources */
	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return network_id;
}

static gchar *__ps_context_load_network_name_from_database(int network_id, gchar *cp_name)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	gchar *network_name = NULL;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return NULL;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup_printf("%d", network_id));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
						(GDestroyNotify)g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select network_name from network_info where network_info_id = ? ");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 1);
	dbg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if (value) {
			g_hash_table_iter_init(&iter2, (GHashTable *)value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					g_free(network_name);
					network_name = g_strdup(value2);

					/* TODO - Check this logic */
					break;
				}
			}
		}
	}

	/* Free resources */
	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return network_name;
}

static int __ps_context_load_profile_id_from_database(gchar *cp_name)
{
	gpointer handle;
	GHashTable *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	int profile_id = -1;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return profile_id;
	}

	/* Initialize parameters */
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select max(profile_id) as last_profile from pdp_profile");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, NULL, out_param, 1);
	dbg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if (value) {
			g_hash_table_iter_init(&iter2, (GHashTable *)value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					if (!value2 || (g_strcmp0((const char *)value2, "") == 0))
						profile_id = 0;
					else
						profile_id = atoi((const char *)value2);

					/* TODO - Check this logic */
					break;
				}
			}
		}
	}

	/* Free resources */
	g_hash_table_destroy(out_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return profile_id;
}

static int __ps_context_load_num_of_pdn_from_database(gchar *mccmnc, gchar *cp_name)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	int num_of_pdn = 0;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return 0;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup(mccmnc));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select a.max_pdp_3g from max_pdp a, network_info b \
		where a.network_info_id = b.network_info_id and b.mccmnc = ? ");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 1);
	dbg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if (value) {
			g_hash_table_iter_init(&iter2, (GHashTable *)value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				if (g_str_equal(key2, "0") == TRUE) {
					if (!value2 || (g_strcmp0((const char *)value2, "") == 0)) {
						num_of_pdn = 3;
						dbg("There is NO value... Using 'default'");
					} else {
						num_of_pdn = atoi((const char *) value2);
						dbg("value (%d)", num_of_pdn);
					}

					/* TODO - Check this logic */
					break;
				}
			}
		}
	}

	if (num_of_pdn <= 0) {
		dbg("Loaded value is wrong... restoring to 'default'");
		num_of_pdn = PS_MAX_CID;
	} else if (num_of_pdn > PS_MAX_CID) {
		dbg("Loaded value is gretaer than 3... restoring to 'default'");
		num_of_pdn = PS_MAX_CID;
	}

	/* Free resources */
	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return num_of_pdn;
}

static gboolean __ps_context_remove_profile_tuple(dictionary *dic, int profile_index, gchar *cp_name)
{
	gpointer handle;
	gboolean rv = FALSE;
	GHashTable *in_param;
	gchar *network_info_id;
	gchar *section_key = NULL;
	char szQuery[5000];

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return rv;
	}

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	/* network info id */
	section_key = g_strdup_printf("connection:network_info_id_%d", profile_index);
	network_info_id = iniparser_getstr(dic, section_key);
	g_hash_table_insert(in_param, "1", g_strdup(network_info_id));
	g_free(section_key);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		" delete from pdp_profile where network_info_id = ?");

	rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
	dbg("delete from pdp_profile where network_info_id = %s, result(%d)", network_info_id, rv);
	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

static gboolean __ps_context_insert_profile_tuple(dictionary *dic, int profile_index, gchar *cp_name)
{
	gpointer handle;
	GHashTable *in_param;
	gboolean rv = FALSE;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return rv;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	{/* profile id */
		gchar *profile_id;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:profile_id_%d", profile_index);
		profile_id = iniparser_getstr(dic, item_key);
		if (profile_id == NULL) {
			g_free(item_key);
			goto EXIT;
		}
		g_hash_table_insert(in_param, "1", g_strdup(profile_id));
		g_free(item_key);
	}

	{/* profile name */
		gchar *profile_name;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:profile_name_%d", profile_index);
		profile_name = iniparser_getstr(dic, item_key);
		if (profile_name == NULL) {
			g_free(item_key);
			goto EXIT;
		}
		g_hash_table_insert(in_param, "2", g_strdup(profile_name));
		g_free(item_key);
	}

	{/* apn */
		gchar *apn;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:apn_%d", profile_index);
		apn = iniparser_getstr(dic, item_key);
		if (apn == NULL) {
			g_free(item_key);
			goto EXIT;
		}
		g_hash_table_insert(in_param, "3", g_strdup(apn));
		g_free(item_key);
	}

	{/* auth type */
		gchar *auth_type;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:auth_type_%d", profile_index);
		auth_type = iniparser_getstr(dic, item_key);
		if (auth_type == NULL)
			g_hash_table_insert(in_param, "4", g_strdup_printf("%d", CONTEXT_AUTH_NONE));
		else
			g_hash_table_insert(in_param, "4", g_strdup(auth_type));
		g_free(item_key);
	}

	{/* auth id */
		gchar *auth_id;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:auth_id_%d", profile_index);
		auth_id = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "5", g_strdup(auth_id));
		g_free(item_key);
	}

	{/* auth pwd */
		gchar *auth_pwd;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:auth_pwd_%d", profile_index);
		auth_pwd = iniparser_getstr(dic, item_key);
		g_hash_table_insert(in_param, "6", g_strdup(auth_pwd));
		g_free(item_key);
	}

	{/* pdp protocol */
		gchar *pdp_protocol;
		gchar *item_key = NULL;
		item_key = g_strdup_printf("connection:pdp_protocol_%d", profile_index);
		pdp_protocol = iniparser_getstr(dic, item_key);
		if (pdp_protocol == NULL)
			g_hash_table_insert(in_param, "7", g_strdup_printf("%d", CONTEXT_TYPE_IP));
		else
		g_hash_table_insert(in_param, "7", g_strdup(pdp_protocol));
		g_free(item_key);
	}

	{/* proxy ip */
		gchar *proxy_ip_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:proxy_ip_addr_%d", profile_index);
		proxy_ip_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "8", g_strdup(proxy_ip_addr));
		g_free(section_key);
	}

	{/* home url */
		gchar *home_url;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:home_url_%d", profile_index);
		home_url = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "9", g_strdup(home_url));
		g_free(section_key);
	}

	{/* linger time */
		gchar *linger_time;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:linger_time_%d", profile_index);
		linger_time = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "10", g_strdup(linger_time));
		g_free(section_key);
	}

	{/* traffic class */
		gchar *traffic_class;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:traffic_class_%d", profile_index);
		traffic_class = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "11", g_strdup(traffic_class));
		g_free(section_key);
	}

	{/* is static ip address */
		gchar *is_static_ip_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:is_static_ip_addr_%d", profile_index);
		is_static_ip_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "12", g_strdup(is_static_ip_addr));
		g_free(section_key);
	}

	{/* ip address if static ip is true */
		gchar *ip_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:ip_addr_%d", profile_index);
		ip_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "13", g_strdup(ip_addr));
		g_free(section_key);
	}

	{/* is static dns address */
		gchar *is_static_dns_addr;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:is_static_dns_addr_%d", profile_index);
		is_static_dns_addr = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "14", g_strdup(is_static_dns_addr));
		g_free(section_key);
	}

	{/* dns address 1 */
		gchar *dns_addr1;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:dns_addr1_%d", profile_index);
		dns_addr1 = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "15", g_strdup(dns_addr1));
		g_free(section_key);
	}

	{/* dns address 2 */
		gchar *dns_addr2;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:dns_addr2_%d", profile_index);
		dns_addr2 = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "16", g_strdup(dns_addr2));
		g_free(section_key);
	}

	{/* network info id */
		gchar *network_info_id;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:network_info_id_%d", profile_index);
		network_info_id = iniparser_getstr(dic, section_key);
		g_hash_table_insert(in_param, "17", g_strdup(network_info_id));
		g_free(section_key);
	}

	{/* service category id */
		gchar *svc_category_id;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:svc_category_id_%d", profile_index);
		svc_category_id = iniparser_getstr(dic, section_key);
		if (svc_category_id == NULL)
			g_hash_table_insert(in_param, "18", g_strdup_printf("%d", CONTEXT_ROLE_UNKNOWN));
		else
		g_hash_table_insert(in_param, "18", g_strdup(svc_category_id));
		g_free(section_key);
	}

	{/* hidden */
		gchar *hidden;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:hidden_%d", profile_index);
		hidden = iniparser_getstr(dic, section_key);
		if (hidden == NULL)
			g_hash_table_insert(in_param, "19", g_strdup_printf("%d", FALSE));
		else
		g_hash_table_insert(in_param, "19", g_strdup(hidden));
		g_free(section_key);
	}

	{/* editable */
		gchar *editable;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:editable_%d", profile_index);
		editable = iniparser_getstr(dic, section_key);
		if (editable == NULL)
			g_hash_table_insert(in_param, "20", g_strdup_printf("%d", TRUE));
		else
		g_hash_table_insert(in_param, "20", g_strdup(editable));
		g_free(section_key);
	}

	{/* default internet connection */
		gchar *default_internet_con;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:default_internet_con_%d", profile_index);
		default_internet_con = iniparser_getstr(dic, section_key);
		if (default_internet_con == NULL)
			g_hash_table_insert(in_param, "21", g_strdup_printf("%d", FALSE));
		else
		g_hash_table_insert(in_param, "21", g_strdup(default_internet_con));
		g_free(section_key);
	}

	{/* insert data into table */
		gchar *is_roaming_apn;
		gchar *section_key = NULL;
		section_key = g_strdup_printf("connection:is_roaming_apn_%d", profile_index);
		is_roaming_apn = iniparser_getstr(dic, section_key);
		if (is_roaming_apn == NULL)
			g_hash_table_insert(in_param, "22", g_strdup_printf("%d", FALSE));
		else
			g_hash_table_insert(in_param, "22", g_strdup(is_roaming_apn));
		g_free(section_key);
	}

	{/* insert data into table */
		char szQuery[5000];

		/* SQL query */
		memset(szQuery, 0x0, sizeof(szQuery));
		snprintf(szQuery, sizeof(szQuery), "%s",
			" insert into pdp_profile(\
			 profile_id, profile_name, apn, auth_type, auth_id, auth_pwd, \
			 pdp_protocol, proxy_ip_addr, home_url, linger_time, \
			 traffic_class, is_static_ip_addr, ip_addr, is_static_dns_addr, dns_addr1, dns_addr2, \
			 network_info_id, svc_category_id, hidden, editable, default_internet_con, \
			 user_defined, is_roaming_apn, profile_enable) values(\
			 ?, ?, ?, ?, ?, ?, \
			 ?, ?, ?, ?, \
			 ?, ?, ?, ?, ?, ?, \
			 ?, ?, ?, ?, ?, 0, ?, 1)");

		rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
		dbg("Insert to Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));
	}

EXIT:
	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

static int __ps_context_get_network_id(gchar *mccmnc, gchar *cp_name)
{
	int network_id;

	network_id = __ps_context_load_network_id_from_database(mccmnc, cp_name);
	dbg("network id(%d)", network_id);
	if (network_id > 0)
		return network_id;

	network_id = __ps_context_insert_network_id_to_database(mccmnc, cp_name);
	if (network_id <= 0)
		return -1;

	return network_id;
}

GVariant *__ps_context_get_profile_properties(gpointer object, GVariantBuilder *properties)
{
	gchar *s_authtype = NULL, *s_role = NULL, *s_type = NULL;
	ps_context_t *context = NULL;
	char *apn, *username, *password, *proxy_addr, *home_url, *profile_name;

	g_return_val_if_fail(object != NULL, NULL);
	g_return_val_if_fail(properties != NULL, NULL);

	context = (ps_context_t *) object;
	ps_dbg_ex_co(_ps_service_ref_co_network(_ps_context_ref_service(context)),
		"get profile properties, path(%s)", _ps_context_ref_path(context));

	s_authtype = g_strdup_printf("%d", tcore_context_get_auth(context->co_context));
	s_role = g_strdup_printf("%d", tcore_context_get_role(context->co_context));
	s_type = g_strdup_printf("%d", tcore_context_get_type(context->co_context));

	apn = tcore_context_get_apn(context->co_context);
	username = tcore_context_get_username(context->co_context);
	password = tcore_context_get_password(context->co_context);
	proxy_addr = tcore_context_get_proxy(context->co_context);
	home_url = tcore_context_get_mmsurl(context->co_context);
	profile_name = tcore_context_get_profile_name(context->co_context);
	g_variant_builder_init(properties, G_VARIANT_TYPE("a{ss}"));

	g_variant_builder_add(properties, "{ss}", "path", context->path);
	if (apn)
		g_variant_builder_add(properties, "{ss}", "apn", apn);

	if (s_authtype)
		g_variant_builder_add(properties, "{ss}", "auth_type", s_authtype);

	if (username)
		g_variant_builder_add(properties, "{ss}", "auth_id", username);

	if (password)
		g_variant_builder_add(properties, "{ss}", "auth_pwd", password);

	if (s_type)
		g_variant_builder_add(properties, "{ss}", "pdp_protocol", s_type);

	if (proxy_addr)
		g_variant_builder_add(properties, "{ss}", "proxy_addr", proxy_addr);

	if (home_url)
		g_variant_builder_add(properties, "{ss}", "home_url", home_url);

	if (s_role)
		g_variant_builder_add(properties, "{ss}", "svc_ctg_id", s_role);

	g_variant_builder_add(properties, "{ss}", "profile_name", profile_name);
	g_variant_builder_add(properties, "{ss}", "hidden", BOOL2STRING(context->hidden));
	g_variant_builder_add(properties, "{ss}", "editable", BOOL2STRING(context->editable));
	g_variant_builder_add(properties, "{ss}", "default_internet_conn", BOOL2STRING(context->is_default));
	g_variant_builder_add(properties, "{ss}", "profile_enable", BOOL2STRING(context->profile_enable));

	/* Freeing locally allocated memory */
	g_free(s_authtype);
	g_free(s_role);
	g_free(s_type);
	g_free(apn);
	g_free(username);
	g_free(password);
	g_free(proxy_addr);
	g_free(home_url);
	g_free(profile_name);

	dbg("Exiting");

	return g_variant_builder_end(properties);
}

static gboolean __ps_context_set_default_connection_enable(gpointer object, gboolean enabled)
{
	ps_context_t *context = object;

	g_return_val_if_fail(context != NULL, FALSE);

	context->is_default = enabled;
	return TRUE;
}

static gpointer __ps_context_add_context(gpointer modem, gchar *mccmnc, int profile_id)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	ps_modem_t *mdm = modem;
	CoreObject *co_modem = _ps_modem_ref_co_modem(mdm);
	GDBusConnection *conn = NULL;
	TcorePlugin *p = NULL;
	gchar *path = NULL;

	GHashTableIter iter;
	gpointer object = NULL;
	gpointer key, value;

	/* Initialize Storage */
	if (g_str_has_suffix(mdm->cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		ps_err_ex_co(co_modem, "Failed to get Storage handle");
		return NULL;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup_printf("%d", profile_id));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select \
		 a.network_info_id, a.network_name, a.mccmnc, \
		 b.profile_id, b.profile_name, b.apn, \
		 b.auth_type, b.auth_id, b.auth_pwd, \
		 b.proxy_ip_addr, b.home_url, b.pdp_protocol, \
		 b.linger_time, b.traffic_class, b.is_static_ip_addr, b.ip_addr, \
		 b.is_static_dns_addr, b.dns_addr1, b.dns_addr2, b.svc_category_id, b.hidden, b.editable, \
		 b.default_internet_con, b.user_defined, b.is_roaming_apn, b.profile_enable \
		 from network_info a, pdp_profile b \
		 where b.profile_id = ? and a.network_info_id = b.network_info_id ");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 26);
	ps_dbg_ex_co(co_modem, "Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	ps_dbg_ex_co(co_modem, "Create profile by Profile ID: [%d]", profile_id);
	conn = _ps_modem_ref_dbusconn(modem);
	p = _ps_modem_ref_plugin(modem);

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		/* Create context */
		object = __ps_context_create_context(conn, p, mccmnc, (GHashTable *)value, mdm->cp_name);
		path = _ps_context_ref_path(object);

		/* Insert to contexts */
		mdm->contexts = g_slist_append(mdm->contexts, object);
		ps_dbg_ex_co(co_modem, "context (%p, %s) insert to hash", object, path);
	}

	/* Free resources */
	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return object;
}

gboolean _ps_context_check_is_roaming_apn_support(gchar* mccmnc, gchar* cp_name)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE, ret = FALSE;
	guint profile_cnt;
	int network_info_id = -1;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return ret;
	}

	network_info_id = __ps_context_load_network_id_from_database(mccmnc, cp_name);
	if (network_info_id == -1)
		err("Failed to load network_info_id from mccmnc (%s)", mccmnc);

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup_printf("%d", network_info_id));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s", "select profile_id from pdp_profile \
		where network_info_id = ? and is_roaming_apn = 1 ");

	rv = tcore_storage_read_query_database(strg_db, handle, szQuery, in_param, out_param, 1);
	dbg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	profile_cnt = g_hash_table_size(out_param);
	if (profile_cnt > 0) {
		dbg("roaming profiles for (mccmnc: %s [%d]) exists: count[%d]", mccmnc, network_info_id,  profile_cnt);
		ret = TRUE;
	}
	/* Free resources */
	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);
	return ret;
}

gboolean _ps_context_initialize(gpointer plugin)
{
	gboolean rv = TRUE;

	rv &= __ps_context_create_storage_handle(plugin);
	dbg("Global variable initialized: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	return rv;
}

gboolean _ps_context_reset_profile_table(gchar *cp_name)
{
	gpointer handle;
	char szQuery[1000];
	gboolean rv = FALSE;

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return rv;
	}

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s", " delete from pdp_profile");

	rv = tcore_storage_remove_query_database(strg_db, handle, szQuery, NULL);
	dbg("Reset profile table: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

gboolean _ps_context_fill_profile_table_from_ini_file(gchar *cp_name)
{
	int profile_index = 1;
	int data_exist = 0;
	gchar *section_key = NULL;
	dictionary *dic = NULL;

	if (g_str_has_suffix(cp_name, "1"))
		dic = iniparser_load("/opt/system/csc-default/data/csc-default-data-connection-2.ini");
	else
		dic = iniparser_load("/opt/system/csc-default/data/csc-default-data-connection.ini");

	if (dic == NULL) {
		dbg("fail to load the csc default file");
		return FALSE;
	}

	/* delete first */
	do {
		section_key = g_strdup_printf("connection:profile_id_%d", profile_index);
		dbg("section key (%s)", section_key);
		data_exist = iniparser_find_entry(dic, section_key);
		if (!data_exist) {
			g_free(section_key);
			dbg("no more data in ini");
			break;
		}
		__ps_context_remove_profile_tuple(dic, profile_index, cp_name);
		g_free(section_key);
		profile_index++;
	} while (TRUE);

	/* insert later */
	profile_index = 1;
	do {
		section_key = g_strdup_printf("connection:profile_id_%d", profile_index);
		dbg("section key (%s)", section_key);
		data_exist = iniparser_find_entry(dic, section_key);
		if (!data_exist) {
			g_free(section_key);
			iniparser_freedict(dic);
			dbg("no more data in ini");
			break;
		}
		__ps_context_insert_profile_tuple(dic, profile_index, cp_name);
		g_free(section_key);
		profile_index++;
	} while (TRUE);

	return TRUE;
}

GSList* _ps_context_create_hashtable(gpointer modem, gboolean roaming)
{
	gpointer handle;
	GHashTable *in_param;
	GSList *out_param = NULL;
	char szQuery[5000];
	gboolean rv = FALSE, roaming_apn = FALSE;
	int retry = 1;
	unsigned int index;

	ps_modem_t *mdm = modem;
	CoreObject *co_modem = _ps_modem_ref_co_modem(mdm);

	/* Initialize Storage */
	if (g_str_has_suffix(mdm->cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		ps_err_ex_co(co_modem, "Failed to get Storage handle");
		return NULL;
	}

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	/* Check/Store roaming APN support at every SIM init complete. */
	roaming_apn = _ps_context_check_is_roaming_apn_support(mdm->operator, mdm->cp_name);
	_ps_modem_set_roaming_apn_support(modem, roaming_apn);

	if(roaming) {
		/* Check roaming APN support first in the Roaming network. */
		ps_warn_ex_co(co_modem, "Roaming Network[%s], Roaming APN Support[%s]",
			roaming ? "TRUE" : "FALSE", roaming_apn ? "TRUE" : "FALSE");
		roaming = roaming_apn;
	}
	ps_dbg_ex_co(co_modem, "create profile by mccmnc (%s)", mdm->operator);

	memset(szQuery, 0x0, sizeof(szQuery));
	snprintf(szQuery, sizeof(szQuery), "%s",
		"select \
		 a.network_info_id, a.network_name, a.mccmnc, \
		 b.profile_id, b.profile_name, b.apn, \
		 b.auth_type, b.auth_id, b.auth_pwd, \
		 b.proxy_ip_addr, b.home_url, b.pdp_protocol, \
		 b.linger_time, b.traffic_class, b.is_static_ip_addr, b.ip_addr, \
		 b.is_static_dns_addr, b.dns_addr1, b.dns_addr2, b.svc_category_id, b.hidden, b.editable, \
		 b.default_internet_con, b.user_defined, b.is_roaming_apn, b.profile_enable \
		 from network_info a, pdp_profile b \
		 where a.mccmnc = ? and b.is_roaming_apn = ? and a.network_info_id = b.network_info_id ");

	g_hash_table_insert(in_param, "1", g_strdup(mdm->operator));
	g_hash_table_insert(in_param, "2", g_strdup_printf("%d", roaming));

	for (retry = 0; retry < 5; retry++) {
		ps_dbg_ex_co(co_modem, "Reading database", mdm->operator);
		rv = tcore_storage_read_query_database_in_order(strg_db, handle, szQuery, in_param, &out_param, 26);
		if (rv != FALSE)
			break;
	}
	ps_dbg_ex_co(co_modem, "Read Database: [%s], Retry[%d]", (rv == TRUE ? "SUCCESS" : "FAIL"), retry);

	for (index = 0; index < g_slist_length(out_param); index++) {
		gchar *path = NULL;
		gpointer object = NULL;
		GHashTable *value = NULL;

		value = g_slist_nth_data(out_param, index);
		if(value == NULL)
			continue;
		/* Create new 'context' */
		object = __ps_context_create_context(mdm->conn, mdm->plg, mdm->operator, value, mdm->cp_name);
		path = _ps_context_ref_path(object);

		mdm->contexts = g_slist_append(mdm->contexts, object);
		ps_dbg_ex_co(co_modem, "context (%p, %s) insert to linked-list", object, path);
	}

	g_hash_table_destroy(in_param);
	g_slist_free_full(out_param, (GDestroyNotify)g_hash_table_destroy);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	dbg("Exiting");
	return mdm->contexts;
}

gboolean _ps_context_add_context(gpointer modem, gchar *operator, GHashTable *property)
{
	GHashTable *services = NULL;
	gpointer context = NULL;
	ps_modem_t *mdm = modem;

	GHashTableIter iter;
	gpointer key, value;
	int network_id = 0;
	int profile_id = 0;

	network_id = __ps_context_get_network_id(operator, mdm->cp_name);
	if (network_id <= 0) {
		dbg("fail to add network info");
		return FALSE;
	}

	profile_id = __ps_context_insert_profile_to_database(property, network_id, mdm->cp_name);
	if (profile_id <= 0) {
		dbg("fail to insert profile info to database");
		return FALSE;
	}

	context = __ps_context_add_context(modem, operator, profile_id);
	if (!context)
		return FALSE;

	services = _ps_modem_ref_services(modem);
	if (!services)
		return FALSE;

	g_hash_table_iter_init(&iter, services);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE)
		_ps_service_ref_context(value, context);

	return TRUE;
}

gboolean _ps_context_get_properties_handler(gpointer object, GVariantBuilder *properties)
{
	int context_state = 0;
	gboolean active = FALSE;
	ps_context_t *context = object;
	char *dev_name = NULL;
	char *proxy = NULL;
	char *ipv4_address, *ipv4_gateway, *ipv4_dns1, *ipv4_dns2;
	char *ipv6_address, *ipv6_gateway, *ipv6_dns1, *ipv6_dns2;


	dbg("get context properties");
	g_return_val_if_fail(context != NULL, FALSE);
	g_return_val_if_fail(properties != NULL, FALSE);

	context_state =    tcore_context_get_state(context->co_context);
	if (context_state == CONTEXT_STATE_ACTIVATED)
		active = TRUE;

	/* IPV4 data */
	ipv4_address = tcore_context_get_ipv4_addr(context->co_context);
	ipv4_gateway = tcore_context_get_ipv4_gw(context->co_context);
	ipv4_dns1 = tcore_context_get_ipv4_dns1(context->co_context);
	ipv4_dns2 = tcore_context_get_ipv4_dns2(context->co_context);

	/* IPV6 data */
	ipv6_address = tcore_context_get_ipv6_addr(context->co_context);
	ipv6_gateway = tcore_context_get_ipv6_gw(context->co_context);
	ipv6_dns1 = tcore_context_get_ipv6_dns1(context->co_context);
	ipv6_dns2 = tcore_context_get_ipv6_dns2(context->co_context);

	proxy = tcore_context_get_proxy(context->co_context);
	dev_name = tcore_context_get_ipv4_devname(context->co_context);

	g_variant_builder_open(properties, G_VARIANT_TYPE("a{ss}"));

	g_variant_builder_add(properties, "{ss}", "path", (context->path));
	g_variant_builder_add(properties, "{ss}", "active", (BOOL2STRING(active)));

	/* Adding IPV4 data to builder */
	if (ipv4_address)
		g_variant_builder_add(properties, "{ss}", "ipv4_address", ipv4_address);

	if (ipv4_gateway)
		g_variant_builder_add(properties, "{ss}", "ipv4_gateway", ipv4_gateway);

	if (ipv4_dns1)
		g_variant_builder_add(properties, "{ss}", "ipv4_dns1", ipv4_dns1);

	if (ipv4_dns2)
		g_variant_builder_add(properties, "{ss}", "ipv4_dns2", ipv4_dns2);

	/* Adding IPV6 data to builder */
	g_variant_builder_add(properties, "{ss}", "ipv6_address",
		(ipv6_address == NULL ? "::" : ipv6_address));
	g_variant_builder_add(properties, "{ss}", "ipv6_gateway",
		(ipv6_gateway == NULL ? "::" : ipv6_gateway));
	g_variant_builder_add(properties, "{ss}", "ipv6_dns1",
		(ipv6_dns1 == NULL ? "::" : ipv6_dns1));
	g_variant_builder_add(properties, "{ss}", "ipv6_dns2",
		(ipv6_dns2 == NULL ? "::" : ipv6_dns2));

	if (proxy)
		g_variant_builder_add(properties, "{ss}", "proxy", proxy);

	if (dev_name)
		g_variant_builder_add(properties, "{ss}", "dev_name", dev_name);

	g_variant_builder_add(properties, "{ss}", "default_internet_conn", (BOOL2STRING(context->is_default)));
	g_variant_builder_close(properties);

	/* Freeing local memory */
	g_free(ipv4_address);
	g_free(ipv4_gateway);
	g_free(ipv4_dns1);
	g_free(ipv4_dns2);
	g_free(ipv6_address);
	g_free(ipv6_gateway);
	g_free(ipv6_dns1);
	g_free(ipv6_dns2);
	g_free(proxy);
	g_free(dev_name);

	dbg("Exiting");
	return TRUE;
}


GVariant *_ps_context_get_properties(gpointer object, GVariantBuilder *properties)
{
	int context_state = 0;
	gboolean active = FALSE;
	ps_context_t *context = object;
	char *dev_name = NULL;
	char *proxy = NULL;
	char *ipv4_address, *ipv4_gateway, *ipv4_dns1, *ipv4_dns2;
	char *ipv6_address, *ipv6_gateway, *ipv6_dns1, *ipv6_dns2;
	pcscf_addr *pcscf_ipv4, *pcscf_ipv6;
	unsigned int i;

	ps_dbg_ex_co(_ps_service_ref_co_network(_ps_context_ref_service(context)), "get context properties");
	g_return_val_if_fail(context != NULL, NULL);
	g_return_val_if_fail(properties != NULL, NULL);

	context_state =    tcore_context_get_state(context->co_context);
	if (context_state == CONTEXT_STATE_ACTIVATED)
		active = TRUE;

	active &= context->b_active;

	/* IPV4 data */
	ipv4_address = tcore_context_get_ipv4_addr(context->co_context);
	ipv4_gateway = tcore_context_get_ipv4_gw(context->co_context);
	ipv4_dns1 = tcore_context_get_ipv4_dns1(context->co_context);
	ipv4_dns2 = tcore_context_get_ipv4_dns2(context->co_context);

	/* IPV6 data */
	ipv6_address = tcore_context_get_ipv6_addr(context->co_context);
	ipv6_gateway = tcore_context_get_ipv6_gw(context->co_context);
	ipv6_dns1 = tcore_context_get_ipv6_dns1(context->co_context);
	ipv6_dns2 = tcore_context_get_ipv6_dns2(context->co_context);

	/* P-CSCF data */
	pcscf_ipv4 = tcore_context_get_pcscf_ipv4_addr(context->co_context);
	pcscf_ipv6 = tcore_context_get_pcscf_ipv6_addr(context->co_context);

	proxy = tcore_context_get_proxy(context->co_context);
	dev_name = tcore_context_get_ipv4_devname(context->co_context);

	g_variant_builder_init(properties, G_VARIANT_TYPE("a{ss}"));

	g_variant_builder_add(properties, "{ss}", "path", (context->path));
	g_variant_builder_add(properties, "{ss}", "active", (BOOL2STRING(active)));
	g_variant_builder_add(properties, "{ss}", "routing_only", (BOOL2STRING(context->b_routing_only)));

	/* Adding IPV4 data to builder */
	if (ipv4_address)
		g_variant_builder_add(properties, "{ss}", "ipv4_address", ipv4_address);

	if (ipv4_gateway)
		g_variant_builder_add(properties, "{ss}", "ipv4_gateway", ipv4_gateway);

	if (ipv4_dns1)
		g_variant_builder_add(properties, "{ss}", "ipv4_dns1", ipv4_dns1);

	if (ipv4_dns2)
		g_variant_builder_add(properties, "{ss}", "ipv4_dns2", ipv4_dns2);

	/* Adding IPV6 data to builder */
	g_variant_builder_add(properties, "{ss}", "ipv6_address",
		(ipv6_address == NULL ? "::" : ipv6_address));
	g_variant_builder_add(properties, "{ss}", "ipv6_gateway",
		(ipv6_gateway == NULL ? "::" : ipv6_gateway));
	g_variant_builder_add(properties, "{ss}", "ipv6_dns1",
		(ipv6_dns1 == NULL ? "::" : ipv6_dns1));
	g_variant_builder_add(properties, "{ss}", "ipv6_dns2",
		(ipv6_dns2 == NULL ? "::" : ipv6_dns2));

	if (ipv6_address) {
		gboolean ipv6_link_only = FALSE;
		if (g_ascii_strncasecmp(ipv6_address, "fe80::", (gsize)6) == 0)
			ipv6_link_only = TRUE;
		g_variant_builder_add(properties, "{ss}", "ipv6_link_only", BOOL2STRING(ipv6_link_only));
	}

	if (proxy)
		g_variant_builder_add(properties, "{ss}", "proxy", proxy);

	if (dev_name)
		g_variant_builder_add(properties, "{ss}", "dev_name", dev_name);

	if (pcscf_ipv4) {
		char *buf;

		buf = g_strdup_printf("%d", pcscf_ipv4->count);
		g_variant_builder_add(properties, "{ss}", "pcscf_ipv4_count", buf);
		g_free(buf);

		for (i = 0; i < pcscf_ipv4->count; i++) {
			buf = g_strdup_printf("%s_%d", "pcscf_ipv4_addr", i);
			g_variant_builder_add(properties, "{ss}", buf, pcscf_ipv4->addr[i]);
			g_free(buf);
		}
	} else {
		/* Update pCSCF address (IPv4) count as '0' */
		g_variant_builder_add(properties, "{ss}", "pcscf_ipv4_count", "0");
	}

	if (pcscf_ipv6) {
		char *buf;

		buf = g_strdup_printf("%d", pcscf_ipv6->count);
		g_variant_builder_add(properties, "{ss}", "pcscf_ipv6_count", buf);
		g_free(buf);

		for (i = 0; i < pcscf_ipv6->count; i++) {
			buf = g_strdup_printf("%s_%d", "pcscf_ipv6_addr", i);
			g_variant_builder_add(properties, "{ss}", buf, pcscf_ipv6->addr[i]);
			g_free(buf);
		}
	} else {
		/* Update pCSCF address (IPv6) count as '0' */
		g_variant_builder_add(properties, "{ss}", "pcscf_ipv6_count", "0");
	}

	g_variant_builder_add(properties, "{ss}", "default_internet_conn", BOOL2STRING(context->is_default));

	/* Freeing local memory */
	if (pcscf_ipv4) {
		for (i = 0; i < pcscf_ipv4->count; i++)
			g_free(pcscf_ipv4->addr[i]);
		g_free(pcscf_ipv4);
	}
	if (pcscf_ipv6) {
		for (i = 0; i < pcscf_ipv6->count; i++)
			g_free(pcscf_ipv6->addr[i]);
		g_free(pcscf_ipv6);
	}
	g_free(ipv4_address);
	g_free(ipv4_gateway);
	g_free(ipv4_dns1);
	g_free(ipv4_dns2);
	g_free(ipv6_address);
	g_free(ipv6_gateway);
	g_free(ipv6_dns1);
	g_free(ipv6_dns2);
	g_free(proxy);
	g_free(dev_name);

	dbg("Exiting");
	return g_variant_builder_end(properties);
}

gboolean _ps_context_set_alwayson_enable(gpointer object, gboolean enabled)
{
	ps_context_t *context = object;
	int role = CONTEXT_ROLE_UNKNOWN;
	g_return_val_if_fail(context != NULL, FALSE);

	role = tcore_context_get_role(context->co_context);

	if ((role == CONTEXT_ROLE_INTERNET) && context->is_default)
		context->alwayson = enabled;

	dbg("context (%p) alwayson (%d)", context, context->alwayson);
	return TRUE;
}

gboolean _ps_context_get_default_context(gpointer object, int svc_cat_id)
{
	ps_context_t *context = object;
	int role = CONTEXT_ROLE_UNKNOWN;
	g_return_val_if_fail(context != NULL, FALSE);

	role = tcore_context_get_role(context->co_context);
	if (role == svc_cat_id && context->is_default)
		return TRUE;

	return FALSE;
}

gboolean _ps_context_get_non_default_context(gpointer object, int svc_cat_id)
{
	ps_context_t *context = object;
	int role = CONTEXT_ROLE_UNKNOWN;
	g_return_val_if_fail(context != NULL, FALSE);

	role = tcore_context_get_role(context->co_context);
	if (role == svc_cat_id && !context->is_default)
		return TRUE;

	return FALSE;
}

gboolean _ps_context_set_service(gpointer object, gpointer service)
{
	ps_context_t *context = object;
	g_return_val_if_fail(context != NULL, FALSE);

	context->p_service = service;
	return TRUE;
}

gpointer _ps_context_ref_service(gpointer object)
{
	ps_context_t *context = object;
	g_return_val_if_fail(context != NULL, NULL);

	return context->p_service;
}

gchar *_ps_context_ref_path(gpointer object)
{
	ps_context_t *context = object;
	g_return_val_if_fail(context != NULL, NULL);

	return context->path;
}

gboolean _ps_context_get_alwayson_enable(gpointer object)
{
	ps_context_t *context = object;
	g_return_val_if_fail(context != NULL, FALSE);
	dbg("context (%p) alwayson (%d)", context, context->alwayson);
	return context->alwayson;
}

gpointer _ps_context_ref_co_context(gpointer object)
{
	ps_context_t *context = object;
	g_return_val_if_fail(context != NULL, NULL);

	return context->co_context;
}

gboolean _ps_context_set_connected(gpointer object, gboolean enabled)
{
	gchar *ipv4 = NULL;
	gchar *ipv6 = NULL;
	ps_context_t *context = object;
	Storage *strg_vconf = NULL;
	gpointer p_modem = NULL;

	gboolean b_roaming_checker = TRUE;
	gboolean data_allowed = FALSE;
	gboolean b_mms_checker = FALSE;
	gboolean b_ims_checker = TRUE;

	enum co_context_role role = CONTEXT_ROLE_UNKNOWN;

	dbg("Entry [enabled :%d]", enabled);

	g_return_val_if_fail(context != NULL, FALSE);

	strg_vconf = tcore_server_find_storage(tcore_plugin_ref_server(context->plg), "vconf");
	data_allowed = tcore_storage_get_bool(strg_vconf, STORAGE_KEY_3G_ENABLE);
	ipv4 = tcore_context_get_ipv4_addr(context->co_context);
	ipv6 = tcore_context_get_ipv6_addr(context->co_context);
	role = tcore_context_get_role(context->co_context);
	p_modem = _ps_service_ref_modem(context->p_service);

	if (role == CONTEXT_ROLE_MMS || role == CONTEXT_ROLE_PREPAID_MMS)
		b_mms_checker = TRUE;

	if (role == CONTEXT_ROLE_IMS || role == CONTEXT_ROLE_IMS_EMERGENCY)
		b_ims_checker = TRUE;

#if !defined(TIZEN_SUPPORT_MMS_CONNECT_FORCE)
		ps_dbg_ex_co(_ps_service_ref_co_network(_ps_context_ref_service(context)), "csc runtime feature disabled");
		b_mms_checker = FALSE;
#endif

	context->b_active = enabled;
	if ((_ps_modem_get_roaming(p_modem)) && !(_ps_modem_get_data_roaming_allowed(p_modem))) {
		dbg("roaming network is not allowed");
		b_roaming_checker = FALSE;
	}

	if (enabled) {
		gint ps_mode = 0;

		tcore_context_set_state(context->co_context, CONTEXT_STATE_ACTIVATED);

		if (context->deact_required == TRUE) {
			warn("Deactivation is required for context(%p)", context);
			_ps_service_deactivate_context(context->p_service, context);
			context->deact_required = FALSE;
			goto EXIT;
		}

		if (ipv4) {
			if ((g_str_equal(ipv4, "0.0.0.0") == TRUE) && (ipv6 == NULL)) {
				dbg("ip address is NULL");
				_ps_service_deactivate_context(context->p_service, context);
				goto EXIT;
			}
		}
		_ps_service_reset_connection_timer(context);

		/* In case of PDP is disconnected by network in UPS mode.
		 * default internet PDP could be activated if LCD was on at trigger time.
		 */
		ps_mode = _ps_modem_get_psmode(p_modem);
		warn("ps_mode: %d", ps_mode);
		if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
			gint pm_state = tcore_storage_get_int(strg_vconf, STORAGE_KEY_PM_STATE);
			if (pm_state == 3 && _ps_context_get_default_context(context, CONTEXT_ROLE_INTERNET)) {
				char *devname = tcore_context_get_ipv4_devname(context->co_context);
				if (TCORE_RETURN_SUCCESS != tcore_util_netif_down(devname))
					err("Failed to bring up interface");

				warn("[EXCEPTION] do not emit signal for PDP activation.");
				context->b_notify = TRUE;
				goto EXIT;
			}
		}

		if (b_roaming_checker && (data_allowed || b_mms_checker || b_ims_checker))
			__ps_context_emit_property_changed_signal(context);

	} else {
		tcore_context_set_state(context->co_context, CONTEXT_STATE_DEACTIVATED);
		tcore_context_reset_devinfo(context->co_context);
		__ps_context_emit_property_changed_signal(context);
	}
EXIT:
	if (ipv4)
		free(ipv4);

	g_free(ipv6);

	return TRUE;
}

gboolean _ps_context_set_profile_enable(gpointer object, gboolean value)
{
	ps_context_t *context = object;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));

	dbg("Entry [value :%d]", value);

	g_return_val_if_fail(context != NULL, FALSE);

	context->profile_enable = value;
	ps_dbg_ex_co(co_network, "context(%p) profile_enable(%d)", context, context->profile_enable);
	return TRUE;
}

gboolean _ps_context_get_profile_enable(gpointer object)
{
	ps_context_t *context = object;

	ps_dbg_ex_co(_ps_service_ref_co_network(_ps_context_ref_service(context)), "context(%p), profile_enable(%d)", context, context->profile_enable);

	return context->profile_enable;
}

gboolean _ps_context_set_ps_defined(gpointer object, gboolean value)
{
	ps_context_t *context = (ps_context_t *)object;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));

	dbg("Entry [value :%d]", value);

	g_return_val_if_fail(context != NULL, FALSE);

	context->ps_defined = value;
	ps_dbg_ex_co(co_network, "context(%p) ps_defined(%d)", context, context->ps_defined);
	return TRUE;
}

gboolean _ps_context_get_ps_defined(gpointer object)
{
	ps_context_t *context = (ps_context_t *)object;

	ps_dbg_ex_co(_ps_service_ref_co_network(_ps_context_ref_service(context)), "context(%p), ps_defined(%d)", context, context->ps_defined);

	return context->ps_defined;
}

gboolean _ps_context_reset_user_data(gpointer object)
{
	ps_context_t *context = (ps_context_t *)object;

	g_return_val_if_fail(context != NULL, FALSE);
	context->user_data = NULL;

	return TRUE;
}

gboolean _ps_context_set_bearer_info(gpointer object, struct tnoti_ps_dedicated_bearer_info *bearer_info)
{
	ps_context_t *context = (ps_context_t *)object;
	CoreObject *co_context = NULL;

	g_return_val_if_fail(context != NULL, FALSE);
	g_return_val_if_fail(bearer_info != NULL, FALSE);

	co_context = _ps_context_ref_co_context(object);

	if (bearer_info->dedicated_bearer.num_dedicated_bearer > 0) {
		warn("num_dedicated_bearer: %d", bearer_info->dedicated_bearer.num_dedicated_bearer);
		/* reset previous bearer info. */
		tcore_context_reset_bearer_info(co_context);
		tcore_context_set_bearer_info(co_context, bearer_info);
		__ps_context_emit_dedicated_bearer_info_signal(context);
	}

	return TRUE;
}

gpointer _ps_context_get_user_data(gpointer object)
{
	ps_context_t *context = (ps_context_t *)object;
	return context->user_data;
}

TReturn _ps_connection_hdlr(gpointer object)
{
	int rv = TCORE_RETURN_FAILURE;
	ps_context_t *pscontext = object;
	ps_service_t *service = _ps_context_ref_service(pscontext);
	CoreObject *co_network = _ps_service_ref_co_network(service);

	/* TBD: dbus method invocation from upper layer. */
	if (TRUE == _ps_context_get_default_context(pscontext, CONTEXT_ROLE_INTERNET)) {
		ps_dbg_ex_co(co_network, "Change Always-on when default internet.");
		_ps_master_set_always_on_control(_ps_modem_ref_master(_ps_service_ref_modem(service)), TRUE);
	}
	rv = _ps_service_activate_context(pscontext->p_service, pscontext);
	if (rv != TCORE_RETURN_SUCCESS) {
		ps_dbg_ex_co(co_network, "fail to activate context connection");
		return rv;
	}

	ps_dbg_ex_co(co_network, "success to activate context");
	return rv;
}

gboolean _ps_context_create_cdma_profile(gchar *mccmnc, gchar *cp_name)
{
	gpointer handle;
	gboolean rv = FALSE;
	GHashTable *in_param;
	char szQuery[5000];

	/* Initialize Storage */
	if (g_str_has_suffix(cp_name, "1"))
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_1);
	else
		handle = tcore_storage_create_handle(strg_db, DATABASE_PATH_0);
	if (handle == NULL) {
		err("Failed to get Storage handle");
		return FALSE;
	}

	/* Make default MMS profile for CDMA */
	memset(szQuery, 0, 5000);
	snprintf(szQuery, sizeof(szQuery), "%s",
	" update pdp_profile set  \
	 home_url = ? \
	 where profile_id = 1 and svc_category_id = 2 ");

	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	g_hash_table_insert(in_param, "1", g_strdup("http://mms.vtext.com/servlets/mms")); /* home_url */

	rv = tcore_storage_insert_query_database(strg_db, handle, szQuery, in_param);
	dbg("insert into pdp_profile result(%d)", rv);
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	tcore_storage_remove_handle(strg_db, handle);

	return rv;
}

void _ps_default_connection_hdlr(gpointer object)
{
	ps_context_t *pscontext = (ps_context_t *)object;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(pscontext));

	__ps_context_update_default_internet_to_db(pscontext, TRUE);

	/* set request profile */
	tcore_context_set_default_profile(pscontext->co_context, TRUE);
	__ps_context_set_default_connection_enable(pscontext, TRUE);
	_ps_context_set_alwayson_enable(pscontext, TRUE);
	__ps_context_emit_property_changed_signal(pscontext);

	if (CONTEXT_ROLE_INTERNET == tcore_context_get_role(_ps_context_ref_co_context(object))) {
		int rv = TCORE_RETURN_FAILURE;
		ps_dbg_ex_co(co_network, "activation requeset in new ps (%p)", pscontext);
		/* request to connect */
		_ps_service_reset_connection_timer(pscontext);
		rv = _ps_service_connect_default_context(pscontext->p_service);
		if (rv == TCORE_RETURN_PS_NETWORK_NOT_READY) {
			unsigned char cid = -1;
			ps_service_t * p_service = (ps_service_t *)pscontext->p_service;

			ps_dbg_ex_co(co_network, "PS is not attached yet, release resources.");

			cid = tcore_context_get_id(pscontext->co_context);
			_ps_context_set_ps_defined((gpointer)pscontext, FALSE);
			tcore_ps_set_cid_active(p_service->co_ps, cid, FALSE);
			tcore_ps_clear_context_id(p_service->co_ps, pscontext->co_context);
		}
	}
	ps_dbg_ex_co(co_network, "complete to change the default connection");
	return;
}

gint _ps_context_get_number_of_pdn(gchar *operator, gchar *cp_name)
{
	gint num_of_pdn = 0;

	num_of_pdn = __ps_context_load_num_of_pdn_from_database(operator, cp_name);
	dbg("loaded num_of_pdn (%d)", num_of_pdn);

	return num_of_pdn;
}

gboolean _ps_context_handle_ifaceup(gpointer user_data)
{
	ps_context_t *pscontext = user_data;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(pscontext));
	int context_state = 0;

	context_state =  tcore_context_get_state(pscontext->co_context);
	ps_warn_ex_co(co_network, "context_state: %d", context_state);
	if (context_state == CONTEXT_STATE_ACTIVATED) {
		char *devname = tcore_context_get_ipv4_devname(pscontext->co_context);
		gint ps_mode = _ps_modem_get_psmode(_ps_service_ref_modem(pscontext->p_service));
		if (TCORE_RETURN_SUCCESS != tcore_util_netif_up(devname))
			ps_err_ex_co(co_network, "Failed to bring up interface");

		if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
			/* If this flag is true, Connman won't update cellular service state. */
			if (pscontext->b_notify) {
				ps_warn_ex_co(co_network, "[EXCEPTION] notify status change to upper layer.");
				pscontext->b_routing_only = FALSE;
			} else {
				pscontext->b_routing_only = TRUE;
			}
		}
		/*
		 * 20131212, Deprecated: Fixed by HTTP stack.
		 * ===============================================================================
		 * 20130801, JIRA DCM-2221: HTTP communication behavior while bearer switching
		 * Observations: When contiguous HTTP requests while ME handovers from wi-fi to 3G,
		 * libcurl does not make Aborted event to application
		 * even if libcurl receives socket closed event by SIOCKILLADDR.
		 * So, we add work-around patch set here.
		 * ===============================================================================
		 */
		if (pscontext->b_active == FALSE) {
			pscontext->b_active = TRUE;
			ps_dbg_ex_co(co_network, "Celluar profile: Emit property signal to provide IP configuration, devname(%s)", devname);
			__ps_context_emit_property_changed_signal(pscontext);
		}
		pscontext->b_routing_only = FALSE;
		pscontext->b_notify = FALSE;
		return TRUE;
	} else if (context_state == CONTEXT_STATE_DEACTIVATED) {
		/* trigger PDP activation.  */
		_ps_service_activate_context(pscontext->p_service, pscontext);
	}
	return FALSE;
}

gboolean _ps_context_handle_ifacedown(gpointer user_data)
{
	ps_context_t *pscontext = user_data;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(pscontext));
	int context_state = 0;

	context_state =  tcore_context_get_state(pscontext->co_context);
	ps_warn_ex_co(co_network, "context_state: %d", context_state);
	if (context_state == CONTEXT_STATE_ACTIVATED) {
		char *devname = tcore_context_get_ipv4_devname(pscontext->co_context);
		gint ps_mode = _ps_modem_get_psmode(_ps_service_ref_modem(pscontext->p_service));
		ps_dbg_ex_co(co_network, "Cellular profile: Do not send PDP deactivation request message to Modem.");
		if (ps_mode > POWER_SAVING_MODE_NORMAL && ps_mode < POWER_SAVING_MODE_WEARABLE) {
			/* If this flag is true, Connman won't update cellular service state. */
			pscontext->b_routing_only = TRUE;
		} else {
			ps_warn_ex_co(co_network, "reset socket connections, devname(%s)", devname);
			if (TCORE_RETURN_SUCCESS != tcore_util_reset_ipv4_socket(devname, tcore_context_get_ipv4_addr(pscontext->co_context)))
				ps_err_ex_co(co_network, "Failed to reset socket.");
		}

		if (TCORE_RETURN_SUCCESS != tcore_util_netif_down(devname))
			ps_err_ex_co(co_network, "Failed to bring down interface");
		if (pscontext->b_active == TRUE) {
			pscontext->b_active = FALSE;
			__ps_context_emit_property_changed_signal(pscontext);
		}
		pscontext->b_routing_only = FALSE;
		return TRUE;
	}
	return FALSE;
}

static gboolean on_context_get_properties(PacketServiceContext *obj_context,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariant *gv = NULL;
	GVariantBuilder property;

	if (!ps_util_check_access_control(invocation, AC_PS_PUBLIC, "r"))
		return TRUE;

	dbg("Entered");
	gv = _ps_context_get_properties(user_data, &property);
	packet_service_context_complete_get_properties(obj_context, invocation, gv);
	return TRUE;
}

static gboolean on_context_get_profile(PacketServiceContext *obj_context,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	GVariant *gv = NULL;
	GVariantBuilder profile;

	if (!ps_util_check_access_control(invocation, AC_PS_PUBLIC, "r"))
		return TRUE;

	dbg("Entered");
	gv = __ps_context_get_profile_properties(user_data, &profile);
	packet_service_context_complete_get_profile(obj_context, invocation, gv);

	return TRUE;
}

static gboolean on_context_handle_activate(PacketServiceContext *obj_context,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	gboolean rv = FALSE;
	TReturn result = TCORE_RETURN_FAILURE;

	gchar *apn = NULL;
	int context_state = 0;
	unsigned int max_pdn = 0, num_of_active_cids = 0;
	gpointer p_service = NULL; gpointer co_ps = NULL;
	gpointer c_def_context = NULL; unsigned char cid_def = 0;
	GSList *active_cids = NULL;
	CoreObject *co_network;

	ps_context_t *pscontext = user_data;

	if (!ps_util_check_access_control(invocation, AC_PS_PRIVATE, "w"))
		return TRUE;

	dbg("Entered");
	if (pscontext == NULL) {
		err("activation request object is NULL");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	p_service = pscontext->p_service;
	if (!p_service) {
		err("service object is null");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	co_network = _ps_service_ref_co_network(_ps_context_ref_service(pscontext));
	co_ps = _ps_service_ref_co_ps(p_service);
	if (!co_ps) {
		ps_err_ex_co(co_network, "core object is null");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	ps_dbg_ex_co(co_network, "activate context(%s)", _ps_context_ref_path(pscontext));

	apn = (gchar *)tcore_context_get_apn(pscontext->co_context);

	context_state = tcore_context_get_state(pscontext->co_context);
	if (context_state != CONTEXT_STATE_DEACTIVATED) {
		ps_warn_ex_co(co_network, "operation is in progress");
		FAIL_RESPONSE(invocation,  PS_ERR_INTERNAL);
		return TRUE;
	}

	ps_dbg_ex_co(co_network, "requested context(%p) co_context(%p) apn (%s)", pscontext, pscontext->co_context, apn);
	/* check apn is activated or not */
	rv = tcore_ps_is_active_apn(co_ps, (const char *)apn);
	if (rv) {
		ps_dbg_ex_co(co_network, "requested apn is already activated");

		result = _ps_connection_hdlr(pscontext);
		if (result != TCORE_RETURN_SUCCESS) {
			FAIL_RESPONSE(invocation,  PS_ERR_INTERNAL);
			return TRUE;
		}

		packet_service_context_complete_activate(obj_context, invocation, pscontext->path);
		context_state = tcore_context_get_state(pscontext->co_context);
		if (context_state == CONTEXT_STATE_ACTIVATED) {
			ps_dbg_ex_co(co_network, "context is already connected");
			_ps_context_set_connected(pscontext, TRUE);
		}

		ps_dbg_ex_co(co_network, "success to open connection request");
		return TRUE;
	}

	active_cids = tcore_ps_get_active_cids(co_ps);
	num_of_active_cids = g_slist_length(active_cids);
	max_pdn = tcore_ps_get_num_of_pdn(co_ps);
	ps_dbg_ex_co(co_network, "activate cids(%d), max pdn(%d)", num_of_active_cids, max_pdn);
	if (num_of_active_cids < max_pdn) {
		ps_dbg_ex_co(co_network, "enough to active another pdn");
		result = _ps_connection_hdlr(pscontext);
		if (result != TCORE_RETURN_SUCCESS) {
			FAIL_RESPONSE(invocation,  PS_ERR_INTERNAL);
			return TRUE;
		}

		ps_dbg_ex_co(co_network, "success to open connection request");
		packet_service_context_complete_activate(obj_context, invocation, pscontext->path);
		return TRUE;
	}

	/* find the current default connection */
	c_def_context = _ps_service_return_default_context(p_service, CONTEXT_ROLE_INTERNET);
	if (c_def_context == NULL) {
		err("default context is NULL");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}
	cid_def = tcore_context_get_id(((ps_context_t *)c_def_context)->co_context);
	if (cid_def == 0) {
		ps_err_ex_co(co_network, "it is not avaiable to open connection");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	/* check the status of def context */
	context_state = tcore_context_get_state(((ps_context_t *)c_def_context)->co_context);
	if (context_state != CONTEXT_STATE_ACTIVATED) {
		enum co_context_role context_role;
		context_role = tcore_context_get_role(pscontext->co_context);
		ps_err_ex_co(co_network, "default connection is in progress, new context_state[%d], role[%d]", context_state, context_role);
		if (context_state == CONTEXT_STATE_ACTIVATING &&
			(context_role == CONTEXT_ROLE_MMS ||context_role == CONTEXT_ROLE_PREPAID_MMS)) {
			((ps_context_t *)c_def_context)->user_data = pscontext;
			packet_service_context_complete_activate(obj_context, invocation, pscontext->path);
			return TRUE;
		}
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	_ps_service_reset_connection_timer(c_def_context);
	((ps_context_t *)c_def_context)->user_data = pscontext;
	result = tcore_ps_deactivate_cid(co_ps, cid_def);
	if (result != TCORE_RETURN_SUCCESS) {
		ps_err_ex_co(co_network, "fail to deactivate exist network connection");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	packet_service_context_complete_activate(obj_context, invocation, pscontext->path);
	return TRUE;

}

static gboolean on_context_handle_deactiavte(PacketServiceContext *obj_context,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	gboolean rv = FALSE;
	CoreObject *co_network;
	int context_state = 0;
	ps_context_t *pscontext = user_data;
	ps_service_t *service = _ps_context_ref_service(pscontext);

	if (!ps_util_check_access_control(invocation, AC_PS_PRIVATE, "w"))
		return TRUE;

	dbg("Entered");
	if (pscontext == NULL) {
		err("deactivation request object is NULL");
		FAIL_RESPONSE(invocation,  PS_ERR_NO_PROFILE);
		return TRUE;
	}

	co_network = _ps_service_ref_co_network(_ps_context_ref_service(pscontext));
	context_state = tcore_context_get_state(pscontext->co_context);
	ps_dbg_ex_co(co_network, "requested context(%p) co_context(%p), context_state %d", pscontext, pscontext->co_context, context_state);
	if (context_state != CONTEXT_STATE_ACTIVATED) {
		ps_err_ex_co(co_network, "operation is in progress");
		FAIL_RESPONSE(invocation,  PS_ERR_INTERNAL);
		return TRUE;
	}

	ps_dbg_ex_co(co_network, "deactivate context(%s)", _ps_context_ref_path(pscontext));

	_ps_service_reset_connection_timer(pscontext);
	/* TBD: dbus method invocation from upper layer. */
	if (TRUE == _ps_context_get_default_context(pscontext, CONTEXT_ROLE_INTERNET)) {
		ps_dbg_ex_co(co_network, "Change Always-on when default internet.");
		_ps_master_set_always_on_control(_ps_modem_ref_master(_ps_service_ref_modem(service)), FALSE);
	}
	rv = _ps_service_deactivate_context(service, pscontext);
	if (rv != TCORE_RETURN_SUCCESS) {
		ps_err_ex_co(co_network, "fail to deactivate context connection");
		FAIL_RESPONSE(invocation,  PS_ERR_TRASPORT);
		return TRUE;
	}

	ps_dbg_ex_co(co_network, "success to deactivate context");
	packet_service_context_complete_deactivate(obj_context, invocation, pscontext->path);

	context_state =  tcore_context_get_state(pscontext->co_context);
	if (context_state == CONTEXT_STATE_DEACTIVATED) {
		ps_dbg_ex_co(co_network, "context is already disconnected");
		pscontext->ps_defined = FALSE;
		_ps_context_set_connected(pscontext, FALSE);
	}

	return TRUE;
}

static gboolean on_context_set_default_connection(PacketServiceContext *obj_context,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	int rv = 0;
	int role = CONTEXT_ROLE_UNKNOWN;
	gboolean is_default = FALSE;

	gpointer co_ps = NULL;
	gpointer service = NULL;
	gpointer cur_default_ctx = NULL;
	ps_context_t *pscontext = user_data;
	CoreObject *co_network;

	if (!ps_util_check_access_control(invocation, AC_PS_PROFILE, "w"))
		return TRUE;

	dbg("enter set default connection ps_context_t(%p)", pscontext);
	if (pscontext == NULL) {
		err("activation request object is NULL");
		FAIL_RESPONSE(invocation, PS_ERR_NO_PROFILE);
		return TRUE;
	}

	co_network = _ps_service_ref_co_network(_ps_context_ref_service(pscontext));
	ps_dbg_ex_co(co_network, "start default connection");
	role = tcore_context_get_role(pscontext->co_context);
	is_default = tcore_context_get_default_profile(pscontext->co_context);

	if (is_default) {
		ps_err_ex_co(co_network, "already default profile for role (%d).", role);
		goto OUT;
	}

	service = pscontext->p_service;
	cur_default_ctx = _ps_service_return_default_context(service, role);
	ps_dbg_ex_co(co_network, "current default connection (%p)", cur_default_ctx);

	if (!cur_default_ctx) {
		ps_err_ex_co(co_network, "No current default connection.");
		goto OUT;
	}

	/* First, send deactivation request first. */
	rv = _ps_service_deactivate_context(((ps_context_t *)cur_default_ctx)->p_service, cur_default_ctx);
	if (rv == TCORE_RETURN_PS_ACTIVATING) {
		ps_dbg_ex_co(co_network, "fail to deactivate default connection, rv(%d)", rv);
		FAIL_RESPONSE(invocation, PS_ERR_INTERNAL);
		return TRUE;
	}

	/* Normal deactivation case. */
	if (tcore_context_get_state(((ps_context_t *)cur_default_ctx)->co_context) == CONTEXT_STATE_DEACTIVATING) {
		ps_dbg_ex_co(co_network, "deactivation request in current ps (%p)", cur_default_ctx);
		((ps_context_t *)cur_default_ctx)->user_data = pscontext;
	} else {
		int cid = -1;

		ps_dbg_ex_co(co_network, "[Not normal] deactivation request in current ps (%p)", cur_default_ctx);

		cid = tcore_context_get_id(((ps_context_t *)cur_default_ctx)->co_context);
		_ps_context_set_ps_defined(cur_default_ctx, FALSE);
		co_ps = _ps_service_ref_co_ps(service);
		tcore_ps_set_cid_active((CoreObject *)co_ps, cid, FALSE);
		tcore_ps_clear_context_id((CoreObject *)co_ps, ((ps_context_t *)cur_default_ctx)->co_context);
	}

	/* unset default info of previous connection */
	_ps_context_set_alwayson_enable(cur_default_ctx, FALSE);
	__ps_context_set_default_connection_enable(cur_default_ctx, FALSE);
	tcore_context_set_default_profile(((ps_context_t *)cur_default_ctx)->co_context, FALSE);

	/* db update - release default connection */
	ps_dbg_ex_co(co_network, "context(%p): release default connection property.", cur_default_ctx);
	__ps_context_update_default_internet_to_db((ps_context_t *)cur_default_ctx, FALSE);
	/* Allow Connman to update profile information. */
	__ps_context_emit_property_changed_signal(cur_default_ctx);

	ps_dbg_ex_co(co_network, "activation requeset in new ps (%p)", pscontext);
OUT:
	_ps_default_connection_hdlr(pscontext);
	packet_service_context_complete_set_default_connection(obj_context, invocation, TRUE);
	return TRUE;

}

static gboolean on_context_modify_profile(PacketServiceContext *obj_context,
		GDBusMethodInvocation *invocation,
		GVariant *property,
		gpointer user_data)
{
	GVariantIter g_iter;
	gchar *g_value;
	gchar *g_key;

	gboolean rv = FALSE;
	int context_state = 0;
	ps_context_t *context = user_data;
	CoreObject *co_network = _ps_service_ref_co_network(_ps_context_ref_service(context));
	GHashTable *profile_property = NULL;

	if (!ps_util_check_access_control(invocation, AC_PS_PROFILE, "w"))
		return TRUE;

	ps_dbg_ex_co(co_network, "modify context's profile properties");

	context_state = tcore_context_get_state(context->co_context);
	if (context_state == CONTEXT_STATE_ACTIVATING) {
		ps_dbg_ex_co(co_network, "Modify profile in activating state, set deactivate flag.");
		context->deact_required = TRUE;
		goto EXIT;
	}

	/*Creating the profile property hash for for internal handling*/
	/*Create a hash table for the profile property as all fucntion already use ghash table */
	profile_property = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_variant_iter_init(&g_iter, property);
	while (g_variant_iter_next(&g_iter, "{ss}", &g_key, &g_value)) {

		g_hash_table_insert(profile_property, g_strdup(g_key), g_strdup(g_value));
		/* must free data for ourselves */
		g_free(g_value);
		g_free(g_key);
	}

	rv = __ps_context_update_profile(context, profile_property);
	if (rv != TRUE) {
		FAIL_RESPONSE(invocation,  PS_ERR_INTERNAL);
		g_hash_table_destroy(profile_property);
		return TRUE;
	}

	_ps_service_deactivate_context(context->p_service, context);
	if (context_state == CONTEXT_STATE_DEACTIVATED) {
		ps_dbg_ex_co(co_network, "context is already disconnected");
		_ps_context_set_connected(context, FALSE);
	}
EXIT:
	packet_service_context_complete_modify_profile(obj_context, invocation, TRUE);
	g_hash_table_destroy(profile_property);
	return TRUE;
}

static gboolean on_context_remove_profile(PacketServiceContext *obj_context,
	GDBusMethodInvocation *invocation, gpointer user_data)
{
	ps_context_t *context = user_data;
	ps_service_t *service = _ps_context_ref_service(context);
	CoreObject *co_network = _ps_service_ref_co_network(service);

	if (!ps_util_check_access_control(invocation, AC_PS_PROFILE, "w"))
		return TRUE;
	g_return_val_if_fail(service != NULL, FALSE);

	ps_dbg_ex_co(co_network, "Remove context.");

	__ps_context_remove_database(context);
	_ps_service_unref_context(service, context);
	service->contexts = g_slist_remove(service->contexts, context);
	_ps_context_remove_context(context);

	packet_service_context_complete_remove_profile(obj_context, invocation, TRUE);

	ps_dbg_ex_co(co_network, "Exit");
	return TRUE;
}

static void _ps_context_setup_interface(PacketServiceContext *context, ps_context_t *context_data)
{
	dbg("Entered");
	g_signal_connect(context,
			"handle-get-properties",
			G_CALLBACK(on_context_get_properties),
			context_data);

	g_signal_connect(context,
			"handle-get-profile",
			G_CALLBACK(on_context_get_profile),
			context_data);

	g_signal_connect(context,
			"handle-activate",
			G_CALLBACK(on_context_handle_activate),
			context_data);

	g_signal_connect(context,
			"handle-deactivate",
			G_CALLBACK(on_context_handle_deactiavte),
			context_data);

	g_signal_connect(context,
			"handle-set-default-connection",
			G_CALLBACK(on_context_set_default_connection),
			context_data);

	g_signal_connect(context,
			"handle-modify-profile",
			G_CALLBACK(on_context_modify_profile),
			context_data);

	g_signal_connect(context,
			"handle-remove-profile",
			G_CALLBACK(on_context_remove_profile),
			context_data);
	dbg("Exiting");
	return;
}
