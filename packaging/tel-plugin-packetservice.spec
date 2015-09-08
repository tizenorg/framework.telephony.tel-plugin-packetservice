%define major 0
%define minor 2
%define patchlevel 89

Name:           tel-plugin-packetservice
Version:        %{major}.%{minor}.%{patchlevel}
Release:        3
License:        Apache-2.0
Summary:        Telephony Packet Service library
Group:          System/Libraries
Source0:        tel-plugin-packetservice-%{version}.tar.gz
BuildRequires:  cmake
BuildRequires:  python
BuildRequires:  python-xml
BuildRequires:  model-build-features
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(iniparser)
BuildRequires:  pkgconfig(tcore)
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
Telephony Packet Service library

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix} -DSYSCONFDIR=%{_sysconfdir} \
%if 0%{?model_build_feature_connect_default_connection_without_timer}
        -DCONNECT_DEFAULT_CONNECTION_WITHOUT_TIMER=1 \
%endif

make %{?_smp_mflags}

%post
/sbin/ldconfig

#create db
mkdir -p /opt/dbspace

if [ ! -f /opt/dbspace/.dnet.db ]
then
  sqlite3 /opt/dbspace/.dnet.db < /usr/share/ps-plugin/dnet_db.sql
  sqlite3 /opt/dbspace/.dnet.db < /usr/share/ps-plugin/dnet_db_init.sql
fi
if [ ! -f /opt/dbspace/.dnet2.db ]
then
  sqlite3 /opt/dbspace/.dnet2.db < /usr/share/ps-plugin/dnet_db.sql
  sqlite3 /opt/dbspace/.dnet2.db < /usr/share/ps-plugin/dnet_db_init.sql
fi

rm -f /usr/share/ps-plugin/dnet_db.sql

#change file permission
if [ -f /opt/dbspace/.dnet.db ]
then
	chmod 660 /opt/dbspace/.dnet.db
fi

if [ -f /opt/dbspace/.dnet.db-journal ]
then
	chmod 664 /opt/dbspace/.dnet.db-journal
fi

if [ -f /usr/lib/rpm-plugins/msm.so ]
then
	chsmack -a 'telephony_framework::db' /opt/dbspace/.dnet.db*
fi

if [ -f /etc/opt/upgrade/520.tel-plugin-packetservice.patch.sh ]
then
	chmod 700 /etc/opt/upgrade/520.tel-plugin-packetservice.patch.sh
fi

%postun -p /sbin/ldconfig

%install
%make_install
mkdir -p %{buildroot}%{_datadir}/license

%files
%manifest tel-plugin-packetservice.manifest
%defattr(-,root,root,-)
/opt/etc/dump.d/module.d/dump_packetservice.sh
#%doc COPYING
%{_datadir}/ps-plugin/dnet_db.sql
%{_datadir}/ps-plugin/dnet_db_init.sql
%{_datadir}/ps-plugin/apns-conf.xml
%{_sysconfdir}/opt/upgrade/*
%{_libdir}/telephony/plugins/ps-plugin*
%{_datadir}/license/tel-plugin-packetservice
