%define name MonetDB
%define major_version 4
%define minor_version 3
%define sublevel 7
%define release 1
%define version %{major_version}.%{minor_version}.%{sublevel}
%define prefix /usr

%define monetdb_uid    85
%define monetdb_gid    85

Name: %{name}
Version: %{version}
Release: %{release}
Summary: MonetDB - Monet Database Management System
Group: System
Source: %{name}-%{version}.tar.gz
BuildRoot: /var/tmp/%{name}-root
Packager: Niels Nes <Niels.Nes@cwi.nl>
Copyright:   MPL - http://monetdb.cwi.nl/Legal/MonetDBLicense-1.0.html

%package client
Summary: MonetDB clients
Group: System

%package server
Summary: MonetDB server 
Group: System 

%package devel
Summary: MonetDB development package 
Group: System 

%description
MonetDB is a database management system that is developed from a
main-memory perspective with use of a fully decomposed storage model,
automatic index management, extensibility of data types and search
accellerators, SQL- and XML- frontends.

%description client
Add the MonetDB client description here

%description server
Add the MonetDB server description here
Requires: %{name}-client

%description devel
Add the MonetDB devel description here
Requires: %{name}-server
Requires: epsffit
PreReq: /sbin/chkconfig, /sbin/service, sh-utils
PreReq: %{_sbindir}/groupadd, %{_sbindir}/useradd


%prep
rm -rf $RPM_BUILD_ROOT

%setup -q -n %{name}-%{version}

%build

./configure --prefix=%{prefix} 

make

%install
rm -rf $RPM_BUILD_ROOT

make install \
	DESTDIR=$RPM_BUILD_ROOT 

find $RPM_BUILD_ROOT -name .incs.in | xargs rm

# cleanup stuff we don't want to install
rm -rf $RPM_BUILD_ROOT%{prefix}/bin/epsffit
rm -rf $RPM_BUILD_ROOT%{prefix}/share/MonetDB/conf/conf.bash
rm -rf $RPM_BUILD_ROOT%{prefix}/share/MonetDB/general.mil
rm -rf $RPM_BUILD_ROOT%{prefix}/share/MonetDB/lady.gif

# Fixes monet config script
#perl -p -i -e "s|$RPM_BUILD_ROOT||" $RPM_BUILD_ROOT%{prefix}/bin/monet_config

%clean
rm -fr $RPM_BUILD_ROOT

%files client
%defattr(-,monetdb,monetdb) 
%{prefix}/bin/MapiClient* 
%{prefix}/lib/libmutils.*
%{prefix}/lib/libMapi.* 
%{prefix}/lib/libstream.*
%{prefix}/lib/MonetDB/*jar

%{prefix}/share/MonetDB/site_perl/* 
%{prefix}/share/MonetDB/python/* 

%{prefix}/etc/monet.conf 

%files server
%defattr(-,monetdb,monetdb) 
%{prefix}/bin/Mserver 

%{prefix}/lib/libbat.*
%{prefix}/lib/libmonet.*
%{prefix}/lib/MonetDB/*.so* 
%{prefix}/lib/MonetDB/*.la* 

%{prefix}/share/MonetDB/mapiserver.mil 
%{prefix}/share/MonetDB/tools/* 

%files devel
%defattr(-,monetdb,monetdb) 
%{prefix}/bin/monet-config*
%{prefix}/share/MonetDB/conf/monet.m4 

%{prefix}/include/*.h
%{prefix}/include/*/*.[hcm]

%{prefix}/bin/mel
%{prefix}/bin/calibrator
%{prefix}/bin/Mx 
%{prefix}/bin/prefixMxFile 
%{prefix}/bin/idxmx 

%{prefix}/share/MonetDB/Mprofile-commands.lst 
%{prefix}/share/MonetDB/quit.mil 

%{prefix}/bin/prof.py*
%{prefix}/bin/Mtest.py*
%{prefix}/bin/Mapprove.py*
%{prefix}/bin/Mfilter.py*
%{prefix}/bin/Mprofile.py*
%{prefix}/bin/Mlog*
%{prefix}/bin/Mdiff*
%{prefix}/bin/Mtimeout*
%{prefix}/bin/MkillUsers*
%{prefix}/bin/Mshutdown.py*

%{prefix}/lib/autogen/* 

%pre 
%{_sbindir}/groupadd -g %{monetdb_gid} -r monetdb 2>/dev/null || :
%{_sbindir}/useradd -d %{_var}/lib/monetdb -s /bin/true -g monetdb -M -r -u %{monetdb_uid} monetdb 2>/dev/null || :

%post server
umask 022

#create monetdb init script
#/sbin/chkconfig --add monetdb

