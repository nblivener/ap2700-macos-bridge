#!/usr/bin/env python3
"""Prepare a copied AP2700 tree with apio.dll routed through apbridge.dll."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

import pefile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "research" / "installer" / "AP2700Setup"
DEFAULT_OUTPUT = ROOT / "build" / "ap2700"
BUILD_ROOT = ROOT / "build"
MARKER_NAME = ".apusb-generated"
HOOKS = {
    "KERNEL32.dll": {
        "CreateFileA": "AP_CreateFileA@28",
        "DeviceIoControl": "AP_DeviceIoControl@32",
        "CloseHandle": "AP_CloseHandle@4",
        "GetOverlappedResult": "AP_GetOverlappedResult@16",
    },
    "SETUPAPI.dll": {
        "SetupDiGetClassDevsA": "AP_SetupDiGetClassDevsA@16",
        "SetupDiEnumDeviceInterfaces": "AP_SetupDiEnumDeviceInterfaces@20",
        "SetupDiGetDeviceInterfaceDetailA": "AP_SetupDiGetDeviceInterfaceDetailA@24",
        "SetupDiDestroyDeviceInfoList": "AP_SetupDiDestroyDeviceInfoList@4",
    },
}
REDIRECT_NAME = b"apbridge.dll"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def imports(pe: pefile.PE) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for descriptor in pe.DIRECTORY_ENTRY_IMPORT:
        dll = descriptor.dll.decode("ascii")
        names: list[str] = []
        for symbol in descriptor.imports:
            if symbol.name is None:
                raise RuntimeError(f"ordinal import from {dll} is unsupported")
            names.append(symbol.name.decode("ascii"))
        result[dll] = names
    return result


def patch_import_dll_names(path: Path) -> dict[str, list[str]]:
    pe = pefile.PE(str(path), fast_load=False)
    imported = imports(pe)
    image = bytearray(path.read_bytes())
    patched: set[str] = set()
    for descriptor in pe.DIRECTORY_ENTRY_IMPORT:
        dll = descriptor.dll.decode("ascii")
        if dll not in HOOKS:
            continue
        original = descriptor.dll
        if len(original) != len(REDIRECT_NAME):
            raise RuntimeError(f"cannot in-place replace {dll!r} with apbridge.dll")
        offset = pe.get_offset_from_rva(descriptor.struct.Name)
        if image[offset : offset + len(original)] != original:
            raise RuntimeError(f"PE import name offset mismatch for {dll}")
        image[offset : offset + len(original)] = REDIRECT_NAME
        patched.add(dll)
    pe.close()
    missing = set(HOOKS) - patched
    if missing:
        raise RuntimeError(f"required import descriptors not found: {sorted(missing)}")
    path.write_bytes(image)
    return imported


def make_def(imported: dict[str, list[str]], output: Path) -> None:
    lines = ["LIBRARY apbridge", "EXPORTS"]
    for dll in ("KERNEL32.dll", "SETUPAPI.dll"):
        for name in imported[dll]:
            target = HOOKS[dll].get(name)
            if target is not None:
                lines.append(f"    {name}={target}")
            else:
                lines.append(f"    {name}={dll.removesuffix('.dll')}.{name}")
    output.write_text("\n".join(lines) + "\n", encoding="ascii")


def compile_bridge(output_dir: Path, def_file: Path, compiler: str) -> None:
    command = [
        compiler,
        "-std=c11",
        "-Os",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-shared",
        str(ROOT / "shim" / "apbridge.c"),
        str(def_file),
        "-o",
        str(output_dir / "apbridge.dll"),
        "-lws2_32",
        "-lsetupapi",
    ]
    subprocess.run(command, check=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--compiler", default="i686-w64-mingw32-gcc")
    parser.add_argument(
        "--exclude-drivers",
        action="store_true",
        help="omit the Driver and Driver64 directories from the copied runtime tree",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    build_root = BUILD_ROOT.resolve()
    original_dll = source / "apio.dll"
    if not original_dll.is_file():
        raise RuntimeError(f"missing source DLL: {original_dll}")
    if output == build_root or build_root not in output.parents:
        raise RuntimeError(f"output must be a child of {build_root}")
    if source == output or source in output.parents or output in source.parents:
        raise RuntimeError("source and output trees must not overlap")

    original_before = sha256(original_dll)
    ignore = shutil.ignore_patterns("Driver", "Driver64") if args.exclude_drivers else None
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        marker = output / MARKER_NAME
        if not marker.is_file():
            raise RuntimeError(f"refusing to reuse unmarked output directory: {output}")
        shutil.copytree(source, output, ignore=ignore, dirs_exist_ok=True)
    else:
        shutil.copytree(source, output, ignore=ignore)
        (output / MARKER_NAME).write_text("generated by tools/prepare_shim.py\n", encoding="ascii")

    copied_dll = output / "apio.dll"
    if sha256(copied_dll) != original_before:
        raise RuntimeError("copied apio.dll does not match the source before patching")
    imported = patch_import_dll_names(copied_dll)
    def_file = output / "apbridge.def"
    make_def(imported, def_file)
    compile_bridge(output, def_file, args.compiler)
    original_after = sha256(original_dll)
    if original_after != original_before:
        raise RuntimeError("source apio.dll changed while preparing the shim")

    patched = pefile.PE(str(copied_dll), fast_load=False)
    redirected = [item.dll.decode("ascii") for item in patched.DIRECTORY_ENTRY_IMPORT]
    patched.close()
    if redirected.count("apbridge.dll") != 2:
        raise RuntimeError("patched apio.dll does not contain exactly two apbridge.dll imports")
    print(f"source apio.dll sha256: {original_before}")
    print(f"prepared runtime: {output}")
    print(f"patched DLL: {copied_dll}")
    print(f"shim DLL: {output / 'apbridge.dll'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"prepare_shim.py: {error}", file=sys.stderr)
        raise SystemExit(1)
