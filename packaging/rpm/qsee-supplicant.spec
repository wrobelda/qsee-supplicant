Name:           qsee-supplicant
Version:        0.1.1
Release:        1%{?dist}
Summary:        Userspace services for Qualcomm QSEECOM trusted applications

License:        BSD-2-Clause AND BSD-3-Clause-Clear
URL:            https://github.com/wrobelda/qsee-supplicant
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-rpm-macros
%{?systemd_requires}

%description
Provides a machine-wide listener supplicant and a per-application loader for
trusted applications using Qualcomm's legacy QSEECOM interface through the
Linux TEE subsystem.

%prep
%autosetup

%build
%make_build

%check
%make_build check

%install
%{__make} DESTDIR=%{buildroot} PREFIX=%{_prefix} SBINDIR=%{_sbindir} \
           UNITDIR=%{_unitdir} DOCDIR=%{_docdir}/%{name} \
           install-bin install-systemd install-doc

%post
%systemd_post qsee-supplicant.service qsee-app-loader@.service

%preun
%systemd_preun qsee-supplicant.service qsee-app-loader@.service

%postun
%systemd_postun_with_restart qsee-supplicant.service qsee-app-loader@.service

%files
%license LICENSE LICENSES/BSD-3-Clause-Clear.txt
%{_sbindir}/qsee-supplicant
%{_sbindir}/qsee-app-loader
%{_unitdir}/qsee-supplicant.service
%{_unitdir}/qsee-app-loader@.service
%{_docdir}/%{name}/README.md

%changelog
* Mon Aug 10 2026 Dawid Wróbel <me@dawidwrobel.com> - 0.1.1-1
- Add Alpine and portable archive release formats.

* Mon Aug 10 2026 Dawid Wróbel <me@dawidwrobel.com> - 0.1.0-1
- Initial release.
