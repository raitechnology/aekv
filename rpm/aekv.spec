Name:		aekv
Version:	999.999
Vendor:	        Rai Technology, Inc
Release:	99999%{?dist}
Summary:	Aeron distribution

License:	ASL 2.0
URL:		https://github.com/raitechnology/%{name}
Source0:	%{name}-%{version}-99999.tar.gz
BuildRoot:	${_tmppath}
Prefix:	        /usr
BuildRequires:  gcc-c++
BuildRequires:  chrpath
BuildRequires:  raikv
BuildRequires:  pcre2-devel
BuildRequires:  openssl-devel
BuildRequires:  git-core
BuildRequires:  liblzf-devel
BuildRequires:  systemd
BuildRequires:  zlib-devel
BuildRequires:  hdrhist
BuildRequires:  aeron
Requires:       raikv
Requires:       pcre2
Requires:       openssl
Requires:       liblzf
Requires:       zlib
Requires:       hdrhist
Requires:       aeron
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
Rai backing store

%prep
%setup -q


%define _unpackaged_files_terminate_build 0
%define _missing_doc_files_terminate_build 0
%define _missing_build_ids_terminate_build 0
%define _include_gdb_index 1

%build
make build_dir=./usr %{?_smp_mflags} dist_bins
cp -a ./include ./usr/include

%install
rm -rf %{buildroot}
mkdir -p  %{buildroot}

# in builddir
cp -a * %{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
/usr/bin/*
/usr/lib64/*
/usr/include/*

%post
echo "${RPM_INSTALL_PREFIX}/lib64" > /etc/ld.so.conf.d/%{name}.conf
/sbin/ldconfig

%postun
if [ $1 -eq 0 ] ; then
rm -f /etc/ld.so.conf.d/%{name}.conf
fi
/sbin/ldconfig

%changelog
* __DATE__ <support@raitechnology.com>
- Hello world
