#!/bin/sh
if [ -f /opt/dbspace/.dnet.db ]
then
/bin/echo 'alter table pdp_profile add is_roaming_apn INTEGER default 0;' | /usr/bin/sqlite3 /opt/dbspace/.dnet.db
/bin/echo 'alter table pdp_profile add profile_enable INTEGER default 1;' | /usr/bin/sqlite3 /opt/dbspace/.dnet.db
fi

if [ -f /opt/dbspace/.dnet2.db ]
then
/bin/echo 'alter table pdp_profile add is_roaming_apn INTEGER default 0;' | /usr/bin/sqlite3 /opt/dbspace/.dnet2.db
/bin/echo 'alter table pdp_profile add profile_enable INTEGER default 1;' | /usr/bin/sqlite3 /opt/dbspace/.dnet2.db
fi
