"""Independent PSD reader for checking Serika's importer against Photoshop's own data.

Lists layer records in file order (Photoshop stores them bottom-to-top: a group's end marker,
its children, then the folder record) and writes Photoshop's embedded merged composite as a PNG
reference. Pure standard library on purpose: it shares no code with Serika.

Usage: python -I scripts/psd_inspect.py <file.psd> <reference.png> [downscale]
"""
import struct
import sys
import zlib


def packbits(data, size):
    out = bytearray()
    i = 0
    while len(out) < size and i < len(data):
        n = data[i]
        i += 1
        if n < 128:
            out += data[i:i + n + 1]
            i += n + 1
        elif n > 128:
            out += bytes([data[i]]) * (257 - n)
            i += 1
    return bytes(out[:size])


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + row for row in rows)
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    path, out = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    b = open(path, "rb").read()
    assert b[:4] == b"8BPS"
    version = struct.unpack(">H", b[4:6])[0]
    channels, height, width, depth, mode = struct.unpack(">HIIHH", b[12:26])
    print("version", version, "channels", channels, "size", width, height, "depth", depth, "mode", mode)
    p = 26
    p += 4 + struct.unpack(">I", b[p:p + 4])[0]  # color mode data
    p += 4 + struct.unpack(">I", b[p:p + 4])[0]  # image resources
    lm_len = struct.unpack(">I", b[p:p + 4])[0]
    lm = p + 4
    after_lm = lm + lm_len
    li_len = struct.unpack(">I", b[lm:lm + 4])[0]
    q = lm + 4
    count = struct.unpack(">h", b[q:q + 2])[0]
    q += 2
    print("layer records:", abs(count), "(first record listed first)")
    for index in range(abs(count)):
        top, left, bottom, right, nch = struct.unpack(">iiiiH", b[q:q + 18])
        q += 18 + nch * 6
        q += 4  # 8BIM
        blend = b[q:q + 4].decode("latin1")
        q += 4
        opacity, clipping, flags, _ = struct.unpack(">BBBB", b[q:q + 4])
        q += 4
        extra_len = struct.unpack(">I", b[q:q + 4])[0]
        q += 4
        e = q
        end = q + extra_len
        e += 4 + struct.unpack(">I", b[e:e + 4])[0]  # mask
        e += 4 + struct.unpack(">I", b[e:e + 4])[0]  # blending ranges
        nlen = b[e]
        name = b[e + 1:e + 1 + nlen].decode("latin1", "replace")
        e += ((1 + nlen + 3) // 4) * 4
        section = None
        keys = []
        while e + 12 <= end:
            key = b[e + 4:e + 8].decode("latin1")
            n = struct.unpack(">I", b[e + 8:e + 12])[0]
            data = b[e + 12:e + 12 + n]
            keys.append(key)
            if key in ("lsct", "lsdk"):
                section = struct.unpack(">I", data[:4])[0]
            if key == "luni":
                chars = struct.unpack(">I", data[:4])[0]
                name = data[4:4 + chars * 2].decode("utf-16-be", "replace")
            e += 12 + n + (n % 2)
        q = end
        print(f"{index:3d} section={section} clip={clipping} op={opacity} blend={blend} vis={not flags & 2} "
              f"rect=({left},{top})-({right},{bottom}) {name!r} {' '.join(keys)}")
    # Merged composite
    p = after_lm
    compression = struct.unpack(">H", b[p:p + 2])[0]
    p += 2
    planes = []
    if compression == 1:
        counts = struct.unpack(">" + "H" * (height * channels), b[p:p + 2 * height * channels])
        p += 2 * height * channels
        for c in range(min(channels, 3)):
            rows = []
            for y in range(height):
                n = counts[c * height + y]
                rows.append(packbits(b[p:p + n], width))
                p += n
            planes.append(rows)
    elif compression == 0:
        for c in range(min(channels, 3)):
            planes.append([b[p + y * width:p + (y + 1) * width] for y in range(height)])
            p += width * height
    else:
        print("merged compression", compression, "unsupported")
        return
    w2, h2 = width // scale, height // scale
    out_rows = []
    for y in range(h2):
        sy = y * scale
        r, g, bl = planes[0][sy], planes[1][sy], planes[2][sy]
        row = bytearray()
        for x in range(w2):
            sx = x * scale
            row += bytes((r[sx], g[sx], bl[sx]))
        out_rows.append(bytes(row))
    write_png(out, w2, h2, out_rows)
    print("wrote", out, w2, h2)


main()
