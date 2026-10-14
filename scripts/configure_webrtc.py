from __future__ import annotations

import os
import re
import sys
import winreg
from pathlib import Path


def windows_kits_include_dir() -> Path:
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\Microsoft\Windows Kits\Installed Roots",
            0,
            winreg.KEY_READ | winreg.KEY_WOW64_32KEY,
        ) as key:
            kits_root = Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
    except OSError:
        program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
        kits_root = Path(program_files) / "Windows Kits" / "10"
    return kits_root / "Include"


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"Expected build configuration was not found in {path}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def expose_target(root: Path) -> None:
    examples_build = root / "examples" / "BUILD.gn"
    text = examples_build.read_text(encoding="utf-8")
    if 'deps += [ "aec3_virtual_mic" ]' in text:
        return
    needle = '  deps = []\n'
    insertion = (
        '  deps = []\n\n'
        '  if (is_win) {\n'
        '    deps += [ "aec3_virtual_mic" ]\n'
        '  }\n'
    )
    replace_once(examples_build, needle, insertion)


def expose_tflite_op_resolver(root: Path) -> None:
    # The neural residual echo estimator needs tflite's BuiltinOpResolver,
    # whose target is only visible to //modules/* and Chromium internals.
    tflite_build = root / "third_party" / "tflite" / "BUILD.gn"
    entry = '      "//examples/aec3_virtual_mic:*",\n'
    if entry in tflite_build.read_text(encoding="utf-8"):
        return
    needle = (
        '      "//components/optimization_guide/internal/*",\n'
        '      "//modules/*",\n'
    )
    replace_once(tflite_build, needle, needle + entry)


def select_installed_sdk() -> tuple[str, str] | None:
    kits = windows_kits_include_dir()
    versions: list[tuple[tuple[int, ...], Path]] = []
    if not kits.is_dir():
        return None
    for directory in kits.iterdir():
        try:
            numeric_version = tuple(int(part) for part in directory.name.split("."))
        except ValueError:
            continue
        if (directory / "um").is_dir() and (directory / "shared" / "sdkddkver.h").is_file():
            versions.append((numeric_version, directory))
    if not versions:
        return None

    _, selected = max(versions)
    sdkddkver = (selected / "shared" / "sdkddkver.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    ntddi_names = re.findall(
        r"^#define\s+(NTDDI_WIN11_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)",
        sdkddkver,
        flags=re.MULTILINE,
    )
    if not ntddi_names:
        raise RuntimeError(f"No Windows 11 NTDDI version was found in {selected}")
    ntddi_name = max(ntddi_names, key=lambda item: int(item[1], 16))[0]
    return selected.name, ntddi_name


def use_available_sdk(root: Path) -> None:
    setup_toolchain = root / "build" / "toolchain" / "win" / "setup_toolchain.py"
    setup_text = setup_toolchain.read_text(encoding="utf-8")
    match = re.search(r"^SDK_VERSION = '([^']+)'", setup_text, flags=re.MULTILINE)
    if match is None:
        raise RuntimeError(f"SDK_VERSION was not found in {setup_toolchain}")

    required_version = match.group(1)
    required_include = windows_kits_include_dir() / required_version / "um"
    if required_include.is_dir():
        return

    installed = select_installed_sdk()
    if installed is None:
        raise RuntimeError("No installed Windows 10/11 SDK was found")
    sdk_version, ntddi_name = installed
    setup_text = re.sub(
        r"^SDK_VERSION = '[^']+'",
        f"SDK_VERSION = '{sdk_version}'",
        setup_text,
        count=1,
        flags=re.MULTILINE,
    )
    setup_toolchain.write_text(setup_text, encoding="utf-8")

    win_build = root / "build" / "config" / "win" / "BUILD.gn"
    win_text = win_build.read_text(encoding="utf-8")
    win_text, replacements = re.subn(
        r'"NTDDI_VERSION=NTDDI_WIN11_[A-Z0-9_]+"',
        f'"NTDDI_VERSION={ntddi_name}"',
        win_text,
        count=1,
    )
    if replacements != 1:
        raise RuntimeError(f"NTDDI_VERSION was not found in {win_build}")
    win_build.write_text(win_text, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: configure_webrtc.py WEBRTC_ROOT", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    expose_target(root)
    expose_tflite_op_resolver(root)
    use_available_sdk(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
