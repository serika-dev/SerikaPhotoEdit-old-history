# Changelog

All notable changes to Serika PhotoEdit are recorded here. The format follows
[Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Write entries for users, under
`[Unreleased]`, in the same pull request as the change. See [docs/RELEASING.md](docs/RELEASING.md).

## [Unreleased]

### Added

- AI background removal and subject selection on this computer: BiRefNet (MIT) through ONNX Runtime, loaded only when present, with DirectML GPU acceleration when that runtime is installed. Preferences › AI picks the engine, shows its status and downloads the model with checksum verification.
- A low-resource Fast engine remains the default; Select › Subject (AI), Layer › Remove Background and Layer › Remove Background (AI) choose explicitly.
- Live MCP sessions: `SerikaPhotoEdit --mcp --attach --mcp-root <folder>` lets AI agents edit the documents open in the window, with every change on screen and in the undo history. Serika starts if it is not running, the title bar shows when an agent is connected, and Preferences › AI can turn agent access off.
- MCP protocol 2026-07-28 (stateless requests, `server/discover`, cache hints, version errors) alongside the 2025 handshake for existing clients.
- MCP tools for subject selection, background removal, layer masks, image placement, duplicating, deleting, grouping and restacking layers, single-layer previews, history, AI status and a documented action catalog.
- Photoshop-style Layers panel: an eye column (click, drag down to toggle many, Alt-click to solo), lock and footer icons, folder icons for groups, checkerboard thumbnails and a type glyph for text layers.
- Adjustments panel icons for the sixteen Photoshop adjustments, with the rest under More adjustments.
- Redrawn tool icons with a distinct glyph for every tool and Photoshop's corner triangle on toolbar slots that hold several tools, plus overlapping foreground/background colour chips.
- Optional GPU (OpenGL 3.3) adjustments and Gaussian blur with CPU fallback, a persistent preference and device diagnostics.
- LittleCMS ICC CMYK soft proofing, intents and black point compensation, paper simulation, gamut warnings, and 8/16-bit CMYK TIFF and ink separations. Layer editing remains RGB.
- Editable mixed typography, paragraph metrics, text on paths and shared render/outline layout.
- Native stdio MCP server with workspace access controls, isolated documents, previews, layers, native saves, atomic actions and undo/redo.
- Home screen featuring the PhotoEdit mascot, with matching Windows, Linux and macOS application icons.
- Multi-layer alignment, distribution, grouping, duplication, deletion and stack ordering, with stable selections and undo.
- Editable embedded Smart Object documents, linked placement/reload/relink, replacement and export, with retained transforms, masks and Smart Filters.
- Brush dynamics, deterministic stroke spacing, pressure/tilt controls, stroke previews and Serika brush preset import/export.
- Draggable RGB and per-channel Curves, histogram overlays, numeric point editing and curve presets.
- Pattern Fill layers and five gradient styles with multiple colour/opacity stops, plus an editable Gradient Map ramp.
- MCP layout tools for precise designs: text laid out in a box (wrapped or kept line by line, aligned by its ink and centred optically, optionally shrunk to fit), `measure_text`, `align_layers`, `check_layout` and full-resolution `region` previews, plus layout rules in the server instructions. `inspect_document` reports every layer's ink bounds, and text layers can be restyled.
- `serika_iconsheet` and `serika_psdcheck` developer tools, `scripts/psd_inspect.py` and `scripts/release.py`.

### Changed

- Select subject / Remove background / Add mask is a floating bar near the bottom of the canvas and is off by default (View › Contextual Task Bar).
- Essentials uses Photoshop's three panel stacks, with Properties as a tab beside Adjustments, so Layers keeps its room on short screens. Workspace layouts saved by earlier builds are reset once.
- Icons are drawn as vectors at the display's scale, so they stay sharp at 125 % and 150 %.
- The PSD import report lists only features imported as pixels, once per layer, instead of every preserved metadata block.
- Serika brand refresh: violet accents, plum and lavender surfaces, and bundled Outfit and Onest typefaces across all four themes, with visible keyboard focus and neutral image surrounds.
- Multi-layer Move and keyboard nudging preserve selections and unlinked masks, with gesture cancellation and atomic undo.
- The version is defined once in CMake and generated into the application.

### Fixed

- PSD files from Photoshop opened upside down with empty groups, because layer records were read top-to-bottom; PSDs saved by Serika had the mirror-image problem in Photoshop. Files saved by earlier Serika builds still open correctly.
- Hidden layers could not be shown again: the dark theme drew nothing for an unchecked visibility box.
- Dragging layers in the Layers panel could reorder unpredictably or nest layers inside non-group layers.
- Remove Background used the whole document instead of the target layer, so a placed photo kept its background.
- The Adjustments panel's two-letter labels were ambiguous (three buttons read "Au").
- Editing documents with many layers was very slow: every change re-rendered each layer through full-canvas images. Plain, masked and text layers and full-strength pass-through groups now render directly, with identical pixels; a 63-layer cover dropped from about 5 seconds to 0.3 seconds per change.
- Adding to, subtracting from or intersecting a selection on a large document took about a second per operation; long MCP action lists froze the window until Windows closed it as not responding.
- MCP `place_image` ignored a single `width` or `height` instead of scaling proportionally.

## [0.0.1] - 2026-10-08

Initial public release under the MIT license, with source and Windows x64 portable ZIP/MSI downloads. The same release also provides verified Linux x86_64/aarch64 tar.gz, AppImage and DEB packages and native macOS Intel/Apple Silicon DMG/ZIP packages, using unchanged v0.0.1 application source.

### Added

- Native Qt Widgets editor with tabs, detachable windows, dockable panels, workspaces, command palette and four themes.
- High-precision RGB layers, selections and linked raster/vector masks; groups, adjustments, layer effects, smart objects and copy-on-write undo.
- Brush/eraser, clone/heal, fill/gradient, selection, shape, text and path editing, guides/rulers and Liquify.
- Crop/perspective previews with commit/cancel, ratios, output size, overlays, straighten and automatic bounds detection; inline transforms, Patch and Content-Aware Move.
- Select and Mask refinement, saved selections, layer comps, selected History Brush sources, Fade and editable retained-source smart-filter stacks.
- Remappable Photoshop-style shortcuts, conflicts, held modifiers, tool cycles and JSON import/export.
- Native SPE/SPEB, PSD/PSB interchange, LibRaw RAW Develop and raster/HDR/SVG/PDF import workflows.
- Actions, parameter playback and headless folder batch processing.

**Validation and known limits.** The Windows 11 x64 Release build passed all eight CTest suites (227 QtTest checks). Portable GUI/batch smoke checks, ZIP integrity and MSI administrative extraction passed; interactive installation, file associations and uninstall remain unverified. Native Ubuntu 24.04 x86_64/aarch64, macOS 15 Intel and macOS 14 Apple Silicon builds each passed all 227 checks, with Linux package and Mac ZIP/DMG smoke checks. The Windows executable and MSI are unsigned; Mac bundles use ad-hoc signatures without notarization (minimums macOS 15.0 Intel, 14.0 Apple Silicon). Windows ARM64 remains unverified. This early release does not provide full Photoshop parity.

See the [0.0.1 release notes](docs/releases/0.0.1.md), [validation record](docs/RELEASE-VALIDATION.md) and [implementation status](docs/IMPLEMENTATION-STATUS.md) for details.

[Unreleased]: https://github.com/serika-dev/SerikaPhotoEdit/compare/v0.0.1...HEAD
[0.0.1]: https://github.com/serika-dev/SerikaPhotoEdit/releases/tag/v0.0.1
