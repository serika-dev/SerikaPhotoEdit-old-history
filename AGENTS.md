# Serika PhotoEdit — guide for coding agents

A native, Photoshop-style layer editor in C++20 and Qt 6.8 Widgets (MIT). This file is the shared
brief for AI agents and humans; `CLAUDE.md` imports it. Read [CONTRIBUTING.md](CONTRIBUTING.md) for
platform setup and [docs/RELEASING.md](docs/RELEASING.md) for versions, branches and the changelog.

## Build and test

Windows (this repository's bundled toolchain lives in the ignored `.tools/`):

```bash
R=$PWD/.tools; export PATH="$R/Qt/6.8.3/mingw_64/bin:$R/Qt/Tools/mingw1310_64/bin:$R/deps/install/bin:$R/python/cmake/data/bin:$PATH"
cmake --build build --parallel 8            # configure first with scripts/build.ps1 if build/ is missing
(cd build && ctest -j4 --output-on-failure) # 17 suites, run headless (offscreen) by CTest
```

macOS/Linux: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt kit>
-DSERIKA_WARNINGS_AS_ERRORS=ON && cmake --build build && ctest --test-dir build --output-on-failure`.

- Warnings are errors. Every change ships with tests in `tests/<area>_tests.cpp` (QtTest); run a
  single suite with `build/<area>_tests.exe -platform offscreen` (set `QT_QPA_FONTDIR=C:/Windows/Fonts`
  on Windows when running a suite outside CTest).
- The running app locks `build/SerikaPhotoEdit.exe`: close it (check the title for `*` unsaved
  edits first) before rebuilding.
- See the app: `build/SerikaPhotoEdit.exe --demo --screenshot out.png` captures the window after
  1.5 s. Icons: `build/serika_iconsheet sheet.png [primary]`. PSD import: `build/serika_psdcheck
  file.psd composite.png` against `python -I scripts/psd_inspect.py file.psd reference.png`
  (Photoshop's own embedded composite).

## Map

| Path | What lives there |
| --- | --- |
| `src/document/` | `Document` (state, tiles, history via `mutate`/transactions), layer operations, transforms |
| `src/compositor/` | CPU compositor, blend modes, adjustments; optional GPU (`GpuProcessor`) |
| `src/io/` | Native SPE/SPEB, PSD/PSB (`psd/`), RAW, raster, CMYK export — read `src/io/README.md` |
| `src/actions/` | `ActionRunner`: JSON action steps used by actions, batch and MCP `run_actions` |
| `src/tools/` | Brush dynamics, local algorithms, subject engines and `AiSegmenter` (ONNX Runtime) |
| `src/ui/` | `MainWindow` split across `MainWindow*.cpp`, `Commands.cpp` (`runCommand`), `Dialogs.cpp`, canvas, panels, shortcuts, themes in `resources/themes/*.qss` |
| `src/mcp/` | `McpServer` (protocol + tools), `McpLiveBridge` (desktop window host for `--attach`) |
| `tools/`, `scripts/` | Developer executables; build, packaging, release and MCP launcher scripts |
| `docs/` | User and engineering docs; keep them in step with behaviour |

Key invariants: `state.layers` is bottom-to-top (index 0 is the bottom); layer IDs are stable;
every user-visible edit goes through `Document::mutate` or a transaction so it can be undone;
groups/artboards are the only containers; PSD records on disk are bottom-to-top.

## Conventions

- Match the surrounding code: C++20, Qt types, `serika` namespace, 4-space indent, 110 columns.
  Format touched files with the real binary, `.tools/format/clang_format/data/bin/clang-format.exe -i`
  (the `bin/clang-format.exe` shim is broken). HEAD is clang-format clean.
- Comments explain *why*, briefly; no narration of what the next line does.
- Never pop dialogs from code an agent or batch job can reach (`showMessage` falls back to a modal
  box when no document is open). Long work on the UI thread must keep the window painting.
- Icons are vector glyphs in `src/ui/canvas/CanvasView.cpp` (`paintToolGlyph`, 24-unit grid,
  1.5 strokes); render a sheet with `serika_iconsheet` to review. Theme rules go in all four `.qss`.
- Files use CRLF or LF as they already do; Git normalises (`* text=auto`). Avoid `sed -i` on CRLF
  files and shell heredocs containing `\n` escapes — write patch scripts to files instead.

## Git and releases

- Branch from `main` (`fix/…`, `feature/…`); never commit straight to `main` or a `release/X.Y`
  branch, never push, tag or force anything without the maintainer asking.
- Commits use the repository-local identity (`git config --local user.email`); verify before the
  first commit. Imperative subject ≤ 72 chars, body says why; end with your `Co-Authored-By:` line.
- Every user-visible change adds an entry under `## [Unreleased]` in `CHANGELOG.md` (Keep a
  Changelog sections). `python scripts/release.py check` must pass; releases are cut only with
  `python scripts/release.py cut X.Y.Z` by the maintainer.

## Using Serika from an agent (MCP)

`.mcp.json` registers the `serika` server for Claude Code via `scripts/serika-mcp.py`. It attaches
to the running window (starting the local build if needed), so your edits appear live and the user
can undo them; files are confined to `.serika-workspace/` (override with `SERIKA_MCP_ROOT`). Start
with `list_documents` → `inspect_document` (stable layer IDs) → edit → `preview_document` → save.
`list_actions` documents every `run_actions` command. AI background removal needs ONNX Runtime and
the BiRefNet model ([docs/AI-BACKGROUND-REMOVAL.md](docs/AI-BACKGROUND-REMOVAL.md)); dev builds can
use `SERIKA_ONNXRUNTIME` / `SERIKA_AI_MODEL`. Full reference: [docs/MCP-SERVER.md](docs/MCP-SERVER.md).

## Skills

Project skills in `.claude/skills/` cover: `build-and-test`, `release`, `serika-mcp`,
`add-mcp-tool`, `psd-fidelity` and `ui-icons`. Other agents can read the same `SKILL.md` files as
task playbooks.
