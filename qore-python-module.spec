%{?_datarootdir: %global mydatarootdir %_datarootdir}
%{!?_datarootdir: %global mydatarootdir %{buildroot}/usr/share}

%global module_api %(qore --latest-module-api 2>/dev/null)
%global module_dir %{_libdir}/qore-modules
%global user_module_dir %{mydatarootdir}/qore-modules/

Name:           qore-python-module
Version:        1.2.1
Release:        1
Summary:        Qorus Integration Engine - Qore Python module
License:        MIT
Group:          Productivity/Networking/Other
Url:            https://qoretechnologies.com
Source:         qore-python-module-%{version}.tar.bz2
BuildRequires:  gcc-c++
%if 0%{?el7}
BuildRequires:  devtoolset-7-gcc-c++
%endif
BuildRequires:  cmake >= 3.5
%if 0%{?suse_version} || 0%{?fedora} || 0%{?sles_version} || 0%{?el9}
BuildRequires:  python3-devel >= 3.8
Requires:       python3 >= 3.8
%else
%if 0%{?redhat} || 0%{?centos}
BuildRequires:  python38-devel
Requires:       python38
%endif
%endif
BuildRequires:  qore-devel >= 1.12.4
BuildRequires:  qore-stdlib >= 1.12.4
BuildRequires:  qore >= 1.12.4
BuildRequires:  doxygen
Requires:       qore-module(abi)%{?_isa} = %{module_api}
Requires:       %{_bindir}/env
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
This package contains the Python module for the Qore Programming Language.

%prep
%setup -q

%build
%if 0%{?el7}
# enable devtoolset7
. /opt/rh/devtoolset-7/enable
unset PYTHONPATH
%endif
export CXXFLAGS="%{?optflags}"
alias python=/usr/bin/python3
cmake -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_BUILD_TYPE=RELWITHDEBINFO -DCMAKE_SKIP_RPATH=1 -DCMAKE_SKIP_INSTALL_RPATH=1 -DCMAKE_SKIP_BUILD_RPATH=1 -DCMAKE_PREFIX_PATH=${_prefix}/lib64/cmake/Qore .
make %{?_smp_mflags}
make %{?_smp_mflags} docs
sed -i 's/#!\/usr\/bin\/env qore/#!\/usr\/bin\/qore/' test/*.qtest

%install
make DESTDIR=%{buildroot} install %{?_smp_mflags}

%files
%{module_dir}

%check
qore -l ./python-api-%{module_api}.qmod test/python.qtest -v

%package doc
Summary: python module for Qore
Group: Development/Languages/Other

%description doc
python module for the Qore Programming Language.

This RPM provides API documentation, test and example programs

%files doc
%defattr(-,root,root,-)
%doc docs/python test/*.qtest

%changelog
* Sun Dec 29 2024 David Nichols <david@qore.org>
- updated to version 1.2.1
- fixed a thread-safety race condition in qpy_deregister()
- fixed an incorrect assertion in getQoreDateTimeFromTime()
- removed a debug printf() statement left in callPythonMethod()
- fixed a typo in an error message

* Sun Dec 25 2022 David Nichols <david@qore.org>
- updated to version 1.2.0

* Tue Dec 20 2022 David Nichols <david@qore.org>
- updated to version 1.1.7

* Tue Dec 6 2022 David Nichols <david@qore.org>
- updated to version 1.1.6

* Sat Dec 3 2022 David Nichols <david@qore.org>
- updated to version 1.1.5

* Wed Jan 26 2022 David Nichols <david@qore.org>
- updated to version 1.1.4

* Thu Jan 20 2022 David Nichols <david@qore.org>
- updated to version 1.1.3

* Tue Jan 18 2022 David Nichols <david@qore.org>
- updated to version 1.1.2

* Sun Oct 31 2021 David Nichols <david@qore.org>
- updated to version 1.1.1

* Wed Oct 27 2021 David Nichols <david@qore.org>
- updated to version 1.1.0

* Tue Oct 12 2021 David Nichols <david@qore.org>
- updated to version 1.0.7

* Fri Oct 8 2021 David Nichols <david@qore.org>
- updated to version 1.0.6

* Tue Oct 5 2021 David Nichols <david@qore.org>
- updated to version 1.0.5

* Sat Oct 2 2021 David Nichols <david@qore.org>
- updated to version 1.0.4

* Sun Aug 15 2021 David Nichols <david@qore.org>
- initial version
