#!/usr/bin/env python3
# Writes src/assets/PlayfieldPlywood.png, the default Plywood image of the Playfield Maker (the hole walls): 256 x 512, deterministic.
# v runs from the top (0) to the bottom (1) of the board over 7 equal plies with 6 glue lines; the last 8 rows are near black (the backing seen
# through a hole). Standard library only: python3 tools/make_playfield_plywood.py
import os, random, struct, zlib

W, H, BACK = 256, 512, 8
FACE, INNER, GLUE = (0.80, 0.66, 0.47), (0.72, 0.56, 0.38), (0.25, 0.18, 0.12)
rng = random.Random(1906)
plyH = (H - BACK) / 7.0
shift = [0.0] + [rng.uniform(-0.06, 0.06) for _ in range(5)] + [0.0]  # A fixed shade per inner ply
grain = [[rng.uniform(-0.03, 0.03) for _ in range(W)] for _ in range(7)]  # Along u, per ply

def smooth(row):  # A little blur along u: grain streaks rather than pixel noise
    return [(row[(x - 1) % W] + 2 * row[x] + row[(x + 1) % W]) / 4 for x in range(W)]

grain = [smooth(smooth(g)) for g in grain]
rows = []
for y in range(H):
    line = bytearray([0])  # filter: none
    if y >= H - BACK:
        line += bytes([round(0.05 * 255)] * 3 * W)
    else:
        ply = min(6, int(y / plyH))
        inPly = y - ply * plyH
        glue = ply > 0 and inPly < 0.02 * plyH + 0.5  # 2% of a ply at the top of each ply but the first: 6 glue lines
        for x in range(W):
            if glue:
                c = GLUE
            else:
                base = FACE if ply in (0, 6) else INNER
                d = shift[ply] + grain[ply][x]
                c = tuple(min(1.0, max(0.0, v * (1.0 + d))) for v in base)
            line += bytes(round(v * 255) for v in c)
    rows.append(bytes(line))

def chunk(t, data):
    return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'assets', 'PlayfieldPlywood.png')
with open(out, 'wb') as f:
    f.write(png)
print(out, len(png), 'bytes')
