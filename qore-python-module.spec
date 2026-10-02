# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
%global source_date_epoch_from_changelog 1
%global use_source_date_epoch_as_buildtime 1
%if v"%{rpmversion}" >= v"4.20"
%global build_mtime_policy clamp_to_source_date_epoch
%else
%global clamp_mtime_to_source_date_epoch 1
%endif
%bcond_without tests
%bcond_without docs
%global python_version %(/usr/bin/python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
%global python_sitearch %(/usr/bin/python3 -c 'import sysconfig; print(sysconfig.get_path("platlib", vars={"base": "/usr", "platbase": "/usr"}))')
%global python_extension_suffix %(/usr/bin/python3 -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')
Name: qore-python-module
Version: 1.3.0
Release: 2%{?dist}
Summary: Bidirectional Qore and Python integration
License: LGPL-2.1-or-later AND MIT AND Python-2.0
URL: https://github.com/qoretechnologies/module-python
Source0: %{name}-%{version}.tar.xz
BuildRequires: cmake >= 3.12
BuildRequires: make
BuildRequires: gcc-c++
BuildRequires: python3-devel >= 3.7
BuildRequires: qore-devel >= 3.0.0~
BuildRequires: qore-rpm-macros >= 3.0.0~
%if %{with tests}
BuildRequires: qore-xml-module >= 2.3.0
%endif
%if %{with docs}
BuildRequires: doxygen
%if 0%{?suse_version}
BuildRequires: util-linux
%else
BuildRequires: util-linux-core
%endif
%endif
# The bridge embeds the full CPython ABI, not the limited/stable ABI.
Requires: python(abi) = %{python_version}

%description
Run Python functions and classes from Qore, or import Qore APIs from Python
using qoreloader. Includes the native bridge, the versioned Python extension
alias and compiler metadata. The package uses the distribution's default
Python interpreter and library.

%if %{with docs}
%package doc
Summary: Qore Python integration reference documentation
BuildArch: noarch
%description doc
API reference and examples for bidirectional Qore and Python integration.
%endif

%prep
%autosetup
%build
%{?set_build_flags}
. %{_rpmconfigdir}/qore/module-env.sh
unset PYTHONPATH PYTHONHOME Python3_ROOT Python3_LIBRARIES Python3_INCLUDE_DIRS Python3_EXECUTABLE
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
qore_set_source_prefix_maps "%{qore_debug_source_dir}"
cmake -S . -B build -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-DNDEBUG \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DCMAKE_SKIP_RPATH=ON -DCMAKE_IGNORE_PREFIX_PATH=/usr/local \
  -DQore_DIR=%{_libdir}/cmake/Qore -DQORE_EXECUTABLE=/usr/bin/qore \
  -DQORE_QPP_EXECUTABLE=/usr/bin/qpp \
  -DPython3_EXECUTABLE=/usr/bin/python3 -DPython3_ROOT_DIR=/usr \
  -DQORE_PYTHON_STRICT_DOCS=ON -DQORE_GENERATE_JAVA_BINDINGS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=%{!?with_docs:ON}%{?with_docs:OFF}
cmake --build build -- %{?_smp_mflags}
%if %{with docs}
cmake --build build --target docs -- %{?_smp_mflags}
%endif
%install
DESTDIR=%{buildroot} cmake --install build
chmod 755 %{buildroot}%{_libdir}/qore-modules/python-api-*.qmod
install -d %{buildroot}%{python_sitearch}
module_file="%{_libdir}/qore-modules/python-api-$(/usr/bin/qore --latest-module-api).qmod"
relative=$(/usr/bin/python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$module_file" '%{python_sitearch}')
ln -s "$relative" '%{buildroot}%{python_sitearch}/qoreloader%{python_extension_suffix}'
%if %{with docs}
install -d %{buildroot}%{_docdir}/%{name}-doc
cp -a build/docs/python/html %{buildroot}%{_docdir}/%{name}-doc/
hardlink -t -O %{buildroot}%{_docdir}/%{name}-doc
%endif
%check
%if %{with tests}
. %{_rpmconfigdir}/qore/module-env.sh
/usr/bin/python3 -B -W error rpm/test_fixture.py -v
/usr/bin/python3 -B -W error rpm/run-tests.py --build-dir "$PWD/build"
%endif
%files
%license COPYING.LGPL COPYING.Python
%doc README.md
%{_libdir}/qore-modules/python-api-*.qmod
%{python_sitearch}/qoreloader%{python_extension_suffix}
%dir %{_datadir}/qore/metadata/python
%{_datadir}/qore/metadata/python/*.meta.json
%if %{with docs}
%files doc
%license COPYING.LGPL COPYING.Python
%doc %{_docdir}/%{name}-doc/
%endif
%changelog
* Fri Oct 02 2026 David Nichols <david@qore.org> - 1.3.0-2
- Package the bidirectional bridge, Python ABI alias and compiler metadata.
- Run isolated embedded and standalone suites and build strict API documentation.
