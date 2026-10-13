# Serika PhotoEdit

An open-source desktop photo editor built with C++20 and Qt 6.8 Widgets. Serika combines a native, dockable interface with layered editing, high-precision RGB, masks and familiar Photoshop-style shortcuts.

**Version 0.0.1 is an early release.** It provides working editing workflows, with substantial limits in specialist tools and interchange. See the [implementation status](docs/IMPLEMENTATION-STATUS.md) and [format fidelity notes](src/io/README.md).

The current source includes an unreleased [Serika brand refresh](docs/BRANDING.md) and an expanded editing workflow: multi-layer operations, editable Smart Objects, brush dynamics, per-channel Curves and real gradient/pattern fill editors. It also adds [GPU processing](docs/GPU-PROCESSING.md), [ICC CMYK proofing/export](docs/COLOR-MANAGEMENT.md), [rich typography](docs/TYPOGRAPHY.md) and a [local MCP server](docs/MCP-SERVER.md). Published v0.0.1 downloads retain the original interface and feature set. See the [new editing workflows](docs/EDITING-WORKFLOWS.md).

![Serika PhotoEdit editor](docs/screenshots/editor.png)

## Download

Downloads belong to the existing [v0.0.1 release](https://github.com/serika-dev/SerikaPhotoEdit/releases/tag/v0.0.1). See that live release page for download availability and the [current validation record](https://github.com/serika-dev/SerikaPhotoEdit/blob/main/docs/RELEASE-VALIDATION.md) for completed package checks.

| Platform | Downloads | Requirements |
| --- | --- | --- |
| Windows x64 | [Portable ZIP](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-win64.zip) · [MSI](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-win64.msi) | Windows 11 tested; executable and installer unsigned |
| macOS Intel | [DMG](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-macos-x86_64.dmg) · [ZIP](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-macos-x86_64.zip) | macOS 15+; native x86_64, ad-hoc signed |
| macOS Apple Silicon | [DMG](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-macos-arm64.dmg) · [ZIP](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-macos-arm64.zip) | macOS 14+; native arm64, ad-hoc signed |
| Linux x86_64 | [AppImage](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-linux-x86_64.AppImage) · [tar.gz](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-linux-x86_64.tar.gz) · [DEB](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-linux-amd64.deb) | Ubuntu 24.04/glibc 2.39 baseline |
| Linux aarch64 | [AppImage](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-linux-aarch64.AppImage) · [tar.gz](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-linux-aarch64.tar.gz) · [DEB](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SerikaPhotoEdit-0.0.1-linux-arm64.deb) | Ubuntu 24.04/glibc 2.39 baseline |

- [Source ZIP](https://github.com/serika-dev/SerikaPhotoEdit/archive/refs/tags/v0.0.1.zip) or [source tarball](https://github.com/serika-dev/SerikaPhotoEdit/archive/refs/tags/v0.0.1.tar.gz).
- [SHA-256 checksums](https://github.com/serika-dev/SerikaPhotoEdit/releases/download/v0.0.1/SHA256SUMS.txt) for published downloads.

Windows: extract the entire ZIP and open `bin/SerikaPhotoEdit.exe`, or use the MSI with its optional SPE/PSD/PSB file-association feature. Runtime libraries are included. MSI payload extraction was checked; normal installation, association changes and uninstall still need acceptance testing.

macOS: choose the package matching your processor, open the DMG and drag `SerikaPhotoEdit.app` to Applications, or extract the ZIP. Bundles include Qt and application dependencies and use an ad-hoc signature; they are not Developer ID signed or notarized. macOS may require approval under System Settings → Privacy & Security; see [Apple's opening instructions](https://support.apple.com/en-us/102445).

Linux: make the AppImage executable and run it, or extract the tar.gz and run its top-level `SerikaPhotoEdit` launcher. Install a matching DEB with `sudo apt install ./SerikaPhotoEdit-0.0.1-linux-amd64.deb` (use `arm64` for ARM). These packages bundle Qt and application dependencies; glibc, graphics drivers, a desktop session and fonts remain host requirements. If FUSE is unavailable, an AppImage can run with `--appimage-extract-and-run`. Other distributions and older glibc are unverified. [Release notes](docs/releases/0.0.1.md) record the package checks and limits.

## Start editing

Home includes the original layered **Amber Valley** demo. Open an image, add a layer, choose Brush (`B`), paint, and undo with `Ctrl+Z`. Save layered work as **SPE**; use File → Export for a flattened image. Keep SPE as your master document: PSD export uses raster compatibility layers and private Serika tags for some editable fields.

Crop (`C`) previews changes until Enter applies them; Escape cancels. Free Transform (`Ctrl+T`) follows the same commit/cancel pattern. Space temporarily pans, and Alt samples a brush color. Shortcuts can be remapped and exported; see the [keyboard reference](docs/KEYBOARD-SHORTCUTS.md).

## Features

- Document tabs, detachable windows, dockable panels, workspaces, command palette, recent files and four themes with original Serika icons.
- Sparse 256 × 256 tiles; 8-bit, 16-bit and float RGB; copy-on-write history; pixel, text, shape, group, adjustment and smart-object layers.
- CPU compositing with 27 blend modes, clipping, group isolation/pass-through, adjustments and layer effects.
- Brush, eraser, clone/heal, gradient/fill, selection and path tools; text and vector shapes; guides, rulers, zoom/pan and interactive Liquify.
- Raster/vector masks with density, feather and linking; high-precision selection coverage; Select and Mask previews, edge brush and output modes; saved selection channels and layer comps.
- Crop and perspective crop previews, ratios, output size, overlays, straighten and automatic bounds detection; inline transforms; Patch and Content-Aware Move with actual selection transport.
- Checksummed, atomically saved SPE/SPEB; original PSD/PSB import/export; LibRaw RAW import and Develop; common raster formats, TGA, float HDR, SVG rasterization and PDF page import.
- CPU filters, retained-source smart-filter stacks, selectable History Brush sources, Fade, action recording/playback and headless folder batch processing.
- Configurable Photoshop-style shortcuts with conflict detection, tool scopes, held modifiers, tool cycles and JSON import/export.
- Optional OpenGL shader adjustments/blur with CPU fallback; ICC CMYK proofing and 8/16-bit TIFF/separation export; rich text spans, paragraph layout and type on paths.
- AI agents can drive the editor through MCP (protocol 2026-07-28): live sessions edit the open window with full undo, with tools for layers, masks, text, image placement, previews, actions and export.
- Subject selection and background removal with a fast built-in engine or a local BiRefNet AI model through ONNX Runtime (optional, GPU via DirectML on Windows). Nothing is uploaded.
- Multi-layer alignment/distribution and group operations; editable embedded/linked Smart Objects; brush dynamics with native preset libraries; draggable RGB/channel Curves; five gradient styles and tiled pattern fills.

## Build from source

Requires **CMake 3.25+**, a **C++20 compiler**, Ninja or another CMake generator, and **Qt 6.8+** with Core, Gui, Widgets, PrintSupport, Svg and Test. Qt Image Formats supplies optional image codecs; Qt Pdf enables PDF import. Dynamically linked LibRaw, zlib and LittleCMS2 are detected when available. Qt OpenGL enables optional GPU processing. Install `liblcms2-dev` on Debian/Ubuntu, `little-cms2` with Homebrew, or run `scripts/build-lcms.ps1` on Windows and add its install directory to `CMAKE_PREFIX_PATH`. Use `SERIKA_REQUIRE_CMYK=ON` to require ICC proofing/export support, or `SERIKA_WITH_GPU=OFF` for a CPU-only build. RAW import is unavailable without LibRaw; `SERIKA_WITH_RAW=OFF` explicitly disables its discovery.

```sh
git clone https://github.com/serika-dev/SerikaPhotoEdit.git
cd SerikaPhotoEdit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt-kit-directory>" -DSERIKA_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use a Qt kit and dependencies matching your compiler and architecture. On Windows, put the Qt runtime and matching compiler on PATH; an MSVC build needs a Visual Studio developer shell. The tested configuration is Qt 6.8.3, MinGW GCC 13.1, LibRaw 0.22.2 and zlib 1.3.1, all x64. [CONTRIBUTING.md](CONTRIBUTING.md) provides platform-specific setup, test and packaging commands.

## Platform coverage

| Platform | Build/package route | Verified for 0.0.1 |
| --- | --- | --- |
| Windows x64 | Qt desktop kit, CMake/Ninja; portable ZIP and WiX 3 MSI | Windows 11 x64 native/package checks; Windows Server 2022 CI build and offscreen tests |
| Windows ARM64 | Matching ARM64 Qt kit and compiler | No build or device validation |
| macOS Intel / Apple Silicon | Native x86_64 and arm64 app bundles, DMG and ZIP; `scripts/package-macos.sh` | Intel/macOS 15 and Apple Silicon/macOS 14 each passed 227 checks and ZIP/DMG batch, SPE reopen and Cocoa captures with build dependencies hidden |
| Linux x86_64 / aarch64 | Qt 6.8.3 portable tar.gz, AppImage and DEB; `scripts/package-linux.sh` | Native Ubuntu 24.04 builds, 227 checks each, batch save/reopen, XCB/Xvfb and package smoke checks passed |
| Alpine / FreeBSD | Native Qt 6.8+ and compiler; Alpine requires `SERIKA_ALPINE=ON` | Experimental, unverified |

Many distributions ship Qt older than 6.8, so check the selected SDK. [Earlier CI](https://github.com/serika-dev/SerikaPhotoEdit/actions/runs/37781612875) passed builds, offscreen tests and runtime staging on Windows Server 2022, macOS 14 and Ubuntu 24.04. The [Unix packaging workflow](.github/workflows/release-unix.yml) compiles the unchanged v0.0.1 application source with current packaging scripts, documentation and notices. Full interactive desktop acceptance, other OS versions, universal Mac binaries and distribution-wide compatibility remain unverified.

## Validation and limits

The current unreleased Windows x64 Release build passes **all 17 CTest suites: 383 QtTest checks, zero failures**. The headless platform skips 38 hardware GPU cases; a separate native AMD Radeon run passes **44 GPU checks**, with one expected unavailable-device branch skipped. Compiler warnings are treated as errors. New coverage includes ICC transforms/CMYK TIFF/separations, proofing menus/canvas, rich text persistence/undo, GPU precision/CPU parity, and live MCP protocol/file-access/rollback tests. Fourteen UI captures inspect dark/light controls and an editable Smart Object tab with native UI settings unchanged. The new Windows/Linux/macOS CI builds require LittleCMS; results for this source revision are pending.

The published **v0.0.1** Windows build passed the original eight suites and **227 checks**: 79 core, 37 I/O, 8 UI, 34 canvas, 13 actions, 14 shortcuts, 21 masks and 21 workflows. Counts include lifecycle slots and data-driven rows. Native UI, portable launch, headless batch, ZIP integrity and MSI administrative extraction were also checked. Native Linux x86_64/aarch64 and macOS Intel/Apple Silicon builds each passed the same 227 checks without failures or skips, plus their package smoke checks. See the [release validation record](docs/RELEASE-VALIDATION.md) and [earlier benchmark measurements](docs/performance.json).

Serika does not have full Photoshop parity. Content-aware tools use local CPU algorithms; comprehensive Adobe descriptors, Adobe typography interchange, native CMYK/Lab layer editing, spot colors, advanced multi-layer transforms and a full GPU compositor remain absent or limited. ICC proofing/export needs a suitable printer profile; no proprietary press profiles are bundled. Embedded Smart Object documents use Serika's native format; other editors receive raster PSD fallbacks. Many operations allocate full images despite the tiled model; canvas creation is capped at 80 million pixels. Test results cover the exercised behaviors, rather than every image, camera or external PSD/PSB file.

## Headless actions

Actions are JSON documents with a `steps` array. The [included example](examples/web-export.speaction) resizes images and applies an exposure adjustment:

```powershell
./bin/SerikaPhotoEdit.exe --batch ./share/serika-photoedit/examples/web-export.speaction input-folder output-folder
```

Run this command from the extracted portable package. Batch writes SPE files, reports invalid parameters or unsupported commands, and rolls back failed playback. GUI recording captures supported commands and accepted settings, rather than arbitrary pointer gestures.

On Linux, pass the same arguments to the package's `SerikaPhotoEdit` launcher. On macOS, the executable is `SerikaPhotoEdit.app/Contents/MacOS/SerikaPhotoEdit`.

## For agents and contributors

[AGENTS.md](AGENTS.md) is the guide for AI coding agents and humans alike: build and test commands,
architecture, conventions and the release process. Versions, branches and the changelog are covered in
[docs/RELEASING.md](docs/RELEASING.md).

## Contribute and license

Bug reports and focused pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and useful report details, and [CHANGELOG.md](CHANGELOG.md) for release history.

Application code and original Serika icons are [MIT licensed](LICENSE). Qt, LibRaw, codecs and compiler runtimes retain their own licenses; see [third-party notices](docs/THIRD-PARTY.md) and the bundled license texts.
