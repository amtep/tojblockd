Name:       tojblockd
Summary:    Serve directory trees as VFAT block devices
Version:    1.0
Release:    1
Group:      System/Daemons
License:    GPLv2+
URL:        https://github.com/amtep/tojblockd
Source0:    %{name}-%{version}.tar.gz
# Qt5 is only needed for the autotests
BuildRequires: qt5-qmake
BuildRequires: pkgconfig(Qt5Test)

%description
A service process that presents a directory tree as a VFAT block device.
It uses the network block device driver and by default uses /dev/nbd0.

%package tests
Summary: Unit tests for %{name}
Requires: %{name} = %{version}-%{release}
Requires: qt5-qttest

%description tests
Unit tests for %{name}

%prep
%setup -q

%build
make %{?jobs:-j%jobs} tojblockd
cd tests
%qmake5
make %{?jobs:-j%jobs}

%install
install -m755 -D tojblockd %{buildroot}/%{_sbindir}/tojblockd
install -m755 -d %{buildroot}/opt/tests/%{name}
install -m755 tests/*/test-* %{buildroot}/opt/tests/%{name}/
install -m644 -D tests/tests.xml %{buildroot}/opt/tests/%{name}/test-definition/tests.xml
install -m 644 -D systemd/tojblockd@.service %{buildroot}/lib/systemd/system/tojblockd@.service

%post
systemctl --system daemon-reload

%files
%defattr(-,root,root,-)
%doc COPYING
%{_sbindir}/tojblockd
/lib/systemd/system/tojblockd@.service

%files tests
%defattr(-,root,root,-)
/opt/tests/%{name}/*
