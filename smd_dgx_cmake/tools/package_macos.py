"""Bundle an existing Release build; invoke with `uv run ... python`.

Usage: uv run --no-project python smd_dgx_cmake/tools/package_macos.py
Defaults to .setup/install/bin/smd_dgx_qt6 and .setup/qt-6.8.2.
All outputs must stay within the repository. Existing bundles are not overwritten.
"""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True, stderr=subprocess.STDOUT)


def dependencies(binary):
    return [line.strip().split(" (compatibility", 1)[0]
            for line in run("otool", "-L", binary).splitlines()[1:]]


def rpaths(binary):
    lines = run("otool", "-l", binary).splitlines()
    return [lines[i + 2].strip().split(" (offset", 1)[0].removeprefix("path ")
            for i, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"]


def audit(app):
    binaries = [app / "Contents/MacOS/smd_dgx_qt6"]
    binaries += sorted((app / "Contents/Frameworks").glob("*.dylib"))
    binaries += sorted((app / "Contents/PlugIns").rglob("*.dylib"))
    for binary in binaries:
        for dep in dependencies(binary):
            if dep.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if not dep.startswith(("@rpath/", "@loader_path/", "@executable_path/")):
                raise RuntimeError(f"External dependency in {binary}: {dep}")
        for path in rpaths(binary):
            if path.startswith("/"):
                raise RuntimeError(f"Absolute runtime search path in {binary}: {path}")
    return binaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / ".setup/install/bin/smd_dgx_qt6")
    parser.add_argument("--qt", type=Path, default=ROOT / ".setup/qt-6.8.2")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/SMD DGX.app")
    parser.add_argument("--sdl3", type=Path, default=Path("/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib"))
    args = parser.parse_args()
    app = args.output.resolve()
    if not app.is_relative_to(ROOT) or app.suffix != ".app":
        parser.error("Output must be a .app inside the workspace")
    if app.exists():
        parser.error(f"Refusing to overwrite existing bundle: {app}")
    if not args.executable.is_file() or not (args.qt / "bin/macdeployqt").is_file():
        parser.error("Build/install the executable and Qt first")
    for key, relative in {"TMPDIR": ".setup/tmp", "HOME": ".setup/home",
                          "XDG_CACHE_HOME": ".setup/cache", "CFFIXED_USER_HOME": ".setup/home"}.items():
        path = ROOT / relative
        path.mkdir(parents=True, exist_ok=True)
        os.environ[key] = str(path)
    macos = app / "Contents/MacOS"
    frameworks = app / "Contents/Frameworks"
    resources = app / "Contents/Resources"
    for directory in (macos, frameworks, resources):
        directory.mkdir(parents=True)
    shutil.copy2(args.executable, macos / "smd_dgx_qt6")
    with (app / "Contents/Info.plist").open("wb") as out:
        plistlib.dump({"CFBundleExecutable": "smd_dgx_qt6", "CFBundleName": "SMD DGX",
                      "CFBundleDisplayName": "SMD DGX", "CFBundleIdentifier": "org.smd-dgx.debugger",
                      "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0",
                      "CFBundleVersion": "1", "NSHighResolutionCapable": True}, out)
    print(run(args.qt / "bin/macdeployqt", app, "-verbose=2", "-always-overwrite"))
    # Homebrew SDL2 is sdl2-compat: SDL3 is dlopen'ed, not in otool's dependency list.
    # Its macOS loader searches @loader_path/libSDL3.dylib first.
    if any("libSDL2" in p.name for p in frameworks.glob("*.dylib")):
        if not args.sdl3.is_file():
            raise RuntimeError("SDL2-compat requires the SDL3 runtime; supply --sdl3")
        shutil.copy2(args.sdl3, frameworks / "libSDL3.dylib")
        os.chmod(frameworks / "libSDL3.dylib", 0o755)
        run("install_name_tool", "-id", "@rpath/libSDL3.dylib", frameworks / "libSDL3.dylib")
    binaries = [macos / "smd_dgx_qt6"] + list(frameworks.glob("*.dylib"))
    binaries += list((app / "Contents/PlugIns").rglob("*.dylib"))
    for binary in binaries:
        # macdeployqt can retain original build/Homebrew rpaths. No external fallbacks.
        for path in rpaths(binary):
            if path.startswith("/"):
                run("install_name_tool", "-delete_rpath", path, binary)
    (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\n")
    shutil.copy2(ROOT / "LICENSE.txt", resources / "Genesis-Plus-GX-LICENSE.txt")
    if (args.qt / "LICENSES").is_dir():
        shutil.copytree(args.qt / "LICENSES", resources / "Qt-LICENSES")
    for license_path, name in [(ROOT / ".setup/deps/qtbase-everywhere-src-6.8.2/LICENSES", "Qt-LICENSES"),
                               (Path("/opt/homebrew/opt/sdl2-compat/LICENSE.txt"), "SDL2-LICENSE.txt"),
                               (Path("/opt/homebrew/opt/sdl3/LICENSE.txt"), "SDL3-LICENSE.txt")]:
        destination = resources / name
        if license_path.is_dir() and not destination.exists():
            shutil.copytree(license_path, destination)
        elif license_path.is_file():
            shutil.copy2(license_path, destination)
    verified = audit(app)
    minimums = []
    for binary in verified:
        for line in run("otool", "-l", binary).splitlines():
            if line.strip().startswith("minos "):
                minimums.append(tuple(int(part) for part in line.split()[1].split(".")))
    if minimums:
        plist_path = app / "Contents/Info.plist"
        with plist_path.open("rb") as source:
            info = plistlib.load(source)
        info["LSMinimumSystemVersion"] = ".".join(map(str, max(minimums)))
        with plist_path.open("wb") as out:
            plistlib.dump(info, out)
    for binary in verified:
        run("codesign", "--force", "--sign", "-", binary)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--deep", "--strict", app)
    print(f"Created and ad-hoc signed: {app}")


if __name__ == "__main__":
    main()
