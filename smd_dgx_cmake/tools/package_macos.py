"""Bundle an existing Release build; invoke with `uv run ... python`.

Usage: uv run --no-project python smd_dgx_cmake/tools/package_macos.py
Defaults to .setup/install/bin/smd_dgx_qt6 and .setup/qt-6.8.2.
If the Qt installation lacks LICENSES, pass the matching qtbase source
LICENSES directory with --qt-licenses.
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
    deps = [line.strip().split(" (compatibility", 1)[0]
            for line in run("otool", "-L", binary).splitlines()[1:]]
    # otool -L prints LC_ID_DYLIB first for libraries. It is an identity,
    # not a load command, and may still name the original build directory.
    identity = run("otool", "-D", binary).splitlines()[1:]
    if identity and deps and deps[0] == identity[0].strip():
        deps.pop(0)
    return deps


def rpaths(binary):
    lines = run("otool", "-l", binary).splitlines()
    return [lines[i + 2].strip().split(" (offset", 1)[0].removeprefix("path ")
            for i, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"]


def ensure_framework_rpath(app):
    executable = app / "Contents/MacOS/smd_dgx_qt6"
    path = "@executable_path/../Frameworks"
    if path not in rpaths(executable):
        run("install_name_tool", "-add_rpath", path, executable)


# A split Qt installation can expose optional plugins whose modules are not
# app dependencies. These widgets do not use SVG icons, PDF images or a
# virtual keyboard; omit those plugins instead of shipping extra modules.
OPTIONAL_PLUGINS = ("iconengines/libqsvgicon.dylib", "imageformats/libqpdf.dylib",
                    "platforminputcontexts/libqtvirtualkeyboardplugin.dylib")


def prune_optional_plugins(app):
    for relative in OPTIONAL_PLUGINS:
        (app / "Contents/PlugIns" / relative).unlink(missing_ok=True)


def bundle_binaries(app):
    entries = [app / "Contents/MacOS/smd_dgx_qt6"]
    frameworks = app / "Contents/Frameworks"
    entries += sorted(frameworks.glob("*.dylib"))
    for framework in sorted(frameworks.glob("*.framework")):
        binary = framework / framework.stem
        if binary.is_file():
            entries.append(binary.resolve())
    entries += sorted((app / "Contents/PlugIns").rglob("*.dylib"))
    for binary in entries:
        if not binary.resolve().is_relative_to(app.resolve()):
            raise RuntimeError(f"Binary escapes bundle: {binary}")
    return entries


def sanitize_rpaths(app, binaries):
    app = app.resolve()
    executable_dir = app / "Contents/MacOS"
    for binary in binaries:
        for value in rpaths(binary):
            target = None
            for token, base in [("@loader_path", binary.parent),
                                ("@executable_path", executable_dir)]:
                if value == token or value.startswith(token + "/"):
                    target = (base / value[len(token):].lstrip("/")).resolve()
                    break
            if target is None or not target.is_relative_to(app):
                run("install_name_tool", "-delete_rpath", value, binary)


def audit(app):
    app = app.resolve()
    executable = app / "Contents/MacOS/smd_dgx_qt6"
    entries = bundle_binaries(app)
    verified = []
    seen = set()

    def local_path(value, binary):
        for token, base in [("@loader_path", binary.parent),
                            ("@executable_path", executable.parent)]:
            if value == token or value.startswith(token + "/"):
                target = (base / value[len(token):].lstrip("/")).resolve()
                if not target.is_relative_to(app):
                    raise RuntimeError(f"Runtime path escapes bundle: {value} in {binary}")
                return target
        raise RuntimeError(f"Unsupported runtime path in {binary}: {value}")

    def check(binary, inherited):
        binary = binary.resolve()
        if not binary.is_relative_to(app):
            raise RuntimeError(f"Dependency escapes bundle: {binary}")
        if binary in seen:
            return
        seen.add(binary)
        verified.append(binary)
        deps = dependencies(binary)
        paths = rpaths(binary)
        if binary == executable and any(dep.startswith("@rpath/") for dep in deps):
            if not {"@executable_path/../Frameworks", "@loader_path/../Frameworks"}.intersection(paths):
                raise RuntimeError(f"Executable missing bundle Frameworks rpath: {binary}")
        own = []
        for value in paths:
            if value.startswith("/"):
                raise RuntimeError(f"Absolute runtime search path in {binary}: {value}")
            own.append(local_path(value, binary))
        search = list(dict.fromkeys(own + inherited))
        for dep in deps:
            if dep.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dep.startswith("@rpath/"):
                candidates = [(base / dep[len("@rpath/"):]).resolve() for base in search]
            elif dep.startswith(("@loader_path/", "@executable_path/")):
                candidates = [local_path(dep, binary)]
            else:
                raise RuntimeError(f"External dependency in {binary}: {dep}")
            for target in candidates:
                if not target.is_relative_to(app):
                    raise RuntimeError(f"Dependency escapes bundle: {dep} in {binary}")
                if target.is_file():
                    check(target, search)
                    break
            else:
                raise RuntimeError(f"Unresolved dependency in {binary}: {dep}")

    main_paths = [local_path(value, executable) for value in rpaths(executable)
                  if not value.startswith("/")]
    for binary in entries:
        check(binary, main_paths)
    return verified


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / ".setup/install/bin/smd_dgx_qt6")
    parser.add_argument("--qt", type=Path, default=ROOT / ".setup/qt-6.8.2")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/SMD DGX.app")
    parser.add_argument("--qt-licenses", type=Path)
    parser.add_argument("--sdl3", type=Path, default=Path("/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib"))
    args = parser.parse_args()
    app = args.output.resolve()
    if not app.is_relative_to(ROOT) or app.suffix != ".app":
        parser.error("Output must be a .app inside the workspace")
    if app.exists():
        parser.error(f"Refusing to overwrite existing bundle: {app}")
    if not args.executable.is_file() or not (args.qt / "bin/macdeployqt").is_file():
        parser.error("Build/install the executable and Qt first")
    if not (args.qt_licenses or args.qt / "LICENSES").is_dir() and \
            not (ROOT / ".setup/deps/qtbase-everywhere-src-6.8.2/LICENSES").is_dir():
        parser.error("Qt license texts not found; pass --qt-licenses")
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
    prune_optional_plugins(app)
    # Homebrew SDL2 is sdl2-compat: SDL3 is dlopen'ed, not in otool's dependency list.
    # Its macOS loader searches @loader_path/libSDL3.dylib first.
    if any("libSDL2" in p.name for p in frameworks.glob("*.dylib")):
        if not args.sdl3.is_file():
            raise RuntimeError("SDL2-compat requires the SDL3 runtime; supply --sdl3")
        shutil.copy2(args.sdl3, frameworks / "libSDL3.dylib")
        os.chmod(frameworks / "libSDL3.dylib", 0o755)
        run("install_name_tool", "-id", "@rpath/libSDL3.dylib", frameworks / "libSDL3.dylib")
    binaries = bundle_binaries(app)
    # macdeployqt can retain absolute build paths or loader-relative Homebrew
    # fallbacks. Neither may escape the relocatable bundle.
    sanitize_rpaths(app, binaries)
    # A fresh CMake install can have no LC_RPATH at all. Relative dependency
    # names are not sufficient: provide their resolver before auditing/signing.
    ensure_framework_rpath(app)
    (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\n")
    shutil.copy2(ROOT / "LICENSE.txt", resources / "Genesis-Plus-GX-LICENSE.txt")
    qt_licenses = args.qt_licenses or args.qt / "LICENSES"
    if qt_licenses.is_dir():
        shutil.copytree(qt_licenses, resources / "Qt-LICENSES")
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
