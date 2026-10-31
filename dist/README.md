# SMD DGX for macOS

`SMD DGX.app` is a relocatable, self-contained **Apple Silicon** application (~86 MB). Copy the whole `.app`, not only the executable inside it. Qt 6.12 frameworks, their Cocoa/image/style plugins, SDL2-compat, and its dynamically loaded SDL3 runtime are bundled. Homebrew, the source workspace, and IDA are not needed to run it.

**Requires macOS 27.0 or newer.** This is the minimum recorded in the existing executable and bundled SDL3 build; packaging does not lower it. Intel Macs and older macOS releases require a separate compatible build.

Double-click the app and use its ROM-open UI, or launch with a ROM argument:

```bash
"/wherever/SMD DGX.app/Contents/MacOS/smd_dgx_qt6" "/path/to/Pocahontas.md"
```

The bundle is locally **ad-hoc signed**, not Developer-ID signed or notarized. macOS may require explicit approval if it arrives via a quarantined download. No Gatekeeper policy was changed by packaging.

Licenses are included under `Contents/Resources`; the Genesis Plus GX license is non-commercial.

## Rebuild within this workspace

```bash
source .setup/env.sh
uv run --no-project --python /opt/homebrew/bin/python3.11 python smd_dgx_cmake/tools/package_macos.py \
  --executable "$PWD/.setup/install-ida95/bin/smd_dgx_qt6" --qt /opt/homebrew/opt/qt \
  --qt-licenses "$PWD/.setup/deps/qtbase-6.12.0-licenses/LICENSES"
```

Homebrew Qt does not include license texts; `--qt-licenses` supplies the matching qtbase `LICENSES` directory. Unused optional SVG-icon, PDF-image and virtual-keyboard plugins are omitted.

The packager refuses to overwrite an existing output. Use `--output "$PWD/dist/Another Name.app"` for a fresh destination. It copies the installed standalone binary and matching Qt, deploys linked libraries/plugins, adds SDL3's hidden dlopen dependency, removes external runtime search paths, derives the minimum macOS version, audits linkage, and ad-hoc signs/verifies the result.

## Verification

`.setup/logs/ida95-package.log`: deployment and signature verification.

`.setup/logs/ida95-bundle-verification.log` and `.setup/relocation/result.json`: copied to a different directory and renamed, launched with no Qt/SDL/dyld search overrides, loaded Pocahontas, paused, advanced 360 frames, read both CPUs' registers and ROM bytes, and obtained a nonzero 320x224 framebuffer. dyld trace proved Qt, Cocoa plugin, SDL2, and SDL3 loaded from the relocated bundle, with no Homebrew or original workspace Qt paths. Every bundled Mach-O binary was checked for external dependency/search paths, and the relocated signature passed `codesign --verify --deep --strict`.
