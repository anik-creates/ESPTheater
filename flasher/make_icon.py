"""Draws the flasher icon (blue tile with Wi-Fi arcs) and writes a multi-size .ico. No dependencies."""

import math
import struct
import sys

BLUE_TOP = (59, 130, 246)
BLUE_BOTTOM = (29, 78, 216)
WHITE = (255, 255, 255)


def coverage(size, x, y, inside, samples=4):
    """Fraction of pixel (x, y) covered by the shape, by supersampling."""
    hit = 0
    for i in range(samples):
        for j in range(samples):
            px = (x + (i + 0.5) / samples) / size
            py = (y + (j + 0.5) / samples) / size
            hit += inside(px, py)
    return hit / (samples * samples)


def in_tile(px, py, r=0.2):
    dx = max(r - px, 0, px - (1 - r))
    dy = max(r - py, 0, py - (1 - r))
    return dx * dx + dy * dy <= r * r


def in_wifi(px, py):
    cx, cy = 0.5, 0.74                      # centre of the arcs
    dx, dy = px - cx, py - cy
    dist = math.hypot(dx, dy)
    if dist <= 0.075:                       # dot
        return True
    if dy > -0.02:                          # arcs only above the centre
        return False
    angle = math.degrees(math.atan2(-dy, dx))
    if not 45 <= angle <= 135:
        return False
    for radius in (0.2, 0.33, 0.46):
        if abs(dist - radius) <= 0.045:
            return True
    return False


def render(size):
    rows = []
    for y in range(size):
        t = y / max(size - 1, 1)
        bg = tuple(round(a + (b - a) * t) for a, b in zip(BLUE_TOP, BLUE_BOTTOM))
        row = []
        for x in range(size):
            a_tile = coverage(size, x, y, in_tile)
            a_wifi = coverage(size, x, y, in_wifi) if a_tile else 0
            rgb = tuple(round(c * (1 - a_wifi) + w * a_wifi) for c, w in zip(bg, WHITE))
            row.append((rgb, round(255 * a_tile)))
        rows.append(row)
    return rows


def bmp_entry(size):
    pixels = render(size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for row in reversed(pixels):            # bottom-up
        for (r, g, b), a in row:
            xor += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    and_mask = bytes(mask_stride * size)    # alpha channel is used instead
    return header + bytes(xor) + and_mask


def write_ico(path, sizes=(16, 24, 32, 48, 64)):
    images = [bmp_entry(s) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(out + b"".join(images))


if __name__ == "__main__":
    write_ico(sys.argv[1] if len(sys.argv) > 1 else "app.ico")
