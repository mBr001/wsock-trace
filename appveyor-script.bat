@echo off

if %1. == init.  goto init
if %1. == clean. goto clean

echo Usage: %0 "init / clean"
exit /b 0

:init
::
:: The CPU agnostic init-stage.
::
echo Generating src\wsock_trace.appveyor...
echo #                                               > src\wsock_trace.appveyor
echo # This file was generated from %0.             >> src\wsock_trace.appveyor
echo #                                              >> src\wsock_trace.appveyor
echo [core]                                         >> src\wsock_trace.appveyor
echo trace_level            = %%WSOCK_TRACE_LEVEL%% >> src\wsock_trace.appveyor
echo trace_indent           = 2                     >> src\wsock_trace.appveyor
echo trace_caller           = 1                     >> src\wsock_trace.appveyor
echo trace_report           = %%WSOCK_TRACE_LEVEL%% >> src\wsock_trace.appveyor
echo trace_time             = relative              >> src\wsock_trace.appveyor
echo trace_max_len          = %%COLUMNS%%           >> src\wsock_trace.appveyor
echo callee_level           = 1                     >> src\wsock_trace.appveyor
echo cpp_demangle           = 1                     >> src\wsock_trace.appveyor
echo short_errors           = 1                     >> src\wsock_trace.appveyor
echo use_full_path          = 1                     >> src\wsock_trace.appveyor
echo use_toolhlp32          = 1                     >> src\wsock_trace.appveyor
echo dump_modules           = 0                     >> src\wsock_trace.appveyor
echo dump_select            = 1                     >> src\wsock_trace.appveyor
echo dump_hostent           = 1                     >> src\wsock_trace.appveyor
echo dump_protoent          = 1                     >> src\wsock_trace.appveyor
echo dump_servent           = 1                     >> src\wsock_trace.appveyor
echo dump_nameinfo          = 1                     >> src\wsock_trace.appveyor
echo dump_wsaprotocol_info  = 1                     >> src\wsock_trace.appveyor
echo dump_wsanetwork_events = 1                     >> src\wsock_trace.appveyor
echo dump_data              = 1                     >> src\wsock_trace.appveyor
echo max_data               = 5000                  >> src\wsock_trace.appveyor
echo max_displacement       = 1000                  >> src\wsock_trace.appveyor
echo exclude                = htons,htonl,inet_addr >> src\wsock_trace.appveyor
echo hosts_file             = %CD%\hosts            >> src\wsock_trace.appveyor
echo [geoip]                                        >> src\wsock_trace.appveyor
echo enable                 = 1                     >> src\wsock_trace.appveyor
echo use_generated          = 0                     >> src\wsock_trace.appveyor
echo max_days               = 10                    >> src\wsock_trace.appveyor
echo geoip4_file            = %CD%\geoip            >> src\wsock_trace.appveyor
echo geoip6_file            = %CD%\geoip6           >> src\wsock_trace.appveyor
echo ip2location_bin_file   = %CD%\IP4-COUNTRY.BIN  >> src\wsock_trace.appveyor
echo [idna]                                         >> src\wsock_trace.appveyor
echo enable                 = 1                     >> src\wsock_trace.appveyor

echo Generating hosts file...
echo #                                               > hosts
echo # This file was generated from %0.             >> hosts
echo #                                              >> hosts
echo 127.0.0.1   localhost                          >> hosts
echo ::1         localhost                          >> hosts
echo 127.0.0.1   mpa.one.microsoft.com              >> hosts
echo 8.8.8.8     google-public-dns-a.google.com     >> hosts
echo #                                              >> hosts
echo # This hostname is used in test.exe            >> hosts
echo # check that it prints "from 'hosts' file".    >> hosts
echo #                                              >> hosts
echo 10.0.0.20   www.no-such-host.com               >> hosts

::
:: These should survive until 'msvc' + 'mingw' + 'cygwin' gets run.
::
set WSOCK_TRACE=%CD%\src\wsock_trace.appveyor
set WSOCK_TRACE_LEVEL=2
set COLUMNS=120
exit /b 0

::
:: Cleanup after a local 'appveyor-script <builder>'.
:: This is not used by AppVeyor itself (not refered in appveyor.yml).
::
:clean
del /Q IP4-COUNTRY.BIN xz.exe src\wsock_trace.appveyor hosts 2> NUL
echo Cleaning done.
exit /b 0
