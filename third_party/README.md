# Third-party dependencies

Every external piece of AEC3 Virtual Mic falls into one of these groups. Only RNNoise's source
lives in this repository; everything else is fetched at a pinned revision or checksum.

| Dependency | How it is obtained | Pinned by | Shipped in the installer |
| --- | --- | --- | --- |
| **RNNoise** (BSD-3-Clause) | Vendored source in [`rnnoise/`](rnnoise/); its ~78 MB of generated model weights are fetched by [`scripts/fetch_rnnoise_model.ps1`](../scripts/fetch_rnnoise_model.ps1) | Upstream commit in [`rnnoise/VENDOR.md`](rnnoise/VENDOR.md); model SHA-256 in [`rnnoise/model_version`](rnnoise/model_version) | Compiled into the exe |
| **WebRTC** and its third-party tree (abseil, TensorFlow Lite, XNNPACK, …) | Fetched shallowly by [`scripts/fetch_webrtc.bat`](../scripts/fetch_webrtc.bat) | [`WEBRTC_REVISION`](../WEBRTC_REVISION) | Statically linked into the exe |
| **depot_tools** (GN/Ninja/gclient wrappers) | Cloned by `scripts/fetch_webrtc.bat` | Latest at fetch time (build tooling only) | No |
| **VB-CABLE** (VB-Audio donationware) | Downloaded by [`installer/fetch_vbcable.ps1`](../installer/fetch_vbcable.ps1) | Pack URL + SHA-256 in that script | Yes, unmodified, under VB-Audio's distribution terms |
| **Chrome's neural echo model** (Google, unreleased) | Copied at runtime from the user's own Chrome profile | — | **Never** — not redistributable |

Rules:

- Vendor only small, license-compatible sources that must be patched or built with our own GN
  target. Record the upstream URL, revision, local changes and license in a `VENDOR.md` next to
  them.
- Everything large or binary is fetched at a pinned revision or checksum, never committed.
- License texts for everything linked into the executable are collected in
  [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) by
  [`scripts/generate_third_party_notices.py`](../scripts/generate_third_party_notices.py).
