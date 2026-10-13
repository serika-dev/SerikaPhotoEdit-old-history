---
name: serika-mcp
description: Drive the Serika PhotoEdit editor through its MCP server to inspect or edit images, PSDs and layered documents (live in the open window or isolated). Use when asked to make, change or check artwork with Serika.
---

# Editing with Serika over MCP

## Connect

- In this repo, the `serika` server from `.mcp.json` is already live: tools act on the tabs open in
  the Serika window (it starts the local build if needed). File paths are confined to the workspace
  (`.serika-workspace/` or `SERIKA_MCP_ROOT`); point it at the user's project folder when they ask
  you to use files there. Never overwrite a user's source file: save results under new names.
- Elsewhere: `SerikaPhotoEdit --mcp --attach --mcp-root <folder>` (live) or without `--attach`
  (isolated, invisible). Protocol 2026-07-28 (`_meta` per request) and 2025 handshakes both work.

## Workflow

1. `list_documents` (the active tab is marked) or `open_document` / `create_document`.
2. `inspect_document` for stable layer IDs, stack order (bottom first), kinds and bounds.
3. Edit with IDs: `place_image` (one dimension keeps the aspect), `create_text_layer`,
   `set_layer_properties` (also text, colour, x/y), `move_layers` (`above`/`below`/`inside` a
   target, or `top`/`bottom`), `group_layers`, `duplicate_layers`, `delete_layers`,
   `add_layer_mask`, `remove_background`, `run_actions` (call `list_actions` for the catalog).
4. `preview_document` (or `preview_layer`) and look at the PNG before declaring success.
5. `save_document` to `.spe` for an editable master and `.png`/`.psd` for exports
   (`overwrite: true` only when the user agreed).

## Tips

- Template documents (PSD mock-ups): put new art just above the bottom layer
  (`move_layers … position: above, target_layer_id: <bottom id>`) so the template's frame and
  logos stay on top.
- `remove_background` masks the target layer from its own pixels; `engine: "ai"` (BiRefNet) is far
  better on busy backgrounds but takes ~20 s on a laptop CPU; check `ai_status` first. Do not run it
  on art that already has the background the user wants.
- `run_actions` is one undo step and blocks the window while it runs; keep step lists modest.
  Whole-canvas selections on large documents are the expensive part.
- Mistake? `undo` (each tool call is one step) instead of layering fixes on top.
- Closing a modified document requires `discard: true`; only do that for your own scratch edits.
