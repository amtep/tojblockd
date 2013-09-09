Name:       tojblockd
Summary:    Serve directory trees as VFAT block devices
Version:    1.0
Release:    1
Group:      System/Daemons
License:    GPLv2+
Source0:    %{name}-%{version}.tar.gz
Requires:   modules(nbd)

%description
A service process that presents a directory tree as a VFAT block device.
It uses the network block device driver and by default uses /dev/nbd0.

%prep
%setup -q

%build
make %{?jobs:-j%jobs} tojblockd

%install
install -m755 -D tojblockd %{buildroot}/%{_sbindir}/tojblockd

%files
%defattr(-,root,root,-)
%doc COPYING
%{_sbindir}/tojblockd
