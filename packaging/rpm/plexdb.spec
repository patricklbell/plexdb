Name:           plexdb
Version:        %{plexdb_version}
Release:        1%{?dist}
Summary:        PlexDB CQL and key-value database server
License:        GPL-3.0
URL:            https://github.com/plexdb/plexdb
BuildArch:      %{plexdb_arch}

%description
PlexDB CQL and key-value database server binaries.

%install
rm -rf %{buildroot}
install -Dm755 %{_sourcedir}/plexdb_cql_server        %{buildroot}%{_bindir}/plexdb_cql_server
install -Dm755 %{_sourcedir}/plexdb_keyvalue_server   %{buildroot}%{_bindir}/plexdb_keyvalue_server
install -Dm755 %{_sourcedir}/libplexdb_otel_plugin.so %{buildroot}%{_libdir}/plexdb/plugins/libplexdb_otel_plugin.so

%files
%caps(cap_ipc_lock=ep) %attr(0755,root,root) %{_bindir}/plexdb_cql_server
%attr(0755,root,root) %{_bindir}/plexdb_keyvalue_server
%{_libdir}/plexdb/plugins/libplexdb_otel_plugin.so

%changelog
* Mon Jan 01 2024 PlexDB release automation <noreply@plexdb> - 0-1
- Automated packaging.
