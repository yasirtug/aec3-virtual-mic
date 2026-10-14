# AEC3 Virtual Mic installer

Inno Setup 6 script that installs AEC3 Virtual Mic per machine and, when no virtual audio cable is
installed, runs the setup of Virtual Audio Cable (VAC) Lite by Eugene Muzychenko.

## Building

From the repository root, after `bin\AEC3VirtualMic.exe` has been built:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\fetch_vaclite.ps1   # download + verify + extract (no install)
ISCC.exe installer\AEC3VirtualMic.iss                                             # -> installer\Output\AEC3VirtualMic-Setup-<version>.exe
```

- `fetch_vaclite.ps1 [-Force]` downloads the pinned package into `installer\vendor\vaclite\`
  (gitignored), checks its SHA-256 and extracts it unmodified. It does nothing when the verified
  package is already extracted; `-Force` downloads it again. Exit codes: 0 ok, 1 download failed,
  2 hash mismatch, 3 incomplete archive. It runs on Windows PowerShell 5.1 and PowerShell 7.
- The script reads the version from `AEC3VM_VERSION_STR` in `src\resource.h`; `ISCC /DAppVersion=x.y.z` overrides it.
- ISCC packages `bin\AEC3VirtualMic.exe`, `LICENSE` (as `LICENSE.txt`), `THIRD_PARTY_NOTICES.md`,
  `assets\app.ico` (setup icon) and the unmodified VAC Lite package. It never packages the Chrome
  echo model.
- Inno Setup isn't on `PATH` by default. `winget install JRSoftware.InnoSetup` installs it per user to
  `%LOCALAPPDATA%\Programs\Inno Setup 6\`, and Chocolatey `innosetup` installs it to
  `C:\Program Files (x86)\Inno Setup 6\`.

## What the installer does

| Situation | Behavior |
| --- | --- |
| A virtual cable is installed (VAC of any edition, or VB-CABLE) | Setup uses it and installs no cable; no extra page. Detection runs the bundled exe with `--detect-cables` from Setup's temp folder (exit code bitmask: 1 = VB-CABLE, 2 = VAC), i.e. with the app's own detection. |
| No virtual cable | A page says Setup will run the setup of Virtual Audio Cable Lite, that VAC Lite is free for use in a private home environment not associated with income generation, and links to vac.muzychenko.net. The unmodified package is unpacked to Setup's temp folder and VAC's own setup window runs (`setup64.exe`, `setup64a.exe` on ARM64). If VAC Lite's uninstall entry exists afterwards, Setup writes `HKLM\Software\AEC3VirtualMic` `InstalledVacLite=1` (DWORD); otherwise it explains how to install a cable manually. |
| Task "Start AEC3 Virtual Mic with Windows" (unchecked by default) | Runs `AEC3VirtualMic.exe --enable-autostart` as the original (non-elevated) user. |

On a fresh VAC Lite install, Setup also writes the documented per-cable setting
`HKLM\SOFTWARE\EuMus Design\Virtual Audio Cable\4\Cable 1` `Volume control = 1` (DWORD) before VAC's
setup runs ([Configuration data in the Registry](https://vac.muzychenko.net/en/manual/configure.htm)).
VAC's setup copies it into the driver's `Services\VirtualAudioCable_83ed7f0e\Parameters\Cable 1`, so
the cable starts with volume control enabled (verified, no reboot). At 100% the signal is unchanged;
it gives "Line 1" a working volume slider in Windows (−40..+12 dB). An existing VAC is never changed.

VAC's `setup64.exe -u` hands the work to another process and returns at once, so the uninstaller
starts it through `powershell Start-Process -Wait`, which also waits for that child.
| "Launch AEC3 Virtual Mic now" (finish page) | Starts the app as the original user. |
| Uninstall | Closes a running app (mutex `AEC3VirtualMic.Running`, then `taskkill /F`). If VAC Lite is installed, shows an "Also uninstall Virtual Audio Cable Lite" checkbox, checked by default only when the marker says Setup installed it, and runs VAC's own uninstaller (its registered `UninstallString`, `"…\setup64.exe" -u`) when checked. VAC has no documented silent uninstall, so silent uninstalls leave it installed. Removes the HKCU Run value `AEC3VirtualMic` and `HKCU\Software\AEC3VirtualMic` for every loaded user profile, removes `{localappdata}\AEC3VirtualMic` of the uninstalling user, and deletes `HKLM\Software\AEC3VirtualMic`. |

When VAC Lite is installed, the [Run] order is:

1. `AEC3VirtualMic.exe --snapshot-audio-defaults` saves the current default playback and recording
   endpoints for all roles.
2. VAC Lite's own setup (interactive) installs the driver.
3. `AEC3VirtualMic.exe --restore-audio-defaults` gives every role whose default became a virtual cable
   endpoint back to the saved device, and deletes the snapshot. VAC's manual warns that Windows may
   make a newly installed cable the default device
   ([Installation](https://vac.muzychenko.net/en/manual/install.htm), "Checking default endpoint
   assignment").

## Virtual Audio Cable Lite

### Package

- Download: `https://software.muzychenko.net/freeware/vac471lite.zip` (VAC 4.71 Lite), linked from
  the [download page](https://vac.muzychenko.net/en/download.htm). Pinned SHA-256
  `1ed42a7e763bf6237438fb62191ae88b2922f014b37658b29d7801d72c9d5357` (1,385,091 bytes, downloaded
  2026-10-14).
- The package contains `setup.exe` (x86 launcher), `setup64.exe`, `setup64a.exe` (ARM64), `setup.ini`,
  the driver (`x64\vrtaucbl.sys`, `vrtaucbl.inf/.cat`), the control panel and tools. The manual says to
  run setup from the unpacked folder with its subfolder structure intact, and `setup64a.exe` on ARM64
  ([Installation](https://vac.muzychenko.net/en/manual/install.htm)). Setup runs the x64/ARM64 setup
  directly so that it waits for the real setup process, not a launcher.
- `setup.ini`: `AppName=Virtual Audio Cable (lite)`, `AppGUID=83ed7f0e-2028-4956-b0b4-39c76fdaef1d`,
  `RegisterUninstaller=1`, default install folder `%ProgramFilesDir%\Virtual Audio Cable`. The
  installed uninstall entry is
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{83ed7f0e-2028-4956-b0b4-39c76fdaef1d}`
  with `UninstallString` `"C:\Program Files\Virtual Audio Cable\setup64.exe" -u` (verified on an
  installed system). The endpoints are named "Line 1 (Virtual Audio Cable)" on both sides.
- `setup64.exe` contains an unattended mode and short options (`-s`, `-f`, `-r`, `-u`), but they are
  undocumented, so the installer only uses VAC's interactive setup and its registered uninstall
  command.

### Licensing

From the [license agreement](https://vac.muzychenko.net/en/license.txt) and the
[distribution policy](https://vac.muzychenko.net/en/manual/distrib.htm):

- "The Licensee is permitted to distribute FREEWARE (lite) or TRIAL versions of this software" if the
  content and structure are not modified (compressing/archiving is allowed) and the licensee doesn't
  profit from providing it. Lite "can be distributed freely if their packaging and file contents remain
  unchanged and there are no commercial purposes. They cannot be sold, solely or within other
  products." This free, open-source installer ships the package unmodified.
- The Lite version "can be used only in a private home environment that is not associated with income
  generation". The installer page, the app's About box, the README and the notices state this.

## Why VAC Lite and not VB-CABLE

Click round-trip through each cable (shared mode, 48 kHz float, median of 8 clicks, Windows 11),
including Windows' own buffering on both ends:

| Cable / setting | Latency |
| --- | --- |
| VAC Lite (defaults, 3 ms per interrupt) | ~19 ms |
| VB-CABLE 3.3.1.7, default latency 7168 samples | ~75–93 ms |
| VB-CABLE, latency 3072 samples | ~90 ms |
| VB-CABLE, latency 1536 samples (lowest that worked on the test machine) | ~80 ms |

VB-CABLE's latency setting (`VBAudioCableWDM_Latency`, which its control panel writes to both
`HKLM\Software\VB-Audio\Cable` and `HKLM\SYSTEM\CurrentControlSet\Services\VB-Cable`) applies live but
only moves the delay by about a third of the buffer change; a floor of roughly 70 ms remains (its
control panel reports a 4096-sample DMA buffer per side, ≈85 ms [INFERENCE: likely the floor]). The app
still detects and supports an installed VB-CABLE, but prefers VAC Lite when both are present.

`IPolicyConfig` (CLSID `870af99c-171d-4f9e-af0d-e63df40c2bc9`, IID `f8679f50-850a-41cf-9c72-430f290290c8`),
used to restore default devices, is the undocumented interface the Windows Sound control panel uses. Its
layout matches, for example,
[Sunshine's PolicyConfig.h](https://docs.lizardbyte.dev/projects/sunshine/latest/PolicyConfig_8h_source.html)
and [AudioDeviceCmdlets](https://github.com/frgnca/AudioDeviceCmdlets/blob/master/SOURCE/IPolicyConfig.cs).
