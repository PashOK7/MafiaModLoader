"""make_picture.py: draws this example's picture, files/Maps/freeride0.bmp (Python standard library only).

The freeride menu shows the chosen place's picture, Maps\\freeride0.bmp for "City - Small" (a 256x128 24-bit BMP in
the game's archives). This mod ships a picture of its own under that name: a night sky, a skyline and a sign. Nothing
of the game's is in it; run this again after changing it:

    python mods/examples/freeride_picture/make_picture.py [text]
"""
import os
import struct
import sys

W, H = 256, 128
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'files', 'Maps', 'freeride0.bmp')

# a 5x7 font: each letter seven rows of five bits
FONT = {
    'A': '01110 10001 10001 11111 10001 10001 10001', 'B': '11110 10001 10001 11110 10001 10001 11110',
    'C': '01111 10000 10000 10000 10000 10000 01111', 'D': '11110 10001 10001 10001 10001 10001 11110',
    'E': '11111 10000 10000 11110 10000 10000 11111', 'F': '11111 10000 10000 11110 10000 10000 10000',
    'G': '01111 10000 10000 10111 10001 10001 01111', 'H': '10001 10001 10001 11111 10001 10001 10001',
    'I': '11111 00100 00100 00100 00100 00100 11111', 'K': '10001 10010 10100 11000 10100 10010 10001',
    'L': '10000 10000 10000 10000 10000 10000 11111', 'M': '10001 11011 10101 10101 10001 10001 10001',
    'N': '10001 11001 10101 10011 10001 10001 10001', 'O': '01110 10001 10001 10001 10001 10001 01110',
    'P': '11110 10001 10001 11110 10000 10000 10000', 'R': '11110 10001 10001 11110 10100 10010 10001',
    'S': '01111 10000 10000 01110 00001 00001 11110', 'T': '11111 00100 00100 00100 00100 00100 00100',
    'U': '10001 10001 10001 10001 10001 10001 01110', 'V': '10001 10001 10001 10001 10001 01010 00100',
    'W': '10001 10001 10001 10101 10101 11011 10001', 'Y': '10001 10001 01010 00100 00100 00100 00100',
    ' ': '00000 00000 00000 00000 00000 00000 00000',
}


def draw(text):
    px = [[(0, 0, 0)] * W for _ in range(H)]   # rows from the top, (r, g, b)
    for y in range(H):   # the sky: deep blue to dusk at the horizon
        t = y / (H - 1)
        sky = (int(20 + 120 * t * t), int(24 + 50 * t), int(70 + 40 * t))
        for x in range(W):
            px[y][x] = sky
    seed = 7
    for _ in range(60):   # stars
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        x, y = seed % W, (seed >> 8) % 60
        px[y][x] = (230, 230, 210)
    x = 0
    while x < W:   # a skyline, with lit windows
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        w, top = 14 + seed % 22, 58 + (seed >> 10) % 40
        for yy in range(top, H):
            for xx in range(x, min(W, x + w)):
                lit = (xx - x) % 5 in (2, 3) and (yy - top) % 7 in (3, 4) and ((xx * 7 + yy * 13 + seed) % 5) < 2
                px[yy][xx] = (250, 210, 120) if lit else (18, 16, 24)
        x += w + 1
    scale, gap = 2, 2   # the sign: the text in a box
    text = text.upper()
    tw = len(text) * (5 * scale + gap) - gap
    x0, y0 = (W - tw) // 2, 18
    for yy in range(y0 - 6, y0 + 7 * scale + 6):
        for xx in range(x0 - 8, x0 + tw + 8):
            edge = yy in (y0 - 6, y0 + 7 * scale + 5) or xx in (x0 - 8, x0 + tw + 7)
            px[yy][xx] = (200, 40, 40) if edge else (12, 10, 14)
    for i, ch in enumerate(text):
        rows = FONT.get(ch, FONT[' ']).split()
        for r, bits in enumerate(rows):
            for c, bit in enumerate(bits):
                if bit == '1':
                    for dy in range(scale):
                        for dx in range(scale):
                            px[y0 + r * scale + dy][x0 + i * (5 * scale + gap) + c * scale + dx] = (255, 236, 160)
    return px


def write_bmp(path, px):
    row = W * 3   # (a multiple of 4: no padding)
    data = b''.join(bytes(v for (r, g, b) in reversed_row for v in (b, g, r)) for reversed_row in reversed(px))
    header = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, len(data), 2834, 2834, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(header + info + data)
    assert row * H == len(data)


if __name__ == '__main__':
    write_bmp(OUT, draw(sys.argv[1] if len(sys.argv) > 1 else 'MAFIA MOD LOADER'))
    print(OUT)
