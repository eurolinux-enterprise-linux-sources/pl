Summary:	SWI-Prolog - Edinburgh compatible Prolog compiler
Name:		pl
Version: 	4.0.0
Release:	35
Copyright:	GPL-2
Source:		ftp://swi.psy.uva.nl/pub/SWI-Prolog/pl-%{version}.tar.gz
Vendor:		Jan Wielemaker <jan@swi.psy.uva.nl>
Url:		http://www.swi.psy.uva.nl/projects/SWI-Prolog/
Packager:	Tony Nugent <Tony.Nugent@usq.edu.au>
Group:		Development/Languages
Prefix:		/usr
BuildRoot:	/var/tmp/pl
Provides:	pl-%{version}

%description
ISO/Edinburgh-style Prolog compiler including modules, autoload, libraries,
Garbage-collector, stack-expandor, C/C++-interface, GNU-readline interface,
very fast compiler.

This is the CORE RELEASE, containing ONLY the Prolog compiler.  The full
release contains packages for process handling, TCP/IP, XML/SGML, C++
binding and graphical support.

%prep
%setup

%build
env CFLAGS="$RPM_OPT_FLAGS" \
  ./configure --prefix=/usr
make

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/usr
make install prefix=$RPM_BUILD_ROOT/usr bindir=$RPM_BUILD_ROOT/usr/bin \
	man_prefix=$RPM_BUILD_ROOT/usr/man

# why are manpages installed twice?
rm -rf /usr/lib/pl-%{version}/man

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc ChangeLog README src/README.bin README.GUI COPYING
/usr/lib/pl-%{version}
/usr/man/man1/*
/usr/bin/pl*
