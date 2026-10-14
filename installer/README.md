# AEC3 Virtual Mic installer

Inno Setup 6 script that installs AEC3 Virtual Mic per machine and, when needed, the
VB-CABLE virtual audio cable by VB-Audio Software.

## Building

From the repository root, after `bin\AEC3VirtualMic.exe` has been built:

```powershell
pwsh -NoProfile -File installer\fetch_vbcable.ps1   # download + verify + extract VB-CABLE (no install)
ISCC.exe installer\AEC3VirtualMic.iss               # -> installer\Output\AEC3VirtualMic-Setup-<version>.exe
```

- `fetch_vbcable.ps1 [-Force]` downloads the pinned pack into `installer\vendor\vbcable\` (gitignored),
  checks its SHA-256 and extracts it. It does nothing when the verified pack is already extracted;
  `-Force` downloads it again. Exit codes: 0 ok, 1 download failed, 2 hash mismatch, 3 incomplete archive.
  It runs on Windows PowerShell 5.1 and PowerShell 7.
- The script reads the version from `AEC3VM_VERSION_STR` in `src\resource.h`; `ISCC /DAppVersion=x.y.z` overrides it.
- ISCC packages `bin\AEC3VirtualMic.exe`, `LICENSE` (as `LICENSE.txt`), `THIRD_PARTY_NOTICES.md`,
  `assets\app.ico` (setup icon) and the unmodified VB-CABLE pack. It never packages the Chrome echo model.
- Inno Setup isn't on `PATH` by default. `winget install JRSoftware.InnoSetup` installs it per user to
  `%LOCALAPPDATA%\Programs\Inno Setup 6\`, and Chocolatey `innosetup` installs it to
  `C:\Program Files (x86)\Inno Setup 6\`.

## What the installer does

| Situation | Behavior |
| --- | --- |
| VB-CABLE already installed | An info page says the existing VB-CABLE and its settings stay untouched. Setup doesn't install or configure it. |
| VB-CABLE missing, VAC Lite installed | A page with two options, "Use the installed VAC Lite" (default) and "Install VB-CABLE (VB-Audio donationware)". VAC Lite is detected by running the bundled exe with `--detect-cables` from Setup's temp folder (exit code bitmask: 1 = VB-CABLE, 2 = VAC Lite), i.e. with the app's own detection. |
| VB-CABLE missing (and VAC Lite missing, or "Install VB-CABLE" chosen) | The page says VB-CABLE will be installed, that it is VB-Audio donationware (all participations welcome), and links to www.vb-cable.com. Setup then installs it silently, writes `HKLM\Software\AEC3VirtualMic` `InstalledVbCable=1` (DWORD) and runs `AEC3VirtualMic.exe --configure-vbcable` elevated. It does not ask for a restart: the cable works immediately (verified on Windows 11). |
| Always | The unmodified VB-CABLE pack is kept in `{app}\vbcable`, so the uninstaller can remove VB-CABLE on request even when Setup didn't install it. |
| Task "Start AEC3 Virtual Mic with Windows" (unchecked by default) | Runs `AEC3VirtualMic.exe --enable-autostart` as the original (non-elevated) user. |
| "Launch AEC3 Virtual Mic now" (finish page) | Starts the app as the original user. |
| Uninstall | Closes a running app (mutex `AEC3VirtualMic.Running`, then `taskkill /F`). If VB-CABLE is installed, shows an "Also uninstall VB-CABLE" checkbox, checked by default only when the marker says Setup installed it (silent uninstalls use that default), and runs `VBCABLE_Setup_x64.exe -u -h` when checked. Removes the HKCU Run value `AEC3VirtualMic` and `HKCU\Software\AEC3VirtualMic` for every loaded user profile, removes `{localappdata}\AEC3VirtualMic` of the uninstalling user, deletes `HKLM\Software\AEC3VirtualMic`, and asks for a restart if VB-CABLE was removed. |

When VB-CABLE is freshly installed, the [Run] order is:

1. `AEC3VirtualMic.exe --snapshot-audio-defaults` saves the current default playback and recording
   endpoints for all roles.
2. `vbcable\VBCABLE_Setup_x64.exe -i -h` installs the driver.
3. `AEC3VirtualMic.exe --configure-vbcable` keeps the cable's endpoint formats at 48 kHz, restores the
   saved default devices if a CABLE endpoint took them over, and deletes the snapshot.

Windows may show its "Would you like to install this device software (VB-Audio Software)?"
driver-trust prompt even during a silent VB-CABLE install.

## VB-CABLE research

### Package

- The official download is `https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip`
  ("1.29 MB - OCT 2024 / XP to WIN11 32/64 bits/Arm64"). The page also says: "Extract all files and Run
  Setup Program in administrator mode. Reboot after installation / Reboot after uninstallation."
  Source: [vb-audio.com/Cable](https://vb-audio.com/Cable/).
- The pinned SHA-256 is `b950e39f01af1d04ea623c8f6d8eb9b6ea5c477c637295fabf20631c85116bfb`
  (1,318,877 bytes; server `Last-Modified: Fri, 05 Dec 2025`, downloaded 2026-10-14). The pack
  contains driver version 3.3.1.7 (`vbMmeCable64_win10.inf`, `DriverVer = 10/07/2024,3.3.1.7`).
  VB-Audio has re-uploaded files under the same name before (the `Last-Modified` date is newer than
  the pack date). If the hash check fails, verify the new pack and update the pin in `fetch_vbcable.ps1`.
- The setup program must run from the extracted files, as administrator: `VBCABLE_Setup_x64.exe` for
  64-bit Windows, including Arm64. Source:
  [VB-CABLE Reference Manual](https://vb-audio.com/Cable/VBCABLE_ReferenceManual.pdf), "Install VB-Cable"
  and pack `readme.txt`.

### Licensing (bundling)

[VB-Audio Licensing, "VB-CABLE Distribution with other product"](https://vb-audio.com/Services/licensing.htm):
VB-Audio allows distributing the VB-CABLE package with another application and embedding it in that
application's installer ("VB-CABLE silent installation"), provided the donationware model stays visible.
The end user must be able to identify VB-CABLE as a VB-Audio product, and you must mention at least:

- the origin: www.vb-cable.com
- "VB-CABLE is a donationware, all participations are welcome."

The installer's VB-CABLE page shows both. Professional deployments, where users can't donate themselves,
must buy licenses. The older `readme.txt` inside the pack says that integrating the pack into another
installer needs the author's agreement; the licensing page above is VB-Audio's current, explicit
permission.

### Silent install / uninstall

- `VBCABLE_Setup_x64.exe -i -h` installs silently and `VBCABLE_Setup_x64.exe -u -h` uninstalls silently.
  VB-Audio doesn't document these switches officially; they come from community reports:
  [gcloudrig issue #40](https://github.com/gcloudrig/gcloudrig/issues/40) ("-i -h to silence it. Also -u -h
  to uninstall") and a VB-Audio forum post, ["VB-Cable command-line options?"](https://forum.vb-audio.com/viewtopic.php?t=1764)
  ("silent install with "-i -h" and uninstall with "-u -h"").
- Even in silent mode, Windows' driver-trust prompt can appear:
  [VB-Audio forum t=1766](https://forum.vb-audio.com/viewtopic.php?t=1766).
- VB-Audio recommends a reboot after install and uninstall. In testing (Windows 11, pack 45) the cable
  endpoints were usable immediately after the silent install, so Setup doesn't ask for a restart;
  Uninstall returns `UninstallNeedRestart` after removing VB-CABLE.

### Detection

From the pack's `vbMmeCable64_win10.inf`:

- The device name is "VB-Audio Virtual Cable" and the hardware id is `VBAudioVACWDM`.
- The kernel service is `VBAudioVACMME`.
- Endpoint names are "CABLE Input" (playback), "CABLE In 16 Ch" (16-channel playback) and
  "CABLE Output" (recording). Friendly names are e.g. "CABLE Input (VB-Audio Virtual Cable)".

From strings in the pack's `VBCABLE_Setup_x64.exe` [INFERENCE]: the setup registers the uninstall key
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VB:VBCABLE {87459874-1236-4469}`.

