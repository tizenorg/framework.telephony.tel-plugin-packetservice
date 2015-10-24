/*
 * libslp-tapi
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: Ja-young Gu <jygu@samsung.com>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <wait.h>
#include <errno.h>

#include <glib.h>
#include <gio/gio.h>
#include <db-util.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#define DATABASE_PATH "/opt/dbspace/.dnet.db"
#define msg(fmt, args...) do { printf(fmt "\n", ##args); fflush(stdout); } while (0)

enum context_type {
	CONTEXT_TYPE_UNKNOWN,
	CONTEXT_TYPE_X25,
	CONTEXT_TYPE_IP,
	CONTEXT_TYPE_IHOST,
	CONTEXT_TYPE_PPP,
	CONTEXT_TYPE_IPV6,
	CONTEXT_TYPE_IPV4V6,
};

enum context_role {
	CONTEXT_ROLE_UNKNOWN,
	CONTEXT_ROLE_INTERNET,
	CONTEXT_ROLE_MMS,
	CONTEXT_ROLE_PREPAID_INTERNET,
	CONTEXT_ROLE_PREPAID_MMS,
	CONTEXT_ROLE_TETHERING,
	CONTEXT_ROLE_USER_DEFINED,
};

static int __system_command(char *command)
{
	int pid = 0,
	status = 0;
	const char *environ[] = { NULL };

	if (command == NULL)
		return -1;

	msg("%s", command);

	pid = fork();
	if (pid == -1)
		return -1;

	if (pid == 0) {
		char *argv[4];

		argv[0] = "/bin/sh";
		argv[1] = "-c";
		argv[2] = (char *)command;
		argv[3] = 0;

		execve("/bin/sh", argv, (char **)environ);
		exit(127);
	}

	do {
		if (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR)
				return -1;
		} else {
			if (WIFEXITED(status))
				return WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				return WTERMSIG(status);
			else if (WIFSTOPPED(status))
				return WSTOPSIG(status);
		}
	} while (!WIFEXITED(status) && !WIFSIGNALED(status));

	return 0;
}

static void __load_xml_file(const char *docname, const char *groupname, void **i_doc, void **i_root_node)
{
	xmlDocPtr *doc = (xmlDocPtr *)i_doc;
	xmlNodePtr *root_node = (xmlNodePtr *)i_root_node;

	msg("docname:%s, groupname:%s", docname, groupname);

	*doc = xmlParseFile(docname);
	if (*doc) {
		*root_node = xmlDocGetRootElement(*doc);
		if (*root_node) {
			msg("*root_node->name:%s", (*root_node)->name);
			if (0 == xmlStrcmp((*root_node)->name, (const unsigned char *)groupname)) {
				msg("root_node is found !!!");
				return;
			} else {
				msg("Cannot find root node.");
				*root_node = NULL;
			}
		}
		xmlFreeDoc(*doc);
		*doc = NULL;
	} else {
		msg("fail to parse doc(%s)", docname);
	}
}

static void __unload_xml_file(void **i_doc, void **i_root_node)
{
	xmlDocPtr *doc = (xmlDocPtr *)i_doc;
	xmlNodePtr *root_node = (xmlNodePtr *)i_root_node;

	msg("unloading XML");
	if (doc && *doc) {
		xmlFreeDoc(*doc);
		*doc = NULL;
		if (root_node)
			*root_node = NULL;
	}
}

static void *create_handle(const char *path)
{
	int rv = 0;
	sqlite3 *handle = NULL;

	rv = db_util_open(path, &handle, 0);
	if (rv != SQLITE_OK) {
		msg("fail to connect database rv(%d)", rv);
		return NULL;
	}

	msg("connected to %s", path);
	return handle;
}

static gboolean remove_handle(void *handle)
{
	if (!handle)
		return FALSE;

	db_util_close(handle);

	msg("disconnected from database");
	return TRUE;
}

static gboolean read_query_database(void *handle, const char *query, GHashTable *in_param,
		GHashTable *out_param, int out_param_cnt)
{
	int rv = 0, index = 0, outter_index = 0;
	sqlite3_stmt *stmt = NULL;
	char szQuery[5000+1];	/* +1 is for NULL Termination Character '\0' */

	GHashTableIter iter;
	gpointer key, value;

	msg("read query");

	memset(szQuery, '\0', 5001);
	strncpy(szQuery, query, 5000);

	rv = sqlite3_prepare_v2(handle, szQuery, strlen(szQuery), &stmt, NULL);
	if (rv != SQLITE_OK) {
		msg("fail to connect to table (%d)", rv);
		return FALSE;
	}

	if (in_param) {
		g_hash_table_iter_init(&iter, in_param);
		while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
			msg("key(%s), value(%s)", (const char *)key, (const char *)value);

			if (!value || g_strcmp0((const char *) value, "") == 0) {
				msg("bind null");
				rv = sqlite3_bind_null(stmt, atoi((const char *) key));
			} else {
				msg("bind value");
				rv = sqlite3_bind_text(stmt, atoi((const char *) key), (const char *) value, strlen((const char *) value),
						SQLITE_STATIC);
			}

			if (rv != SQLITE_OK) {
				msg("fail to bind data (%d)", rv);
				return FALSE;
			}
		}
	}

	rv = sqlite3_step(stmt);
	msg("read query executed (%d)", rv);

	while (rv == SQLITE_ROW) {

		char tmp_key_outter[10];
		GHashTable *out_param_data;

		out_param_data = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

		for (index = 0; index < out_param_cnt; index++) {
			char *tmp = NULL, tmp_key[10];
			tmp = (char *) sqlite3_column_text(stmt, index);
			snprintf(tmp_key, sizeof(tmp_key), "%d", index);
			g_hash_table_insert(out_param_data, g_strdup(tmp_key), g_strdup(tmp));
		}

		snprintf(tmp_key_outter, sizeof(tmp_key_outter), "%d", outter_index);
		g_hash_table_insert(out_param, g_strdup(tmp_key_outter), out_param_data);
		outter_index++;
		rv = sqlite3_step(stmt);
	}

	sqlite3_finalize(stmt);
	return TRUE;
}

