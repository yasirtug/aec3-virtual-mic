@echo off
setlocal

rem Builds AEC3VirtualMic.exe inside a WebRTC checkout and copies it to bin\.
rem
rem Environment overrides:
rem   AEC3_DEPS_DIR   where scripts\fetch_webrtc.bat put its checkout (default: <repo>\.deps)
rem   WEBRTC_ROOT     WebRTC "src" directory (default: %AEC3_DEPS_DIR%\webrtc\src)
rem   DEPOT_TOOLS     depot_tools directory   (default: %AEC3_DEPS_DIR%\depot_tools)
rem   vs2026_install / vs2022_install
rem                   Visual Studio directory; auto-detected with vswhere when unset.

set "PROJECT_DIR=%~dp0"
if not defined AEC3_DEPS_DIR set "AEC3_DEPS_DIR=%PROJECT_DIR%.deps"
if not defined WEBRTC_ROOT set "WEBRTC_ROOT=%AEC3_DEPS_DIR%\webrtc\src"
if not defined DEPOT_TOOLS set "DEPOT_TOOLS=%AEC3_DEPS_DIR%\depot_tools"
set "STAGE_DIR=%WEBRTC_ROOT%\examples\aec3_virtual_mic"
set "OUT_DIR=%WEBRTC_ROOT%\out\aec3_virtual_mic"
set "PATH=%DEPOT_TOOLS%;%PATH%"
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"
set "DEPOT_TOOLS_UPDATE=0"

if not exist "%WEBRTC_ROOT%\.gn" (
  echo WebRTC source was not found at "%WEBRTC_ROOT%".
  echo Run scripts\fetch_webrtc.bat first, or set WEBRTC_ROOT to an existing checkout.
  exit /b 1
)
if not exist "%DEPOT_TOOLS%\python3.bat" (
  echo depot_tools was not found at "%DEPOT_TOOLS%".
  echo Run scripts\fetch_webrtc.bat first, or set DEPOT_TOOLS to an existing depot_tools.
  exit /b 1
)
if not exist "%WEBRTC_ROOT%\buildtools\win\gn.exe" (
  echo WebRTC GN was not found under "%WEBRTC_ROOT%\buildtools".
  exit /b 1
)
if not exist "%WEBRTC_ROOT%\third_party\ninja\ninja.exe" (
  echo WebRTC Ninja was not found under "%WEBRTC_ROOT%\third_party".
  exit /b 1
)

call :detect_visual_studio

rem RNNoise's generated model weights are fetched (pinned SHA-256), not
rem committed; this is a no-op when they are already present.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_DIR%scripts\fetch_rnnoise_model.ps1"
if errorlevel 1 exit /b 1

rem Stage the project (BUILD.gn plus src\, assets\ and third_party\). The
rem stage directory is recreated so files deleted from the repository do not
rem linger; copies keep their timestamps, so Ninja only rebuilds what changed.
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
if exist "%STAGE_DIR%" (
  echo Could not clean "%STAGE_DIR%". Close programs using it and retry.
  exit /b 1
)
mkdir "%STAGE_DIR%"
if errorlevel 1 exit /b 1
copy /Y "%PROJECT_DIR%BUILD.gn" "%STAGE_DIR%\" >nul
if errorlevel 1 exit /b 1
for %%D in (src assets third_party) do (
  xcopy /E /I /Y /Q "%PROJECT_DIR%%%D" "%STAGE_DIR%\%%D\" >nul || exit /b 1
)

call "%DEPOT_TOOLS%\python3.bat" "%PROJECT_DIR%scripts\configure_webrtc.py" "%WEBRTC_ROOT%"
if errorlevel 1 exit /b 1

"%WEBRTC_ROOT%\buildtools\win\gn.exe" gen "%OUT_DIR%" --root="%WEBRTC_ROOT%" --args="is_debug=false symbol_level=1 rtc_include_tests=false rtc_build_examples=true rtc_build_tools=false use_siso=false target_cpu=\"x64\" treat_warnings_as_errors=false"
if errorlevel 1 exit /b 1

"%WEBRTC_ROOT%\third_party\ninja\ninja.exe" -C "%OUT_DIR%" aec3_virtual_mic
if errorlevel 1 exit /b 1

if not exist "%PROJECT_DIR%bin" mkdir "%PROJECT_DIR%bin"
copy /Y "%OUT_DIR%\aec3_virtual_mic.exe" "%PROJECT_DIR%bin\AEC3VirtualMic.exe" >nul
if errorlevel 1 exit /b 1

echo Built "%PROJECT_DIR%bin\AEC3VirtualMic.exe"
endlocal
exit /b 0

rem Chromium only looks for Visual Studio 2022/2026 under %ProgramFiles%.
rem Point it at the newest installation with the C++ x64 tools (for example
rem Build Tools under Program Files (x86)) unless the caller already chose one.
:detect_visual_studio
if defined vs2026_install goto :eof
if defined vs2022_install goto :eof
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :eof
set "VS_PATH="
set "VS_MAJOR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%I"
for /f "usebackq tokens=1 delims=." %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VS_MAJOR=%%I"
if not defined VS_PATH goto :eof
if "%VS_MAJOR%"=="18" set "vs2026_install=%VS_PATH%"
if "%VS_MAJOR%"=="17" set "vs2022_install=%VS_PATH%"
if defined vs2026_install echo Using Visual Studio 2026 at "%vs2026_install%".
if defined vs2022_install echo Using Visual Studio 2022 at "%vs2022_install%".
goto :eof
