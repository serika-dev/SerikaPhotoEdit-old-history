# Subject selection and background removal

**Select › Subject** and **Layer › Remove Background** have two engines. Both run entirely on this
computer; images are never uploaded.

| Engine | How it works | Cost | Best for |
| --- | --- | --- | --- |
| **Fast** (default) | A colour model learned from the image border, flooded inward, with fractional edge coverage | A fraction of a second on any machine, no extra files | Products or people on plain or soft backgrounds |
| **AI** | [BiRefNet lite](https://huggingface.co/onnx-community/BiRefNet_lite-ONNX) (MIT) at 1024 × 1024 through [ONNX Runtime](https://onnxruntime.ai/) | 5–30 s on a laptop CPU, about 1–3 s on a DirectX 12 GPU; ~225 MB model | Busy backgrounds, hair, fur and complex outlines |

Choose the default in **Preferences › AI**, or pick per use: *Select › Subject (AI)* and *Layer ›
Remove Background (AI)*. If the AI engine is not ready, Serika says what is missing and offers the
Fast engine instead.

*Remove Background* adds a layer mask built from **that layer's own pixels** (like Photoshop), so a
placed photo loses its own background even when it sits on a busy document. The pixels are kept;
delete or disable the mask to undo the effect. *Select Subject* reads the visible image.

## Setting up the AI engine

1. **ONNX Runtime 1.20 or newer.** Windows builds ship `onnxruntime.dll` next to
   `SerikaPhotoEdit.exe`. Elsewhere, install it (for example `brew install onnxruntime`, or the
   [official release archive](https://github.com/microsoft/onnxruntime/releases)) and set its path in
   Preferences › AI. Serika loads it with `QLibrary` only when the AI engine is used; it is never
   linked, so the rest of the editor works without it.
2. **The model.** Preferences › AI › *Download BiRefNet model* fetches it from Hugging Face after
   you confirm, verifies its published SHA-256 and stores it under the application data folder.
   You can instead point *Model file* at any compatible `.onnx` file.
3. **GPU (Windows).** Replace `onnxruntime.dll` with the DirectML build of ONNX Runtime and Serika
   uses the GPU automatically. Set `SERIKA_AI_DEVICE=cpu` to force the CPU.

Overrides for scripted and test use: `SERIKA_ONNXRUNTIME` (library path), `SERIKA_AI_MODEL`
(model path), `SERIKA_AI_THREADS` (CPU threads, 0 = automatic) and `SERIKA_AI_DEVICE`.

## For packagers

Configure with `-DSERIKA_ONNXRUNTIME_LIBRARY=/path/to/onnxruntime.dll` (or `.so`/`.dylib`) to copy
the runtime next to the executable at build time and install it beside the application. Only ONNX
Runtime's MIT C API header is vendored (`third_party/onnxruntime`). Model files are never bundled.

## MCP

Agents call `select_subject` and `remove_background` with `engine: "fast" | "ai"`, and check
readiness with `ai_status`. See [MCP-SERVER.md](MCP-SERVER.md).

## Super resolution

Image › Super Resolution and Layer › Upscale Layer enlarge art with one of two methods:

| Method | How | Speed |
| --- | --- | --- |
| AI | Real-ESRGAN (BSD-3-Clause) through the same ONNX Runtime, on overlapping 128 px tiles; alpha is resampled separately and transparent colours are bled outward so edges stay clean | General x4v3 ≈ 0.4 s per tile, x4plus ≈ 6 s per tile on a laptop CPU |
| Preserve Details | Lanczos-3 in premultiplied 16-bit light with an adaptive unsharp mask | instant |

The models are Qualcomm AI Hub ONNX exports (`real_esrgan_general_x4v3`, `real_esrgan_x4plus`,
release v0.64.0). Preferences › AI downloads the release zip, checks its published SHA-256 and
extracts the `.onnx` graph and its external `.data` weights into
`%APPDATA%/Serika/SerikaPhotoEdit/models/<id>/`. `SERIKA_UPSCALE_MODEL` points every model id at
one `.onnx` file for testing. The models enlarge 4×; other scales are resampled from that result
with Lanczos, and enlargements under 5 % use Preserve Details.

