#!/usr/bin/env python3
"""Generate the window icon, assets/icon.png.

**Generated, not drawn.** The same rule the vehicles and the trig tables follow:
nothing here is modelled by hand or downloaded, so there is no third-party art
in the game and no licence condition to satisfy - see assets/ATTRIBUTION.md.

What it draws is a gear lever, which is what the game is named after: a knob on
a shaft, coming out of a gaiter, over the shift pattern's gate. At sixteen
pixels almost none of that survives - what survives is a round red knob above a
pale diagonal, and that is what the shape is chosen to keep.

Written with the standard library alone. A PNG is a zlib stream in a handful of
chunks, and depending on an imaging library to emit one would mean the icon
could only be regenerated on a machine that had it.

    python3 tools/make_icon.py
"""
import pathlib
import struct
import zlib

SIZE = 256          # generated large; SDL scales it down for the title bar

# The game's own colours, so the icon is the same red the cars are painted in.
KNOB      = (198, 32, 42, 255)
KNOB_LIT  = (232, 82, 88, 255)
KNOB_DARK = (128, 16, 26, 255)
SHAFT     = (188, 190, 198, 255)
SHAFT_DARK= (120, 124, 134, 255)
GAITER    = (34, 34, 40, 255)
PLATE     = (58, 60, 68, 255)
GATE      = (150, 152, 160, 255)
CLEAR     = (0, 0, 0, 0)


def blend(dst, src):
    """src over dst, both straight RGBA."""
    a = src[3] / 255.0
    if a >= 1.0:
        return src
    if a <= 0.0:
        return dst
    return tuple(int(round(src[i] * a + dst[i] * (1.0 - a))) for i in range(3)) + (255,)


class Canvas:
    def __init__(self, n):
        self.n = n
        self.px = [[CLEAR] * n for _ in range(n)]

    def put(self, x, y, colour):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.px[y][x] = blend(self.px[y][x], colour)

    def disc(self, cx, cy, r, colour, shade=None):
        """A filled circle, antialiased by sampling the coverage of each pixel."""
        r2 = r * r
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                # Four samples a side is plenty and keeps the edge from crawling.
                hits = 0
                for sy in range(4):
                    for sx in range(4):
                        px = x + (sx + 0.5) / 4.0
                        py = y + (sy + 0.5) / 4.0
                        if (px - cx) ** 2 + (py - cy) ** 2 <= r2:
                            hits += 1
                if hits == 0:
                    continue
                c = colour
                if shade is not None:
                    # Lit from up and to the left, like everything else here.
                    d = ((x - cx) + (y - cy)) / (2.0 * r)
                    t = max(0.0, min(1.0, 0.5 - d * 0.75))
                    c = tuple(int(round(colour[i] + (shade[i] - colour[i]) * t))
                              for i in range(3)) + (colour[3],)
                self.put(x, y, c[:3] + (int(c[3] * hits / 16),))

    def bar(self, x0, y0, x1, y1, half, colour):
        """A capsule from one point to another - the shaft, and the gate lines."""
        dx, dy = x1 - x0, y1 - y0
        length = (dx * dx + dy * dy) ** 0.5
        if length <= 0.0:
            return
        for y in range(int(min(y0, y1) - half) - 1, int(max(y0, y1) + half) + 2):
            for x in range(int(min(x0, x1) - half) - 1, int(max(x0, x1) + half) + 2):
                hits = 0
                for sy in range(4):
                    for sx in range(4):
                        px = x + (sx + 0.5) / 4.0
                        py = y + (sy + 0.5) / 4.0
                        t = ((px - x0) * dx + (py - y0) * dy) / (length * length)
                        t = max(0.0, min(1.0, t))
                        nx, ny = x0 + dx * t, y0 + dy * t
                        if (px - nx) ** 2 + (py - ny) ** 2 <= half * half:
                            hits += 1
                if hits:
                    self.put(x, y, colour[:3] + (int(colour[3] * hits / 16),))


def build():
    n = SIZE
    c = Canvas(n)
    u = n / 256.0        # everything below is written at 256 and scaled

    # The gate: the shift pattern, faint, behind everything. An H, because that
    # is the pattern anybody pictures when they picture a gearstick.
    for gx in (78, 128, 178):
        c.bar(gx * u, 150 * u, gx * u, 228 * u, 5 * u, PLATE)
    c.bar(78 * u, 189 * u, 178 * u, 189 * u, 5 * u, PLATE)
    for gx in (78, 128, 178):
        c.bar(gx * u, 152 * u, gx * u, 226 * u, 2.0 * u, GATE)
    c.bar(80 * u, 189 * u, 176 * u, 189 * u, 2.0 * u, GATE)

    # The gaiter it comes out of, and the shaft.
    c.disc(128 * u, 196 * u, 30 * u, GAITER)
    c.bar(128 * u, 196 * u, 116 * u, 96 * u, 11 * u, SHAFT_DARK)
    c.bar(126 * u, 194 * u, 114 * u, 96 * u, 7.5 * u, SHAFT)

    # And the knob, which is the whole silhouette at small sizes.
    c.disc(112 * u, 78 * u, 44 * u, KNOB_DARK)
    c.disc(112 * u, 76 * u, 42 * u, KNOB, shade=KNOB_LIT)

    return c


def chunk(kind, data):
    out = struct.pack(">I", len(data)) + kind + data
    return out + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)


def write_png(path, canvas):
    raw = bytearray()
    for row in canvas.px:
        raw.append(0)                       # filter: none, so the file is plain
        for px in row:
            raw += bytes(px)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", canvas.n, canvas.n, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)
    return len(png)


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "assets" / "icon.png"
    n = write_png(out, build())
    print(f"wrote {out.relative_to(root)}: {SIZE}x{SIZE}, {n} bytes")


if __name__ == "__main__":
    main()
