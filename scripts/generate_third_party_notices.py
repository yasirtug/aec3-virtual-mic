"""Regenerates THIRD_PARTY_NOTICES.md from license files in the WebRTC checkout.

Usage: generate_third_party_notices.py [WEBRTC_ROOT]

WEBRTC_ROOT defaults to the checkout created by scripts/fetch_webrtc.bat
(<repo>/.deps/webrtc/src, or %AEC3_DEPS_DIR%/webrtc/src).

The component list mirrors what lld-link actually links into
aec3_virtual_mic.exe (static libraries, Rust rlibs, compiled-in header-only
libraries). Re-check it when WEBRTC_REVISION or BUILD.gn deps change, e.g. from
the link line in out/aec3_virtual_mic/obj/examples/aec3_virtual_mic/aec3_virtual_mic.ninja
and `gn desc out/aec3_virtual_mic //examples/aec3_virtual_mic:aec3_virtual_mic deps --all`.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
OUTPUT = PROJECT_DIR / "THIRD_PARTY_NOTICES.md"

# Paths starting with "project:" are relative to this repository, all others
# to WEBRTC_ROOT.
PROJECT_PREFIX = "project:"


@dataclass
class Component:
    name: str
    url: str
    license: str
    files: list[str]
    used_for: str
    # Directory holding README.chromium (revision lookup); empty = none.
    metadata_dir: str = ""
    revision: str = ""
    note: str = ""
    extra: list[str] = field(default_factory=list)


COMPONENTS = [
    Component(
        "WebRTC",
        "https://webrtc.googlesource.com/src",
        "BSD-3-Clause + additional patent grant",
        ["LICENSE", "PATENTS"],
        "Audio Processing Module (AEC3 echo canceller, noise suppression, "
        "neural residual echo estimator runtime)",
    ),
    Component(
        "Chromium build support code (//build, //buildtools)",
        "https://chromium.googlesource.com/chromium/src/build",
        "BSD-3-Clause",
        ["buildtools/LICENSE"],
        "Rust allocator shims and build glue linked into the executable",
    ),
    Component(
        "Abseil",
        "https://github.com/abseil/abseil-cpp",
        "Apache-2.0",
        ["third_party/abseil-cpp/LICENSE"],
        "C++ utility library used by WebRTC",
        "third_party/abseil-cpp",
    ),
    Component(
        "TensorFlow Lite (including bundled TSL/XLA headers)",
        "https://github.com/tensorflow/tensorflow",
        "Apache-2.0",
        ["third_party/tflite/LICENSE"],
        "Runs the neural residual echo estimator model",
        "third_party/tflite",
    ),
    Component(
        "XNNPACK",
        "https://github.com/google/xnnpack",
        "BSD-3-Clause",
        ["third_party/xnnpack/src/LICENSE"],
        "TensorFlow Lite CPU kernels",
        "third_party/xnnpack",
    ),
    Component(
        "ruy",
        "https://github.com/google/ruy",
        "Apache-2.0",
        ["third_party/ruy/LICENSE"],
        "TensorFlow Lite matrix multiplication",
        "third_party/ruy",
    ),
    Component(
        "pthreadpool",
        "https://github.com/google/pthreadpool",
        "BSD-2-Clause",
        ["third_party/pthreadpool/src/LICENSE"],
        "XNNPACK thread pool",
        "third_party/pthreadpool",
    ),
    Component(
        "cpuinfo",
        "https://github.com/pytorch/cpuinfo",
        "BSD-2-Clause",
        ["third_party/cpuinfo/src/LICENSE"],
        "CPU feature detection for XNNPACK/ruy",
        "third_party/cpuinfo",
    ),
    Component(
        "FXdiv",
        "https://github.com/Maratyszcza/FXdiv",
        "MIT",
        ["third_party/fxdiv/src/LICENSE"],
        "Header-only integer division used by pthreadpool",
        "third_party/fxdiv",
    ),
    Component(
        "FP16",
        "https://github.com/Maratyszcza/FP16",
        "MIT",
        ["third_party/fp16/LICENSE"],
        "Header-only half-precision conversion used by XNNPACK",
        "third_party/fp16",
    ),
    Component(
        "Eigen",
        "https://gitlab.com/libeigen/eigen",
        "MPL-2.0",
        ["third_party/eigen3/LICENSE"],
        "Header-only linear algebra used by TensorFlow Lite kernels",
        "third_party/eigen3",
        note="Eigen is used unmodified as distributed in the WebRTC/Chromium "
        "checkout. Its source code is available at "
        "https://gitlab.com/libeigen/eigen (revision below).",
    ),
    Component(
        "gemmlowp",
        "https://github.com/google/gemmlowp",
        "Apache-2.0",
        ["third_party/gemmlowp/LICENSE"],
        "Header-only fixed-point helpers used by TensorFlow Lite kernels",
        "third_party/gemmlowp",
    ),
    Component(
        "FlatBuffers",
        "https://github.com/google/flatbuffers",
        "Apache-2.0",
        ["third_party/flatbuffers/LICENSE"],
        "TensorFlow Lite model format parsing",
        "third_party/flatbuffers",
    ),
    Component(
        "ml_dtypes",
        "https://github.com/jax-ml/ml_dtypes",
        "Apache-2.0",
        ["third_party/ml_dtypes/src/LICENSE"],
        "Header-only numeric types used by TensorFlow Lite",
        "third_party/ml_dtypes",
    ),
    Component(
        "ARM_NEON_2_x86_SSE",
        "https://github.com/intel/ARM_NEON_2_x86_SSE",
        "BSD-Source-Code",
        ["third_party/neon_2_sse/LICENSE"],
        "Header-only SIMD portability layer used by TensorFlow Lite",
        "third_party/neon_2_sse",
    ),
    Component(
        "FarmHash",
        "https://github.com/google/farmhash",
        "MIT",
        ["third_party/farmhash/LICENSE"],
        "Hashing used by TensorFlow Lite",
        "third_party/farmhash",
    ),
    Component(
        "fft2d (Ooura FFT package)",
        "http://www.kurims.kyoto-u.ac.jp/~ooura/fft.html",
        "fft2d",
        ["third_party/fft2d/LICENSE"],
        "FFT routines used by TensorFlow Lite",
        "third_party/fft2d",
    ),
    Component(
        "Protocol Buffers (lite runtime)",
        "https://github.com/protocolbuffers/protobuf",
        "BSD-3-Clause",
        ["third_party/protobuf/LICENSE"],
        "Message runtime linked by WebRTC/TensorFlow Lite",
        "third_party/protobuf",
    ),
    Component(
        "Perfetto",
        "https://github.com/google/perfetto",
        "Apache-2.0 (with BSD-3-Clause and public-domain parts)",
        ["third_party/perfetto/LICENSE"],
        "Tracing library linked by WebRTC",
        "third_party/perfetto",
    ),
    Component(
        "PFFFT",
        "https://bitbucket.org/jpommier/pffft/",
        "pffft (BSD-like, FFTPACK)",
        ["third_party/pffft/LICENSE"],
        "FFT used by WebRTC audio processing",
        "third_party/pffft",
    ),
    Component(
        "RNNoise (WebRTC copy)",
        "https://github.com/xiph/rnnoise",
        "BSD-3-Clause",
        ["third_party/rnnoise/COPYING"],
        "Voice activity detection inside WebRTC's AGC2",
        "third_party/rnnoise",
    ),
    Component(
        "RNNoise (bundled with this project)",
        "https://github.com/xiph/rnnoise",
        "BSD-3-Clause",
        [PROJECT_PREFIX + "third_party/rnnoise/COPYING"],
        "RNNoise noise suppression modes (compiled-in standard model weights)",
        revision="70f1d256acd4b34a572f999a05c87bf00b67730d",
    ),
    Component(
        "Ooura FFT (WebRTC common_audio)",
        "http://www.kurims.kyoto-u.ac.jp/~ooura/fft.html",
        "LicenseRef-takuya-ooura",
        ["common_audio/third_party/ooura/LICENSE"],
        "FFT used by WebRTC audio processing",
    ),
    Component(
        "spl_sqrt_floor",
        "http://www.pertinentdetail.org/sqrt",
        "SPL-SQRT-FLOOR",
        ["common_audio/third_party/spl_sqrt_floor/LICENSE"],
        "Integer square root used by WebRTC signal processing",
    ),
    Component(
        "fft (WebRTC modules/third_party/fft)",
        "https://webrtc.googlesource.com/src/+/main/modules/third_party/fft",
        "dtoa",
        ["modules/third_party/fft/LICENSE"],
        "FFT used by WebRTC audio processing",
    ),
    Component(
        "LLVM libc++",
        "https://libcxx.llvm.org/",
        "Apache-2.0 WITH LLVM-exception",
        ["third_party/libc++/src/LICENSE.TXT"],
        "C++ standard library (statically linked)",
        "third_party/libc++",
    ),
    Component(
        "LLVM libc",
        "https://libc.llvm.org/",
        "Apache-2.0 WITH LLVM-exception",
        ["third_party/llvm-libc/src/LICENSE.TXT"],
        "Support routines used by libc++",
        "third_party/llvm-libc",
    ),
    Component(
        "LLVM compiler-rt",
        "https://compiler-rt.llvm.org/",
        "Apache-2.0 WITH LLVM-exception",
        ["third_party/compiler-rt/src/LICENSE.TXT"],
        "Compiler runtime builtins",
        "third_party/compiler-rt",
    ),
    Component(
        "cxx (Rust crate)",
        "https://crates.io/crates/cxx",
        "Apache-2.0 (dual-licensed MIT OR Apache-2.0 upstream)",
        ["third_party/rust/chromium_crates_io/vendor/cxx-v1/LICENSE-APACHE"],
        "Rust/C++ interop used by WebRTC's Rust components",
        "third_party/rust/cxx/v1",
    ),
    Component(
        "foldhash (Rust crate)",
        "https://crates.io/crates/foldhash",
        "Zlib",
        ["third_party/rust/chromium_crates_io/vendor/foldhash-v0_2/LICENSE"],
        "Hashing used by WebRTC's Rust components",
        "third_party/rust/foldhash/v0_2",
    ),
    Component(
        "Rust standard library",
        "https://www.rust-lang.org/",
        "MIT OR Apache-2.0 (Unicode tables: Unicode-3.0)",
        [
            "third_party/rust-toolchain/share/doc/rustc/licenses/MIT.txt",
            "third_party/rust-toolchain/share/doc/rustc/licenses/Unicode-3.0.txt",
        ],
        "Runtime for WebRTC's Rust components",
        note="Copyright The Rust Project Developers (see "
        "https://thanks.rust-lang.org). Used under the MIT license option. "
        "The full per-crate notice list for the standard library is "
        "COPYRIGHT-library.html in the Rust toolchain "
        "(share/doc/rustc/COPYRIGHT-library.html).",
    ),
]

HEADER = """\
# Third-party notices