static gboolean query_database(void *handle, const char *query, GHashTable *in_param)
{
	int rv = 0;
	sqlite3_stmt *stmt = NULL;
	char szQuery[5000+1];	/* +1 is for NULL Termination Character '\0' */

	GHashTableIter iter;
	gpointer key, value;
	msg("query database");

	memset(szQuery, '\0', 5001);
	strncpy(szQuery, query, 5000);

	rv = sqlite3_prepare_v2(handle, szQuery, strlen(szQuery), &stmt, NULL);
	if (rv != SQLITE_OK) {
		msg("fail to connect to table (%d)", rv);
		return FALSE;
	}

	if (in_param) {
		g_hash_table_iter_init(&iter, in_param);
		while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
			msg("key(%s), value(%s)", (const char *)key, (const char *)value);

			if (!value || g_strcmp0((const char *) value, "") == 0) {
				msg("bind null");
				rv = sqlite3_bind_null(stmt, atoi((const char *) key));
			} else {
				msg("bind value");
				rv = sqlite3_bind_text(stmt, atoi((const char *) key), (const char *) value, strlen((const char *) value),
						SQLITE_STATIC);
			}

			if (rv != SQLITE_OK) {
				msg("fail to bind data (%d)", rv);
				return FALSE;
			}
		}
	}

	rv = sqlite3_step(stmt);
	msg("query executed (%d)", rv);
	sqlite3_finalize(stmt);

	if (rv != SQLITE_DONE)
		return FALSE;

	return TRUE;
}

