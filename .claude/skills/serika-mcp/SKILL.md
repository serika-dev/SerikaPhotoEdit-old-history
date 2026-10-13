---
name: serika-mcp
description: Drive the Serika PhotoEdit editor through its MCP server to make, edit or check artwork, layouts, covers and PSDs (live in the open window or isolated). Use whenever asked to create or change a design with Serika.
---

# Designing with Serika over MCP

The user watches every change live and judges the result at full size. Misaligned or overflowing
text is the most common failure; the workflow below exists to make that impossible. Do not skip
steps because a downscaled preview "looks fine" — at 1800 px wide a 20 px error is invisible.

## Connect

- In this repo the `serika` server (`.mcp.json`) is live: tools act on the tabs open in the Serika
  window. Files are confined to the workspace (`.serika-workspace/` or `SERIKA_MCP_ROOT`); point it
  at the user's project folder when they ask you to use files there. Never overwrite a user's source
  file; save results under new names.
- Elsewhere: `SerikaPhotoEdit --mcp --attach --mcp-root <folder>` (live) or without `--attach`.

## 1. Measure before you design

1. `list_documents`, then `inspect_document`. Every layer has stable `id`, stack order (bottom
   first) and ink `bounds` [x, y, w, h].
2. Write down the **regions** you will fill, in document pixels, from those bounds — e.g. for a
   case template: back panel, spine, front panel, and any notches or logos the template draws on top.
   `preview_document` with `region` on each corner confirms them.
3. Decide a grid: outer padding (≈4 % of the panel width), gutters, and a type scale (e.g. title,
   heading, body, caption at roughly 2.0 / 1.4 / 1.0 / 0.6 × body).

## 2. Place everything by rule, never by eye

- **Text always goes in a box.** `create_text_layer` with `box` = the region inset by padding,
  `align` (`left`/`center`/`right`), `valign` (`top`/`middle`/`bottom`) and `fit: true`. Serika
  wraps at the box width, aligns every line, positions the real ink and shrinks the font if needed.
  Never centre with spaces, never guess `x`/`y`, never put two lines in two layers to fake alignment.
- Labels inside pills, buttons, title bars: box = the shape's bounds, `align: center`,
  `valign: middle`.
- Use `measure_text` to pick a size before placing long copy; keep body text consistent across a
  design (same family, size and colour for the same role).
- Everything else: `align_layers` (by ink) to a `rect`, the canvas or another layer; `as_group`
  to move a set together. Distances between blocks come from the grid, not from previews.
- Images: `place_image` with only `width` or `height` keeps the aspect ratio. To fill a panel,
  overflow it and mask (rectangle selection → `add_layer_mask from: selection`). `remove_background`
  only on art whose background must go (`engine: "ai"` for busy backgrounds; ~20–80 s on a CPU).
- Templates: keep their frame on top — `move_layers … position: above, target_layer_id: <bottom>`.
- One concern per layer, named for its role (`create_text_layer` names text after its content).

## 3. Verify like a print designer

1. `check_layout` with `safe_areas` = your regions and `margin` = your padding. Fix **every**
   issue (overflow, overlap, crossing a panel edge), then run it again until `ok` is true.
2. `preview_document` with `region` at full resolution over **every** text block and every edge
   where art meets the template. Look for clipped glyphs, uneven padding, text touching edges,
   wrong stacking (text under a box), and low-resolution art.
3. Then the whole-document preview for balance.
4. Mistakes: `undo` (one tool call = one step) or `set_layer_properties` (box text re-lays itself
   out when you change its text, size, weight or tracking) — don't stack corrections on top.
5. Save `.spe` for the editable master and `.png`/`.psd` exports, under new names unless the user
   agreed to overwrite.

## Performance

- Every edit re-renders the document; keep layer counts sensible (one layer for a set of same-colour
  shapes: add rectangles to one selection, then one `fill`).
- `run_actions` is one undo step and the window repaints a few times a second while it runs.

## Reporting

Tell the user what you made, what you verified (check_layout result, regions inspected) and
anything you could not make perfect (e.g. a low-resolution source image), with the saved paths.