AEC3 Virtual Mic is licensed under the MIT License (see `LICENSE`). The
`AEC3VirtualMic.exe` binary statically links the third-party software listed
below; each component remains under its own license, reproduced in full in
this file. WebRTC revision: `{webrtc_revision}`.

This file is generated by `scripts/generate_third_party_notices.py` from the
license files in the pinned WebRTC checkout; do not edit it by hand.

## Not included: Chrome's neural echo model

The "Neural (Chrome model)" echo suppression mode uses
`residual_echo_estimator.tflite`, an unreleased machine-learning residual echo
estimator made by Google that Google Chrome downloads for its own echo
cancellation. This model is **not** part of this project, is **not** covered
by this project's MIT license, and is **not** distributed with the source code,
the executable, or the installer. On request (and on first start) the app only
copies the file from the user's local Chrome profile to
`%LOCALAPPDATA%\\AEC3VirtualMic\\`; nothing is downloaded or uploaded. If
Chrome has not downloaded the model, the app uses classic WebRTC AEC3.

## Bundled with the installer: Virtual Audio Cable Lite

The Windows installer bundles the unmodified **Virtual Audio Cable (VAC) Lite**
package by Eugene Muzychenko and runs its own setup when no virtual audio cable
is installed. VAC Lite is freeware, redistributed free of charge and unmodified
as its license permits (https://vac.muzychenko.net/en/license.txt). It may only
be used in a private home environment not associated with income generation,
and it is not covered by this project's MIT license. https://vac.muzychenko.net

## Components linked into AEC3VirtualMic.exe

| Component | License | Used for |
| --- | --- | --- |
"""


def default_webrtc_root() -> Path:
    deps = os.environ.get("AEC3_DEPS_DIR")
    base = Path(deps) if deps else PROJECT_DIR / ".deps"
    return base / "webrtc" / "src"


def resolve(root: Path, relative: str) -> Path:
    if relative.startswith(PROJECT_PREFIX):
        return PROJECT_DIR / relative[len(PROJECT_PREFIX) :]
    return root / relative


def read_revision(root: Path, component: Component) -> str:
    if component.revision or not component.metadata_dir:
        return component.revision
    readme = root / component.metadata_dir / "README.chromium"
    text = readme.read_text(encoding="utf-8", errors="replace")
    for key in ("Revision", "Version"):
        match = re.search(rf"^{key}:\s*(\S.*)$", text, flags=re.MULTILINE)
        if match and match.group(1).strip() not in ("N/A", "DEPS"):
            return match.group(1).strip()
    return ""


def fence(text: str) -> str:
    marker = "~~~"
    while marker in text:
        marker += "~"
    return f"{marker}text\n{text.rstrip()}\n{marker}"


def anchor(name: str) -> str:
    slug = re.sub(r"[^a-z0-9 -]", "", name.lower()).strip()
    return re.sub(r"\s+", "-", slug)


def main() -> int:
    if len(sys.argv) > 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else default_webrtc_root()
    if not (root / ".gn").is_file():
        print(f"WebRTC checkout not found at {root}", file=sys.stderr)
        return 1
    revision_file = PROJECT_DIR / "WEBRTC_REVISION"
    webrtc_revision = revision_file.read_text(encoding="utf-8").strip()

    missing = [
        str(resolve(root, f))
        for component in COMPONENTS
        for f in component.files
        if not resolve(root, f).is_file()
    ]
    if missing:
        print("Missing license files:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 1

    parts = [HEADER.format(webrtc_revision=webrtc_revision)]
    for component in COMPONENTS:
        parts.append(
            f"| [{component.name}](#{anchor(component.name)}) | "
            f"{component.license} | {component.used_for} |\n"
        )
    parts.append("\n## License texts\n")

    # Identical license texts are reproduced once and referenced afterwards.
    seen: dict[str, str] = {}
    for component in COMPONENTS:
        revision = (
            webrtc_revision if component.name == "WebRTC" else read_revision(root, component)
        )
        parts.append(f"\n### {component.name}\n\n")
        parts.append(f"- Source: {component.url}\n")
        if revision:
            parts.append(f"- Revision: `{revision}`\n")
        parts.append(f"- License: {component.license}\n")
        if component.note:
            parts.append(f"\n{component.note}\n")
        for relative in component.files:
            text = resolve(root, relative).read_text(encoding="utf-8", errors="replace")
            text = text.replace("\r\n", "\n").strip("\n")
            key = re.sub(r"\s+", " ", text).strip()
            label = relative.removeprefix(PROJECT_PREFIX)
            if key in seen:
                parts.append(
                    f"\n`{label}`: identical to the text reproduced for "
                    f"[{seen[key]}](#{anchor(seen[key])}).\n"
                )
                continue
            seen[key] = component.name
            parts.append(f"\n`{label}`:\n\n{fence(text)}\n")

    OUTPUT.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"Wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
