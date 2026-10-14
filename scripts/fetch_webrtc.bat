@echo off
setlocal

rem Fetches depot_tools and WebRTC at the revision pinned in WEBRTC_REVISION.
rem The checkout is shallow (no git history) and skips WebRTC's test resources.
rem
rem Layout (AEC3_DEPS_DIR defaults to <repo>\.deps):
rem   %AEC3_DEPS_DIR%\depot_tools\   shallow depot_tools clone
rem   %AEC3_DEPS_DIR%\webrtc\src\    WebRTC checkout (build.bat's default WEBRTC_ROOT)
rem
rem The checkout is owned by this script: re-running it resets local changes in
rem the WebRTC tree (build.bat re-applies its own patches on every build).
rem Requires Git for Windows on PATH and about 10 GB of free disk space.

for %%I in ("%~dp0..") do set "PROJECT_DIR=%%~fI"
if not defined AEC3_DEPS_DIR set "AEC3_DEPS_DIR=%PROJECT_DIR%\.deps"
set "DEPOT_TOOLS=%AEC3_DEPS_DIR%\depot_tools"
set "GCLIENT_ROOT=%AEC3_DEPS_DIR%\webrtc"
set "DEPOT_TOOLS_URL=https://chromium.googlesource.com/chromium/tools/depot_tools.git"
set "WEBRTC_URL=https://webrtc.googlesource.com/src.git"

set "WEBRTC_REVISION="
for /f "usebackq tokens=1" %%R in ("%PROJECT_DIR%\WEBRTC_REVISION") do if not defined WEBRTC_REVISION set "WEBRTC_REVISION=%%R"
if not defined WEBRTC_REVISION (
  echo WEBRTC_REVISION is missing or empty.
  exit /b 1
)

where git >nul 2>&1
if errorlevel 1 (
  echo Git was not found on PATH. Install Git for Windows first.
  exit /b 1
)

if not exist "%AEC3_DEPS_DIR%" mkdir "%AEC3_DEPS_DIR%"
if errorlevel 1 exit /b 1

if exist "%DEPOT_TOOLS%\gclient.bat" goto :bootstrap
echo Cloning depot_tools...
git clone --depth 1 "%DEPOT_TOOLS_URL%" "%DEPOT_TOOLS%"
if errorlevel 1 exit /b 1

:bootstrap
rem Install depot_tools' CIPD tools and bundled Python once, then pin
rem depot_tools: later gclient calls must not self-update the shallow clone.
echo Bootstrapping depot_tools...
call "%DEPOT_TOOLS%\cipd_bin_setup.bat"
if errorlevel 1 exit /b 1
call "%DEPOT_TOOLS%\bootstrap\win_tools.bat"
if errorlevel 1 exit /b 1

set "PATH=%DEPOT_TOOLS%;%PATH%"
set "DEPOT_TOOLS_UPDATE=0"
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"

if not exist "%GCLIENT_ROOT%" mkdir "%GCLIENT_ROOT%"
if errorlevel 1 exit /b 1
pushd "%GCLIENT_ROOT%"
if errorlevel 1 exit /b 1

if exist ".gclient" goto :sync
call gclient config --unmanaged --name=src "%WEBRTC_URL%"
if errorlevel 1 goto :fail

:sync
echo Syncing WebRTC %WEBRTC_REVISION% (no history, no hooks)...
call gclient sync --nohooks --no-history --reset --revision "src@%WEBRTC_REVISION%"
if errorlevel 1 goto :fail

rem Run only the DEPS hooks a Windows build needs. Skipped on purpose:
rem   - the unnamed test-resources hook (downloads gigabytes of media files),
rem   - win_toolchain (no-op with DEPOT_TOOLS_WIN_TOOLCHAIN=0; local VS is used),
rem   - test metadata, vpython, Siso/reclient configuration and non-Windows hooks.
echo Running required WebRTC hooks...
call python3 src\build\landmines.py --landmine-scripts src\tools_webrtc\get_landmines.py --src-dir src
if errorlevel 1 goto :fail
call python3 src\build\util\lastchange.py -o src\build\util\LASTCHANGE
if errorlevel 1 goto :fail
call download_from_google_storage --no_resume --bucket chromium-browser-clang/rc -s src\build\toolchain\win\rc\win\rc.exe.sha1
if errorlevel 1 goto :fail

popd
echo WebRTC %WEBRTC_REVISION% is ready in "%GCLIENT_ROOT%\src".
exit /b 0

:fail
set "FETCH_ERROR=%ERRORLEVEL%"
popd
if "%FETCH_ERROR%"=="0" set "FETCH_ERROR=1"
exit /b %FETCH_ERROR%
