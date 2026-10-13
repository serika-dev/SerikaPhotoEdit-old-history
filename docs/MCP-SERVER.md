# MCP server

Serika PhotoEdit includes a native stdio [Model Context Protocol](https://modelcontextprotocol.io)
server. It uses the same document, compositor, actions and file-format code as the editor. No
network listener, model account or extra runtime is needed.

## Live sessions: edit what is on screen

```powershell
SerikaPhotoEdit.exe --mcp --attach --mcp-root "C:/Pictures/SerikaWorkspace"
```

`--attach` connects the MCP client to the running Serika window (starting it if needed; add
`--no-launch` to fail instead). The agent's documents are the window's open tabs: every edit appears
immediately, joins the normal undo history, and the title bar shows **AI agent connected** while a
session is attached. Agents can create or open documents (each gets a tab) and bring one to the
front with `focus_document`. Closing a tab with unsaved edits requires `discard: true`, so an agent
never triggers a Save prompt. The bridge listens on a per-user local socket (a named pipe on
Windows) restricted to the current user; turn it off in Preferences › AI.

In this repository, [`.mcp.json`](../.mcp.json) registers the live server for Claude Code through
[`scripts/serika-mcp.py`](../scripts/serika-mcp.py), which finds the local build and uses
`.serika-workspace/` as the file workspace. Set `SERIKA_MCP_MODE=isolated` for an isolated session.

## Isolated sessions

```powershell
SerikaPhotoEdit.exe --mcp --mcp-root "C:/Pictures/SerikaWorkspace"
```

Without `--attach`, each client connection owns private in-memory documents that never appear in a
window. On macOS, use `SerikaPhotoEdit.app/Contents/MacOS/SerikaPhotoEdit`; Linux packages provide
the `SerikaPhotoEdit` launcher. Configure your MCP client to launch the command directly with the
argument array shown in [the example configuration](../examples/serika-photoedit.mcp.json). The
client manages stdin/stdout; do not wrap it in a shell command string.

## Layout guidance

Every session receives layout rules in the server instructions: put text in boxes with
`align`/`valign`/`fit`, align by ink, inset from panel edges, then run `check_layout` with the
panels as `safe_areas` and inspect each text block with a full-resolution `region` preview before
finishing. The project skill `.claude/skills/serika-mcp/SKILL.md` expands this into a complete
design workflow.

## Protocol

Serika speaks **2026-07-28** and the earlier handshake revisions **2025-11-25, 2025-06-18 and
2025-03-26** (a dual-era server). Modern clients send no handshake: every request carries
`io.modelcontextprotocol/protocolVersion` (and optionally client info and capabilities) in
`params._meta`; `server/discover` reports supported versions, capabilities and instructions; an
unsupported version returns error `-32022` listing the supported ones. Every result carries
`resultType: "complete"` and `io.modelcontextprotocol/serverInfo` in `_meta`; `tools/list` and
`server/discover` include `ttlMs`/`cacheScope`. Older clients use `initialize`,
`notifications/initialized` and `ping` as before. Messages are newline-delimited UTF-8 JSON-RPC;
stdout carries only protocol messages and diagnostics go to stderr.

## Tools

| Tool | Behavior |
| --- | --- |
| `server_status`, `list_documents` | Workspace, limits, protocol versions, session kind; documents with the active one marked |
| `create_document`, `open_document`, `close_document`, `focus_document` | RGB documents at 8, 16 or 32 bits; close with unsaved edits needs `discard: true`; focus is live-only |
| `inspect_document`, `history` | Stable document/layer IDs, hierarchy, appearance, selection bounds and undo state |
| `preview_document`, `preview_layer` | PNG image content of the composite or one layer, 32–2048 pixels on the longest edge; `region` crops at full resolution |
| `save_document` | Editable SPE/SPEB, compatible PSD/PSB, or flattened raster export |
| `select_layer`, `set_layer_properties` | Active layer; name, opacity, fill, visibility, blend mode, lock, position, text and colour |
| `add_layer`, `create_text_layer`, `place_image` | Pixel/group/fill/text layers; text in a `box` wraps, aligns its ink exactly and can `fit`; an image file from the workspace as a positioned, scaled layer |
| `measure_text`, `align_layers`, `check_layout` | Ink size of text before placing it; align layers by ink to the canvas, selection, a rect or a layer; report text overflowing safe areas, overlapping other text or crossing panel edges |
| `duplicate_layers`, `delete_layers`, `group_layers`, `ungroup_layers`, `move_layers` | Multi-layer operations by ID; `move_layers` places above/below/inside a target or at the top/bottom |
| `select_subject`, `remove_background`, `ai_status` | Fast or local AI engine; background removal masks the target layer from its own pixels ([details](AI-BACKGROUND-REMOVAL.md)) |
| `add_layer_mask`, `delete_layer_mask` | Reveal-all, hide-all or selection masks; delete or apply |
| `run_actions`, `list_actions` | 1–100 native action steps in one undoable transaction; `list_actions` documents every command with a tested example |
| `undo`, `redo` | Edit history |

Use the returned IDs instead of guessing layer indices. Save SPE as the editable
master; exporting a raster or PSD does not mark the native document saved. Call
`preview_document` to inspect work before saving. No writes occur merely because
a document was created or edited. Existing files require `overwrite: true`.

Call `list_actions` for the full command catalog. Action steps follow
[the action format](../examples/web-export.speaction):

```json
{"document_id":"<returned-id>","steps":[
  {"command":"New Layer","parameters":{"name":"Retouch"}},
  {"command":"adjustment","name":"Exposure","parameters":{"exposure":0.3}}
]}
```

`create_text_layer` uses pixels for its base `font_size`, `x`, `y` and `tracking`. Rich text uses `parameters.typography` documented in [Typography](TYPOGRAPHY.md); span `size` values are points.
Fill parameters use the same settings as the native gradient/pattern editors.
Unsupported action commands return an error; shell commands and arbitrary scripts
are not implemented.

## Scope and limits

File paths must resolve inside the configured workspace, including through
symbolic links. Output parent folders must already exist. The session allows 8
documents, 16 million pixels per document, 30,000 pixels per dimension, 256 layers,
512 MB of stored image/mask/embedded content per document, 20 undo states and 4 MiB
per request. Native decoders and transforms also enforce their own bounds. These
are application limits, not an operating-system memory sandbox; full-image
operations and undo copies can require more working memory.

MCP rejects SVG/SVGZ inputs, including files detected as SVG under a different
extension, because Qt's SVG decoder can read external image references. The GUI
still supports SVG. Linked Smart Objects opened through MCP use their stored
cached pixels; the server exposes no linked-source refresh operation. Resize
defaults are checked against each preceding action's resulting dimensions, and
transform raster bounds are checked before allocation. A rejected action set
restores the document and its undo history.

The desktop remains an RGB editor. MCP uses its normal rendering and format
behavior, including the documented PSD fallbacks. GPU acceleration is optional
via `--gpu` and depends on the platform plugin/device; headless platforms commonly
fall back to CPU. See [GPU processing](GPU-PROCESSING.md).

Protocol references: [stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports),
[lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle),
[tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
