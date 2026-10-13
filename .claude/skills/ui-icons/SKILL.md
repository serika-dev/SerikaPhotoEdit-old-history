---
name: ui-icons
description: Draw or change Serika's vector icons (tools, layer panel, adjustments, task bar) and their Photoshop-style styling. Use for icon, toolbar, panel button or theme work.
---

# Icons and panel styling

## Where icons come from

`toolIcon(name)` / `toolGroupIcon(name)` in `src/ui/canvas/CanvasView.cpp` return a `QIcon` backed
by `GlyphIconEngine`, which paints vectors at the exact size and device pixel ratio requested, so
icons stay sharp at 125 %/150 %. Glyph families, checked in this order:

| Prefix | Function | Grid |
| --- | --- | --- |
| `layer:` (eye, lock, fx, mask, folder, new, trash, …) | `paintLayerGlyph` | 22 units |
| `adj:<Adjustment name>` | `paintAdjustmentGlyph` | 22 units |
| tool names, `ui:`, `ctx:` | `paintToolGlyph` | 24 units, 1.5 stroke |

Style: one distinct silhouette per tool, 1.5-unit round strokes, filled accents for weight (brush
tips, cursor arrows, stamp bases), keep inside units 2–22, variants marked by a small badge
(plus/minus, sparkle, dashed outline). Use the colour passed in; never hard-code theme colours.

## Workflow

1. Draw the glyph; new names must not collide with existing `contains()` checks.
2. `build/serika_iconsheet sheet.png` and read the image: the 100 px render shows mistakes, the
   20 px one shows legibility. Compare neighbours for consistent weight.
3. Buttons: `setGlyph(button, "layer:…", size)` in `MainWindow.cpp` tags the button so
   `applyTheme` recolours it; plain icon-only buttons get `iconButton=true` styling.
4. Styling lives in all four `resources/themes/*.qss` (`dark`, `darkest`, `light`, `lightest`):
   `QWidget#toolbar QToolButton` (flat sidebar), `QToolButton[iconButton="true"]`,
   `QFrame#contextBar`, `QTreeWidget#layersTree`.
5. Screenshot the app (`--demo --screenshot`) and look at it before finishing.
