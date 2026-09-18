#!/usr/bin/env python3
"""Package the tested free-Wine installation as a portable macOS app."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'build/package/AP2700.app'
RES = APP / 'Contents/Resources'


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main():
    # Refuse to overwrite an existing package; archive/move it before rebuilding.
    APP.mkdir(parents=True, exist_ok=False)
    RES.mkdir(parents=True)
    (APP / 'Contents/MacOS').mkdir()
    (APP / 'Contents/Frameworks').mkdir()
    shutil.copytree(ROOT / 'build/wine-runtime/Wine Stable.app', RES / 'Wine Stable.app', symlinks=True)
    template = RES / 'prefix-template'
    shutil.copytree(ROOT / 'build/wine-prefix', template, symlinks=True)
    # Remove external home links/drive mappings. Regular prefix state is retained:
    # this is a PRIVATE app builder, not a sanitization tool for public release.
    for path in template.rglob('*'):
        if path.is_symlink() and os.readlink(path).startswith('/Users/'):
            path.unlink()
            path.mkdir()
    for path in (template / 'dosdevices').iterdir():
        if path.name != 'c:':
            path.unlink()
    for name in ('.update-timestamp', '.wine-bootstrapped'):
        (template / name).unlink(missing_ok=True)
    shutil.copy2(ROOT / 'build/shim_smoke.exe', template / 'drive_c/AP2700/shim_smoke.exe')
    helper = RES / 'apusb-bridge'
    shutil.copy2(ROOT / 'build/apusb-bridge', helper)
    libdir = Path(subprocess.check_output(['brew', '--prefix', 'libusb'], text=True).strip())
    dylib = APP / 'Contents/Frameworks/libusb-1.0.0.dylib'
    shutil.copy2(libdir / 'lib/libusb-1.0.0.dylib', dylib)
    dylib.chmod(0o755)
    linked = subprocess.check_output(['otool', '-L', str(helper)], text=True)
    old_libusb = next(line.strip().split(' (')[0] for line in linked.splitlines()[1:] if 'libusb-1.0' in line)
    run('install_name_tool', '-change', old_libusb,
        '@executable_path/../Frameworks/libusb-1.0.0.dylib', helper)
    run('install_name_tool', '-id', '@executable_path/../Frameworks/libusb-1.0.0.dylib', dylib)
    run('codesign', '--force', '--sign', '-', dylib)
    run('codesign', '--force', '--sign', '-', helper)
    run('xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0',
        ROOT / 'packaging/Launcher.swift', '-o', APP / 'Contents/MacOS/AP2700')
    info = {
        'CFBundleName': 'AP2700', 'CFBundleDisplayName': 'AP2700',
        'CFBundleIdentifier': 'local.ap2700.usbbridge', 'CFBundleExecutable': 'AP2700',
        'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1', 'LSMinimumSystemVersion': '15.0',
        'NSHighResolutionCapable': True, 'LSMultipleInstancesProhibited': True,
        'NSHumanReadableCopyright': 'Local AP2700 Wine/USB bridge launcher. Vendor components retain their own licenses.'
    }
    with (APP / 'Contents/Info.plist').open('wb') as f:
        plistlib.dump(info, f)
    notices = RES / 'Documentation'
    notices.mkdir()
    shutil.copy2(libdir / 'COPYING', notices / 'libusb-COPYING.txt')
    for name in ('README.md', 'SETUP.md', 'CHECKPOINT.md'):
        if (ROOT / name).exists():
            shutil.copy2(ROOT / name, notices / name)
    (notices / 'COMPONENTS.txt').write_text(
        'Wine 11.0_1: https://github.com/Gcenx/macOS_Wine_builds/releases/tag/11.0_1\n'
        'Wine source/license: https://www.winehq.org/ (LGPL); upstream bundle retained.\n'
        'libusb 1.0.30: https://github.com/libusb/libusb/releases/tag/v1.0.30 (LGPL 2.1+)\n'
        'AP2700 3.30 SP2: user-provided Audio Precision installation; not freeware.\n'
        'Local bridge source and reproduction guide accompany this personal deployment.\n')
    run('codesign', '--force', '--sign', '-', APP)
    run('codesign', '--verify', '--deep', '--strict', APP)
    print(APP)


if __name__ == '__main__':
    main()