The installer treats VB-CABLE as present if either of these is true:

- That uninstall key exists.
- A playback endpoint under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{id}`
  has hardware id `VBAudioVACWDM` (property `{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8`) and
  `DeviceState` 1 (active) or 2 (disabled).

The installer doesn't use the service key alone, because it can remain after an uninstall until reboot.
VB-CABLE A+B / C+D use different device names and aren't treated as plain VB-CABLE. The app itself
detects endpoints by name (`audio_devices.cc`).

### Recommended settings and where they live

The manual documents two settings:

- The internal sample rate. When input, output and internal rate are equal, "the sound pass through
  the CABLE without conversion, so with the best audio quality".
- The latency (pipe size). The rule is "Max Latency = 3 x Max Buffer Size"; the default 7168 samples
  covers clients using buffers up to 2048 samples.

Both persist in the registry only when set by the Control Panel running as administrator, and changing
Max Latency needs a reboot. Sources: [Reference Manual](https://vb-audio.com/Cable/VBCABLE_ReferenceManual.pdf),
"Configuring Internal Sample rate", "Configuring Internal Latency", "Understanding Latency".

The registry location isn't documented. It was taken from the pack-45 binaries (strings and disassembly
of `vbaudio_cable64_win10.sys` and `VBCABLE_Setup_x64.exe`) [INFERENCE; not tested on a machine with
VB-CABLE]:

- The driver reads REG_DWORD values once, at driver start. It reads them from
  `HKLM\Software\VB-Audio\Cable`, falling back to `HKLM\System\CurrentControlSet\Services\VB-Cable`:

  | Value | Meaning | Driver default / range |
  | --- | --- | --- |
  | `VBAudioCableWDM` | Max Latency (pipe allocation) | 7168; accepted 1024..32768 |
  | `VBAudioCableWDM_Latency` | Current latency | = Max Latency; accepted >= 48, clamped to Max |
  | `VBAudioCableWDM_SR` | Internal sample rate | 96000 if missing or invalid; accepts 44100/48000/88200/96000/176400/192000 |
  | `VBAudioCableWDM_LoopBack` | Loopback support | 0 |
  | `VBAudioCableWDM_VolCtl` | Windows volume control | 0 |

- The setup writes `VBAudioCableWDM_SR=48000`, `VBAudioCableWDM_Latency=7168` and
  `VBAudioCableWDM_LoopBack=1` when it installs.
- Endpoint default formats come from the INF (`PKEY_AudioEngine_OEMFormat`): 48 kHz, 24-bit, stereo for
  both CABLE Input and CABLE Output. The manual confirms "Both are setup in stereo 48kHz by default"
  (Win10/11).

`ConfigureFreshVbCable()` in `vbcable_setup.cc` runs only after this installer freshly installed
VB-CABLE. It leaves the driver parameters above alone and does this:

- CABLE Input / CABLE Output device formats: if either is not 48 kHz, it is set to 48 kHz 24-bit
  (keeping the channel count) with `IPolicyConfig::SetDeviceFormat`. Normally they are already 48 kHz
  from the INF.
- Default devices: the [Reference Manual](https://vb-audio.com/Cable/VBCABLE_ReferenceManual.pdf)
  ("Set VB-Cable as default device") warns that Windows 10+ may make a newly installed audio driver the
  default device. If CABLE Input/Output became the default for a role, the function restores the
  endpoint saved by `--snapshot-audio-defaults` with `IPolicyConfig::SetDefaultEndpoint`. It does this
  twice (about 2 s and 5 s after the endpoints appear), because switching can lag behind arrival.

### Measured latency

Click round-trip through the cable (shared mode, 48 kHz float, median of 8 clicks, Windows 11, pack 45),
including Windows' own buffering on both ends:

| Cable / setting | Latency |
| --- | --- |
| VAC Lite | ~19 ms |
| VB-CABLE, setup defaults (`VBAudioCableWDM_Latency` 7168) | ~75 ms |
| VB-CABLE, `VBAudioCableWDM_Latency` 3072, after a full reboot | ~93 ms |
| VB-CABLE, `VBAudioCableWDM` / `_Latency` 1024, after a device restart | ~84–86 ms |

Lowering the documented latency values didn't reduce VB-CABLE's delay, so Setup no longer writes
them; its setup already selects the 48 kHz internal rate. VAC Lite is the lower-latency cable, which is
why Setup offers to keep an installed VAC Lite instead of adding VB-CABLE.

`IPolicyConfig` (CLSID `870af99c-171d-4f9e-af0d-e63df40c2bc9`, IID `f8679f50-850a-41cf-9c72-430f290290c8`)
is the undocumented interface the Windows Sound control panel uses. Its layout matches, for example,
[Sunshine's PolicyConfig.h](https://docs.lizardbyte.dev/projects/sunshine/latest/PolicyConfig_8h_source.html)
and [AudioDeviceCmdlets](https://github.com/frgnca/AudioDeviceCmdlets/blob/master/SOURCE/IPolicyConfig.cs).

Uninstalling a VB-CABLE that is the current default device needs no extra handling: Windows picks
another default when the endpoint disappears.
