# RNNoise (vendored)

| | |
| --- | --- |
| Upstream | https://github.com/xiph/rnnoise |
| Revision | `70f1d256acd4b34a572f999a05c87bf00b67730d` |
| Model | Standard model `rnnoise_data-<SHA256>.tar.gz` from https://media.xiph.org/rnnoise/models/, SHA-256 `0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37` (also in `model_version`) |
| License | BSD-3-Clause, see [`COPYING`](COPYING) |
| Built by | `rnnoise` target in [`/BUILD.gn`](../../BUILD.gn) |

Contents: `include/`, `src/` (library sources), `COPYING`, `AUTHORS`, `model_version`. Training,
examples and autotools files are omitted. The generated weights `src/rnnoise_data.c/.h` (~78 MB)
are **not committed**: [`scripts/fetch_rnnoise_model.ps1`](../../scripts/fetch_rnnoise_model.ps1)
downloads the model tarball named by `model_version`, verifies that SHA-256 and extracts them
(`build.bat` runs it automatically), like upstream's `download_model.sh`.

Local modifications: none. The app limits RNNoise's attenuation in its own code
(`src/audio_engine.cc`, dry/wet mix aligned to RNNoise's 20 ms delay) instead of patching the
library.

To update: copy the same subset from the new upstream revision, put the new model's SHA-256 in
`model_version` (the fetch script derives the download URL from it), update this file, the source
list in `BUILD.gn` if files changed, and the RNNoise revision in
`scripts/generate_third_party_notices.py`.