static gboolean __reset_database(void)
{
	gpointer handle;
	char szQuery[5000];
	gboolean rv = FALSE;

	/* Initialize Storage */
	handle = create_handle(DATABASE_PATH);
	if (handle == NULL) {
		msg("Failed to get Storage handle");
		return rv;
	}

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	strcat(szQuery, " delete from pdp_profile");

	rv = query_database(handle, szQuery, NULL);
	msg("Reset profile table: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	/* De-initialize Storage */
	remove_handle(handle);

	return rv;
}

static int __insert_network_id_to_database(gchar *mccmnc)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;
	int network_id = 0;

	/* Initialize Database */
	handle = create_handle(DATABASE_PATH);
	if (handle == NULL) {
		msg("Failed to get Storage handle");
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
	strcpy(szQuery, "select max(network_info_id) as network_id from network_info");

	rv = read_query_database(handle, szQuery, NULL, out_param, 1);
	msg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	g_hash_table_iter_init(&iter, out_param);
	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		GHashTableIter iter2;
		gpointer key2, value2;

		if (value) {
			g_hash_table_iter_init(&iter2, (GHashTable *)value);
			while (g_hash_table_iter_next(&iter2, &key2, &value2) == TRUE) {
				msg("key2(%s) value2(%s)", (const char *)key2, (const char *)value2);
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
	strcpy(szQuery, " insert into network_info(network_info_id, network_name, mccmnc) values(?, ?, ?) ");

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup_printf("%d", network_id));	/* Network ID */
	g_hash_table_insert(in_param, "2", g_strdup_printf("PLMN_%s", mccmnc));
	g_hash_table_insert(in_param, "3", g_strdup(mccmnc));

	rv = query_database(handle, szQuery, in_param);
	if (rv == FALSE) {
		msg("Failed to insert query to Storage");
		network_id = 0;
	}

	/* Free resources */
	g_hash_table_destroy(in_param);

	/* De-initialize Storage */
	remove_handle(handle);

	return network_id;
}


static int __load_network_id_from_database(gchar *mccmnc)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	int network_id = -1;

	/* Initialize Storage */
	handle = create_handle(DATABASE_PATH);
	if (handle == NULL) {
		msg("Failed to get Storage handle");
		return network_id;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup(mccmnc));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL Query */
	memset(szQuery, 0x0, sizeof(szQuery));
	strcpy(szQuery, "select network_info_id from network_info where mccmnc = ? ");

	rv = read_query_database(handle, szQuery, in_param, out_param, 1);
	msg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

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
	remove_handle(handle);

	return network_id;
}


static int __get_network_id(gchar *mccmnc)
{
	int network_id;

	network_id = __load_network_id_from_database(mccmnc);
	msg("network id(%d)", network_id);
	if (network_id > 0)
		return network_id;

	network_id = __insert_network_id_to_database(mccmnc);
	if (network_id <= 0)
		return -1;

	return network_id;
}

static int __load_profile_id_from_database(void)
{
	gpointer handle;
	GHashTable *out_param;
	char szQuery[5000];
	gboolean rv = FALSE;

	GHashTableIter iter;
	gpointer key, value;

	int profile_id = -1;

	/* Initialize Database */
	handle = create_handle(DATABASE_PATH);
	if (handle == NULL) {
		msg("Failed to get Storage handle");
		return profile_id;
	}

	/* Initialize parameters */
	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	strcpy(szQuery, "select max(profile_id) as last_profile from pdp_profile");

	rv = read_query_database(handle, szQuery, NULL, out_param, 1);
	msg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

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
	remove_handle(handle);
	return profile_id;
}

static gboolean __get_default_profile_from_database(int network_info_id, int svc_category_id)
{
	gpointer handle;
	GHashTable *in_param, *out_param;
	char szQuery[5000];
	gboolean rv, ret = FALSE;
	guint profile_cnt;

	/* Initialize Storage */
	handle = create_handle(DATABASE_PATH);
	if (handle == NULL) {
		msg("Failed to get Storage handle");
		return FALSE;
	}

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(in_param, "1", g_strdup_printf("%d", network_info_id));
	g_hash_table_insert(in_param, "2", g_strdup_printf("%d", svc_category_id));

	out_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
			(GDestroyNotify) g_hash_table_destroy);

	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	strcpy(szQuery, "select profile_id from pdp_profile ");
	strcat(szQuery, "where network_info_id = ? and svc_category_id = ? and default_internet_con = 1");

	rv = read_query_database(handle, szQuery, in_param, out_param, 1);
	msg("Read Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	profile_cnt = g_hash_table_size(out_param);
	if (profile_cnt > 0) {
		msg("default profile for (svc_category_id: %d, network_info_id: %d) exists: count[%d]",
			svc_category_id, network_info_id, profile_cnt);
		ret = TRUE;
	}
	/* Free resources */
	g_hash_table_destroy(in_param);
	g_hash_table_destroy(out_param);

	/* De-initialize Database */
	remove_handle(handle);
	return ret;
}

static gboolean __insert_apns_to_database(GHashTable *in_param)
{
	gpointer handle;
	char szQuery[5000];
	gboolean rv = FALSE;

	if (in_param == NULL) {
		msg("in_param is NULL !!!");
		return rv;
	}

	/* Initialize Database */
	handle = create_handle(DATABASE_PATH);
	if (handle == NULL) {
		msg("Failed to get db handle");
		return rv;
	}
	/* SQL query */
	memset(szQuery, 0x0, sizeof(szQuery));
	strcpy(szQuery, " insert into pdp_profile(");
	strcat(szQuery, " profile_id, profile_name, apn, auth_type, auth_id, auth_pwd, ");
	strcat(szQuery, " pdp_protocol, svc_category_id, proxy_ip_addr, home_url, linger_time, ");
	strcat(szQuery, " network_info_id, hidden, editable, default_internet_con, user_defined) values(");
	strcat(szQuery, " ?, ?, ?, ?, ?, ?, ");/* 1, 2, 3, 4, 5, 6(auth_pwd) */
	strcat(szQuery, " ?, ?, ?, ?, 300, ");/* 7, 8, 9, 10(home_url) */
	strcat(szQuery, " ?, 0, 1, ?, 0)");/* 11, 12(default_internet_con) */

	rv = query_database(handle, szQuery, in_param);
	msg("Insert to Database: [%s]", (rv == TRUE ? "SUCCESS" : "FAIL"));

	/* De-initialize Database */
	remove_handle(handle);
	return rv;
}

static gboolean __duplicate_profile_by_type(GHashTable *in_param, gpointer node, int svc_category_id)
{
	gpointer tmp;
	xmlNode *cur_node = node;
	gchar *in_tuple = NULL;
	int profile_index;

	if (!in_param || !node)
		return FALSE;

	tmp = g_hash_table_lookup(in_param, "1");
	if (tmp) { /* profile_id */
		profile_index = atoi((char *)tmp);
		profile_index++;
		g_hash_table_insert(in_param, "1", g_strdup_printf("%d", profile_index));
		msg("profile_id = %d", profile_index);
	} else {
		return FALSE;
	}

	{/* svc_category_id */
		g_hash_table_insert(in_param, "8", g_strdup_printf("%d", svc_category_id));
		msg("svc_category_id = %d", svc_category_id);
	}

	{/* proxy ip */
		gchar *proxy_ip_addr = NULL, *proxy = NULL, *port = NULL;

		if (svc_category_id == CONTEXT_ROLE_MMS) {
			proxy = (char *)xmlGetProp(cur_node, (const unsigned char *)"mmsproxy");
			port = (char *)xmlGetProp(cur_node, (const unsigned char *)"mmsport");
		} else {
			proxy = (char *)xmlGetProp(cur_node, (const unsigned char *)"proxy");
			port = (char *)xmlGetProp(cur_node, (const unsigned char *)"port");
		}
		if (proxy && port) {
			proxy_ip_addr = g_strdup_printf("%s:%s", proxy, port);
			in_tuple = g_strdup(proxy_ip_addr);
			g_free(proxy_ip_addr);
		} else {
			in_tuple = g_strdup("");
		}
		g_hash_table_insert(in_param, "9", g_strdup(in_tuple));
		msg("proxy_ip_addr = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* home url */
		gchar *mmsc = NULL;
		mmsc = (char *)xmlGetProp(cur_node, (const unsigned char *)"mmsc");
		if (mmsc && svc_category_id == CONTEXT_ROLE_MMS)
			in_tuple = g_strdup(mmsc);
		else
			in_tuple = g_strdup("");
		g_hash_table_insert(in_param, "10", g_strdup(in_tuple));
		msg("home_url = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* default internet connection */
		int default_internet_con = 1; /* default */

		tmp = g_hash_table_lookup(in_param, "11"); /* network_info_id */
		if (tmp) {
			int network_info_id = atoi((char *)tmp);
			msg("network_info_id = %d", network_info_id);
			if (network_info_id > 0 && __get_default_profile_from_database(network_info_id, svc_category_id))
				default_internet_con = 0;
		}
		g_hash_table_insert(in_param, "12", g_strdup_printf("%d", default_internet_con));
		msg("default_internet_con = %d", default_internet_con);
	}

	/* insert duplacte profile to database. */
	return __insert_apns_to_database(in_param);
}

static GHashTable *__construct_profile_tuples(gpointer node)
{
	xmlNode *cur_node = node;
	GHashTable *in_param = NULL;
	gchar *in_tuple = NULL;
	int profile_id = 0, network_info_id = -1, svc_category_id = 0;

	if (!cur_node)
		return NULL;

	/* Initialize parameters */
	in_param = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	{/* profile id */
		profile_id = __load_profile_id_from_database();
		profile_id++;
		g_hash_table_insert(in_param, "1", g_strdup_printf("%d", profile_id));
		msg("profile_id = %d", profile_id);
	}

	{/* profile name */
		gchar *profile_name = NULL;
		profile_name = (char *)xmlGetProp(cur_node, (const unsigned char *)"carrier");
		if (profile_name)
			in_tuple = g_strdup(profile_name);
		else
			in_tuple = g_strdup_printf("TEMP_PROFILE_%d", profile_id);

		g_hash_table_insert(in_param, "2", g_strdup(in_tuple));
		msg("profile_name = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* apn */
		gchar *apn = NULL;
		apn = (char *)xmlGetProp(cur_node, (const unsigned char *)"apn");
		if (apn)
			in_tuple = g_strdup(apn);
		else
			in_tuple = g_strdup("");
		g_hash_table_insert(in_param, "3", g_strdup(in_tuple));
		msg("apn = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* auth type */
		gchar *auth_type = NULL, *auth = NULL;
		auth_type = (char *)xmlGetProp(cur_node, (const unsigned char *)"auth_type");
		auth = (char *)xmlGetProp(cur_node, (const unsigned char *)"auth");
		if (auth_type)
			in_tuple = g_strdup(auth_type);
		else if (auth)
			in_tuple = g_strdup(auth);
		else
			in_tuple = g_strdup("0"); /* CONTEXT_AUTH_NONE */

		g_hash_table_insert(in_param, "4", g_strdup(in_tuple));
		msg("auth_type = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* auth id */
		gchar *auth_id = NULL;
		auth_id = (char *)xmlGetProp(cur_node, (const unsigned char *)"user");
		if (auth_id)
			in_tuple = g_strdup(auth_id);
		else
			in_tuple = g_strdup("");
		g_hash_table_insert(in_param, "5", g_strdup(in_tuple));
		msg("auth_id = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* auth pwd */
		gchar *auth_pwd = NULL;
		auth_pwd = (char *)xmlGetProp(cur_node, (const unsigned char *)"password");
		if (auth_pwd)
			in_tuple = g_strdup(auth_pwd);
		else
			in_tuple = g_strdup("");
		g_hash_table_insert(in_param, "6", g_strdup(in_tuple));
		msg("auth_pwd = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* pdp protocol */
		gchar *protocol = NULL;
		int pdp_protocol = CONTEXT_TYPE_IP;
		protocol = (char *)xmlGetProp(cur_node, (const unsigned char *)"protocol");
		if (protocol) {
			if (!g_strcmp0(protocol, "IPV6"))
				pdp_protocol = CONTEXT_TYPE_IPV6;
			else if (!g_strcmp0(protocol, "IPV4V6"))
				pdp_protocol = CONTEXT_TYPE_IPV4V6;
		}
		g_hash_table_insert(in_param, "7", g_strdup_printf("%d", pdp_protocol));
		msg("protocol = %s", protocol);
	}

	{/* service category id */
		gchar *svc_type = NULL;
		svc_type = (char *)xmlGetProp(cur_node, (const unsigned char *)"type");
		if (NULL != g_strrstr(svc_type, "default"))
			svc_category_id = CONTEXT_ROLE_INTERNET;
		else if (!g_strcmp0(svc_type, "mms"))
			svc_category_id = CONTEXT_ROLE_MMS;
		else if (!g_strcmp0(svc_type, "dun"))
			svc_category_id = CONTEXT_ROLE_TETHERING;

		g_hash_table_insert(in_param, "8", g_strdup_printf("%d", svc_category_id));
		msg("svc_category_id = %d", svc_category_id);
	}

	{/* proxy ip */
		gchar *proxy_ip_addr = NULL, *proxy = NULL, *port = NULL;

		if (svc_category_id == CONTEXT_ROLE_MMS) {
			proxy = (char *)xmlGetProp(cur_node, (const unsigned char *)"mmsproxy");
			port = (char *)xmlGetProp(cur_node, (const unsigned char *)"mmsport");
		} else {
			proxy = (char *)xmlGetProp(cur_node, (const unsigned char *)"proxy");
			port = (char *)xmlGetProp(cur_node, (const unsigned char *)"port");
		}
		if (proxy && port) {
			proxy_ip_addr = g_strdup_printf("%s:%s", proxy, port);
			in_tuple = g_strdup(proxy_ip_addr);
			g_free(proxy_ip_addr);
		} else {
			in_tuple = g_strdup("");
		}
		g_hash_table_insert(in_param, "9", g_strdup(in_tuple));
		msg("proxy_ip_addr = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* home url */
		gchar *mmsc = NULL;
		mmsc = (char *)xmlGetProp(cur_node, (const unsigned char *)"mmsc");
		if (mmsc && svc_category_id == CONTEXT_ROLE_MMS)
			in_tuple = g_strdup(mmsc);
		else
			in_tuple = g_strdup("");
		g_hash_table_insert(in_param, "10", g_strdup(in_tuple));
		msg("home_url = %s", in_tuple);
		g_free(in_tuple);
	}

	{/* network info id */
		gchar *plmn = NULL, *mcc = NULL, *mnc = NULL;
		mcc = (char *)xmlGetProp(cur_node, (const unsigned char *)"mcc");
		mnc = (char *)xmlGetProp(cur_node, (const unsigned char *)"mnc");

		if (mcc && mnc) {
			plmn = g_strdup_printf("%s%s", mcc, mnc);
			msg("mccmnc = %s", plmn);
			network_info_id = __get_network_id(plmn);
			g_free(plmn);
		}
		g_hash_table_insert(in_param, "11", g_strdup_printf("%d", network_info_id));
		msg("network_info_id = %d", network_info_id);
	}

	{/* default internet connection */
		int default_internet_con = 1;
		if (__get_default_profile_from_database(network_info_id, svc_category_id))
			default_internet_con = 0;
		g_hash_table_insert(in_param, "12", g_strdup_printf("%d", default_internet_con));
		msg("default_internet_con = %d", default_internet_con);
	}

	return in_param;
}

static gboolean __init_global_apns_from_xml(const char *file_path)
{
	xmlNode *cur_node = NULL;
	xmlNodePtr cur, root_node;
	void *xml_doc = NULL, *xml_root_node = NULL;
	char *version = NULL;
	gboolean ret = FALSE;

	/* remove pdp_profile table first. */
	__reset_database();

	__load_xml_file(file_path, "apns", &xml_doc, &xml_root_node);
	if (!xml_root_node) {
		msg("[APNS CONF] Load error - Root node is NULL.");
		goto EXIT;
	}
	root_node = (xmlNodePtr)xml_root_node;
	version = (char *)xmlGetProp(root_node, (const unsigned char *)"version");
	if (version)
		msg("apns-conf.xml <apns version=\"%s\">", version);
	cur = root_node->xmlChildrenNode;
	/* Compare property */
	for (cur_node = cur; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			GHashTable *in_param = NULL;
			gchar *svc_type = NULL;
			gboolean rv = FALSE;

			in_param = __construct_profile_tuples(cur_node);
			rv = __insert_apns_to_database(in_param);
			if (rv == FALSE)
				continue;

			/* duplicate profiles for the same APNs */
			svc_type = (char *)xmlGetProp(cur_node, (const unsigned char *)"type");
			if (NULL != g_strrstr(svc_type, "default")) {
				if (NULL != g_strrstr(svc_type, "mms")) {
					/* type="default, supl, mms" */
					__duplicate_profile_by_type(in_param, cur_node, CONTEXT_ROLE_MMS);
					if (NULL != g_strrstr(svc_type, "dun")) {
						/* type="default, supl, mms, dun" */
						__duplicate_profile_by_type(in_param, cur_node, CONTEXT_ROLE_TETHERING);
					}
				} else if (NULL != g_strrstr(svc_type, "dun")) {
					/* type="default, supl, dun" */
					__duplicate_profile_by_type(in_param, cur_node, CONTEXT_ROLE_TETHERING);
				}
			}
			g_hash_table_destroy(in_param);
		}
	}
EXIT:
	__unload_xml_file(&xml_doc, &xml_root_node);
	return ret;
}


int main(int arg, char **argv)
{
	int rv;
	__init_global_apns_from_xml("/usr/share/ps-plugin/apns-conf.xml");
	rv = __system_command("/bin/mkdir /opt/usr/share/telephony");
	msg("system command sent, rv(%d)", rv);
	/* remove exist sql */
	rv = __system_command("/bin/rm /opt/usr/share/telephony/dnet_db_init.sql");
	msg("system command sent, rv(%d)", rv);
	/* Dump pdp_profile to sql */
	rv = __system_command("/usr/bin/sqlite3 /opt/dbspace/.dnet.db .dump | grep \"INSERT INTO \\\"pdp_profile\\\"\" > /opt/usr/share/telephony/dnet_db_init.sql");
	msg("system command sent, rv(%d)", rv);
	return 0;
}

