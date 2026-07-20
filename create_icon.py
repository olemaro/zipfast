"""
Generates icon.ico — 32x32 blue rounded rect with white Z.
No dependencies — pure Python struct.
"""
import struct, math

W, H = 32, 32
BLUE  = (37, 99, 235, 255)
WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

grid = [list(BLUE) for _ in range(W * H)]

def px(x, y, color):
    if 0 <= x < W and 0 <= y < H:
        grid[y * W + x] = list(color)

# Rounded corners (radius=5)
R = 5
for y in range(H):
    for x in range(W):
        in_tl = x < R     and y < R
        in_tr = x >= W-R  and y < R
        in_bl = x < R     and y >= H-R
        in_br = x >= W-R  and y >= H-R
        if in_tl and math.hypot(x-R+1,     y-R+1)     > R: px(x, y, CLEAR)
        if in_tr and math.hypot(x-(W-R),   y-R+1)     > R: px(x, y, CLEAR)
        if in_bl and math.hypot(x-R+1,     y-(H-R))   > R: px(x, y, CLEAR)
        if in_br and math.hypot(x-(W-R),   y-(H-R))   > R: px(x, y, CLEAR)

# Top bar of Z:  y=9..12, x=8..23
for y in range(9, 13):
    for x in range(8, 24):
        px(x, y, WHITE)

# Bottom bar:    y=19..22, x=8..23
for y in range(19, 23):
    for x in range(8, 24):
        px(x, y, WHITE)

# Diagonal from (23,12) to (8,19) — thick
x1, y1, x2, y2 = 23, 12, 8, 19
steps = 30
for i in range(steps + 1):
    t = i / steps
    fx = x1 + t * (x2 - x1)
    fy = y1 + t * (y2 - y1)
    for dy in range(-1, 2):
        for dx in range(-1, 2):
            px(round(fx) + dx, round(fy) + dy, WHITE)

# Build BMP DIB (stored bottom-to-top)
xor_bytes = b''
for row in range(H - 1, -1, -1):
    for col in range(W):
        r, g, b, a = grid[row * W + col]
        xor_bytes += struct.pack('4B', b, g, r, a)   # BGRA

and_bytes = b'\x00' * (4 * H)   # 4 bytes/row (32 px / 8), all opaque

bih = struct.pack('<IiiHHIIiiII',
    40,                              # biSize
    W, H * 2,                        # width, height×2
    1, 32,                           # planes, bpp
    0,                               # compression BI_RGB
    len(xor_bytes) + len(and_bytes), # sizeImage
    0, 0, 0, 0)                      # xppm, yppm, clrUsed, clrImportant

image = bih + xor_bytes + and_bytes

icon_dir   = struct.pack('<HHH', 0, 1, 1)
icon_entry = struct.pack('<BBBBHHII', W, H, 0, 0, 1, 32, len(image), 22)

data = icon_dir + icon_entry + image
with open('icon.ico', 'wb') as f:
    f.write(data)
print(f'icon.ico created  ({len(data)} bytes)')
