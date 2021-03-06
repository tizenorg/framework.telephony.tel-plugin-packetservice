PRAGMA journal_mode = PERSIST;
                                      
DROP TABLE IF EXISTS "fd_blacklist";
CREATE TABLE fd_blacklist(
	plmn	INTEGER
);

DROP TABLE IF EXISTS "etc_profile";
CREATE TABLE etc_profile(
	profile_id	INTEGER PRIMARY KEY,
	transport_type	INTEGER,
	proxy_ip_addr	TEXT
	);
	
INSERT INTO "etc_profile" (profile_id, transport_type, proxy_ip_addr)  VALUES(8,2,'168.219.61.250:8080');

DROP TABLE IF EXISTS "network_info";
CREATE TABLE network_info(
	network_info_id INTEGER PRIMARY KEY,
	network_name TEXT,
	mccmnc TEXT
	);

DROP TABLE IF EXISTS "fast_dormancy";
CREATE TABLE fast_dormancy(
	dormant_id	INTEGER PRIMARY KEY,
	network_info_id INTEGER UNIQUE,
	enable_status INTEGER,
	timeout INTEGER
  );

DROP TABLE IF EXISTS "max_pdp";
CREATE TABLE max_pdp(
	max_id	INTEGER PRIMARY KEY,
	network_info_id INTEGER UNIQUE,
	max_pdp_3g INTEGER
  );
  
DROP TABLE IF EXISTS "pdp_profile";
CREATE TABLE pdp_profile(
	profile_id	INTEGER PRIMARY KEY,
	transport_type	INTEGER,
	profile_name	TEXT,
	apn	TEXT,
	auth_type	INTEGER,
	auth_id	TEXT,
	auth_pwd	TEXT,
	pdp_protocol	INTEGER,
	proxy_ip_addr	TEXT,
	home_url	TEXT,
	linger_time	INTEGER,
	is_secure_connection	INTEGER,
	app_protocol_type	INTEGER,
	traffic_class	INTEGER,
	is_static_ip_addr	INTEGER,
	ip_addr	TEXT,
	is_static_dns_addr	INTEGER,
	dns_addr1	TEXT,
	dns_addr2	TEXT,
	network_info_id INTEGER,
	svc_category_id	INTEGER
	);

DROP TABLE IF EXISTS "svc_category";
CREATE TABLE svc_category(
	svc_category_id	INTEGER PRIMARY KEY,
	svc_name	TEXT
	);
	
INSERT INTO "svc_category" VALUES(0,'LTE IMS');
INSERT INTO "svc_category" VALUES(1,'internet');
INSERT INTO "svc_category" VALUES(2,'mms');
INSERT INTO "svc_category" VALUES(3,'prepaid internet');
INSERT INTO "svc_category" VALUES(4,'prepaid mms');
INSERT INTO "svc_category" VALUES(5,'wap');

CREATE INDEX pdp_profile_ix_1 ON pdp_profile (network_info_id);
