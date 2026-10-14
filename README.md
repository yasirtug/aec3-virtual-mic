# AEC3 Virtual Mic

Tired of Discord's unreliable noise/echo cancellation on desktop? Want something stable & better?

\
\
\
\
If you want to experience superior echo cancellation right now & you have Chrome, open a voice recorder website on a tab, play some music in another tab, and talk over the playing music while recording. You'll see music sound is perfectly getting canceled out by the Chrome.

\
So this project is to replicate that, but system wide.

It captures your physical microphone, uses what your speakers are playing as the echo reference,
runs the result through WebRTC's audio processing (the AEC3 echo canceller used by Chrome), and
plays the cleaned-up voice into a virtual audio cable. Voice apps then use that cable as their
microphone.

```
Microphone ─┐
            ├─► WebRTC AEC3 (+ optional neural echo model, noise suppression) ─► Line 1 (playback)
Speakers  ──┘   (loopback = echo reference)                                     │
                                                                                ▼
                                     Discord etc. use "Line 1 (Virtual Audio Cable)" as the mic
```

## Features

- **Echo cancellation with WebRTC AEC3**: the same echo canceller Chrome uses for calls, fed with
  a loopback capture of your real speaker output.
- **Echo suppression mode**: *Neural (Chrome model)* or *Classic WebRTC AEC3*. The neural mode uses
  Chrome's machine-learning residual echo estimator when it is available on your PC (see
  [About the neural echo model](#about-the-neural-echo-model)); the classic mode never touches it.
- **Noise suppression**: off, WebRTC noise suppression (moderate or high), or RNNoise with
  gentle, medium, or full strength. Gentle and medium limit how much noise is removed, so your
  voice sounds more natural.
- **Virtual Audio Cable Lite set up for you**: if you have no virtual audio cable, the installer
  runs the setup of the free *Virtual Audio Cable (VAC) Lite*, which adds very little latency
  (~19 ms measured, against ~80 ms for VB-CABLE). An installed VB-CABLE works too.
- **Tray app**: live level meters, settings can be changed while it runs, optional start with
  Windows (off by default).

## Install

1. Download `AEC3VirtualMic-Setup-<version>.exe` from the
   [Releases](../../releases) page and run it.
2. If no virtual audio cable is installed, the installer tells you so and runs the setup of
   Virtual Audio Cable (VAC) Lite by Eugene Muzychenko (https://vac.muzychenko.net). VAC Lite is
   free for use in a private home environment not associated with income generation. An
   installed VAC or VB-CABLE is detected and used as it is.
3. Optionally enable starting with Windows (off by default; you can change this later in the app).

Uninstalling removes the app, its settings and its autostart entry, and offers to remove
VAC Lite as well (pre-selected only if this installer installed it).

Requires 64-bit Windows 10 or 11.

## Usage

1. Start **AEC3 Virtual Mic**; it runs in the system tray, open its window from the tray icon.
2. Choose your **microphone** and the **speakers** you listen on. The speakers are the echo
   reference, so pick the device Discord (or your game, music, …) plays through.
3. Choose the echo suppression mode and noise suppression, then press **Start**.
4. In Discord (*User Settings → Voice & Video → Input Device*) or any other app, select
   **Line 1 (Virtual Audio Cable)** as the microphone (with VB-CABLE: **CABLE Output
   (VB-Audio Virtual Cable)**). The app shows the exact name to pick.

Tips:

- Switch Discord's Input Profile to Studio so your voice doesn't get processed twice.
- If something fails (for example a device is unplugged), the app stops and shows the reason.
  Press **Start** again once the problem is fixed.

## About the neural echo model

The *Neural (Chrome model)* mode uses `residual_echo_estimator.tflite`, an **unreleased
machine-learning residual echo estimator made by Google** that Google Chrome downloads in the
background for its own echo cancellation.

- This project does **not** include or distribute the model, and it is **not** covered by this
  project's MIT license.
- The app only **copies the file from your local Chrome profile** (Chrome Stable, Beta, Dev or
  Canary) to `%LOCALAPPDATA%\AEC3VirtualMic\`. Nothing is downloaded from the internet and
  nothing is uploaded.
- The model may be **absent** if Chrome is not installed or has not downloaded it (yet). It is
  imported automatically on first start and can be re-imported with *Import from Chrome*.
- When the model is missing or cannot be loaded, the app falls back to **Classic WebRTC AEC3** and
  says so in its status. You can also select *Classic WebRTC AEC3* yourself; then the model is
  never imported or loaded.

## Building from source

The app is built inside a WebRTC checkout with GN, Ninja and clang-cl, pinned to the revision in
[`WEBRTC_REVISION`](WEBRTC_REVISION).

Requirements:

- Git for Windows
- Visual Studio 2022 or 2026 (any edition, including Build Tools) with the
  *Desktop development with C++* workload
- Windows 11 SDK 10.0.26100 including *Debugging Tools for Windows*
- Inno Setup 6 (for the installer)
- About 25 GB of free disk space

Steps (from the repository root, in `cmd`):

```bat
:: 1. Shallow-fetch depot_tools and WebRTC (no git history, no test resources) into .deps\
scripts\fetch_webrtc.bat

:: 2. Build bin\AEC3VirtualMic.exe (also fetches RNNoise's pinned model weights on first run)
build.bat

:: 3. Download VAC Lite for the installer, then build the installer
powershell -NoProfile -ExecutionPolicy Bypass -File installer\fetch_vaclite.ps1
"%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" installer\AEC3VirtualMic.iss
```

The installer is written to `installer\Output\AEC3VirtualMic-Setup-<version>.exe`.

`build.bat` copies `BUILD.gn`, `src\`, `assets\` and `third_party\` into
`<WEBRTC_ROOT>\examples\aec3_virtual_mic`, patches the WebRTC build files it needs
(`scripts\configure_webrtc.py`), and runs `gn gen` and `ninja`. Environment overrides:

| Variable | Default | Meaning |
| --- | --- | --- |
| `AEC3_DEPS_DIR` | `<repo>\.deps` | Where `fetch_webrtc.bat` puts depot_tools and WebRTC |
| `WEBRTC_ROOT` | `%AEC3_DEPS_DIR%\webrtc\src` | Use an existing WebRTC `src` checkout |
| `DEPOT_TOOLS` | `%AEC3_DEPS_DIR%\depot_tools` | Use an existing depot_tools |
| `vs2026_install` / `vs2022_install` | detected with `vswhere` | Visual Studio installation to use |

Keep the repository in a short path (for example `C:\src\aec3`) to stay clear of Windows path
length limits, or enable long paths with `git config --global core.longpaths true`.

To move to a newer WebRTC, change `WEBRTC_REVISION`, re-run `scripts\fetch_webrtc.bat` and
`build.bat`, and regenerate the notices with
`.deps\depot_tools\python3.bat scripts\generate_third_party_notices.py`.

### Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Application sources (engine, UI, installer helpers, resources, manifest) |
| `assets/` | Icons |
| `third_party/` | Vendored source (RNNoise); see [third_party/README.md](third_party/README.md) for how every dependency is obtained |
| `installer/` | Inno Setup script, pinned VAC Lite download, installer research notes |
| `scripts/` | WebRTC and RNNoise-model fetch scripts, WebRTC build patches, third-party notices generator |
| `BUILD.gn`, `build.bat`, `WEBRTC_REVISION` | Build definition, build entry point, pinned WebRTC revision |

### CI and releases

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds the executable and the
installer on GitHub Actions for every push to `main` and every pull request (the installer is
attached to the run as an artifact). Pushing a tag `vX.Y.Z` that matches `AEC3VM_VERSION_STR` in
`src/resource.h` also publishes a GitHub release with the installer.

## License

- AEC3 Virtual Mic is released under the [MIT License](LICENSE).
- The executable statically links WebRTC, TensorFlow Lite, RNNoise and other third-party
  components under their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Chrome's neural echo model is **not** bundled and **not** covered by the MIT license; the app
  only copies it from your own Chrome installation (see above).
- The installer bundles the unmodified **Virtual Audio Cable (VAC) Lite** package by Eugene
  Muzychenko, redistributed free of charge as its license permits
  (https://vac.muzychenko.net/en/license.txt). VAC Lite may only be used in a private home
  environment not associated with income generation; for other use, see
  https://vac.muzychenko.net.
