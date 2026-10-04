@echo off
rem Single build/test entry point. Usage: tools\build.cmd [preset] [configure^|build^|test^|all] [extra ctest args...]
rem   preset: debug (default) ^| release ^| fuzz
rem   action: all (default) = configure + build + test
rem   fuzz preset: "test" runs the parser fuzzer; extra arg = seconds, e.g. tools\build.cmd fuzz all 120
setlocal EnableExtensions

rem Capture the repo root before any shift (shift also moves %0).
set "REPO_ROOT=%~dp0.."

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"
set "ACTION=%~2"
if "%ACTION%"=="" set "ACTION=all"
rem Remaining arguments (3rd onward) are passed to ctest, e.g. -R Parser
set "CTEST_EXTRA="
shift
shift
:collect
if "%~1"=="" goto collected
set "CTEST_EXTRA=%CTEST_EXTRA% %1"
shift
goto collect
:collected

set "VCPKG_ROOT=D:\vcpkg"
set "VCPKG_DEFAULT_BINARY_CACHE=D:\vcpkg-cache"
set "VCPKG_DOWNLOADS=D:\vcpkg-downloads"
set "VCVARS=D:\Visual Studio Community\2022\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
  echo [build.cmd] vcvars64.bat not found: "%VCVARS%"
  exit /b 1
)
rem vcvars64.bat looks up vswhere.exe on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" >nul
if errorlevel 1 (
  echo [build.cmd] vcvars64.bat failed
  exit /b 1
)

cd /d "%REPO_ROOT%"

if /i "%ACTION%"=="configure" goto configure
if /i "%ACTION%"=="build" goto build
if /i "%ACTION%"=="test" goto test
if /i "%ACTION%"=="all" goto configure
echo [build.cmd] unknown action "%ACTION%" (expected configure, build, test or all)
exit /b 2

:configure
echo [build.cmd] cmake --preset %PRESET%
cmake --preset %PRESET%
if errorlevel 1 exit /b 1
if /i "%ACTION%"=="configure" exit /b 0

:build
echo [build.cmd] cmake --build --preset %PRESET%
cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1
if /i "%ACTION%"=="build" exit /b 0

:test
if /i "%PRESET%"=="fuzz" goto fuzz
echo [build.cmd] ctest --preset %PRESET%%CTEST_EXTRA%
ctest --preset %PRESET%%CTEST_EXTRA%
if errorlevel 1 exit /b 1
exit /b 0

:fuzz
rem For the fuzz preset the "test" step runs the parser fuzzer; the extra argument is seconds (default 60).
set "FUZZ_SECONDS=60"
for /f %%s in ("%CTEST_EXTRA%") do set "FUZZ_SECONDS=%%s"
echo [build.cmd] fuzzing lb_parser_fuzz for %FUZZ_SECONDS% s
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\tools\fuzz.ps1" ^
  -Exe "%REPO_ROOT%\build\fuzz\tests\lb_parser_fuzz.exe" -Seconds %FUZZ_SECONDS% ^
  -WorkDir "%REPO_ROOT%\build\fuzz\run" -CorpusDir "%REPO_ROOT%\tests\corpus\reject"
if errorlevel 1 exit /b 1
exit /b 0
