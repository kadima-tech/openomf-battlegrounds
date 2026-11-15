"""Cut-out -> 256x256 pilot photo: extend the beret's cropped crown, shift down, scale, black matte."""
import sys
import numpy as np
from PIL import Image

src, out_full, out_256 = sys.argv[1:4]
im = np.array(Image.open(src).convert('RGBA')).astype(np.float32)
H, W = im.shape[:2]
a = im[:, :, 3]

ys, lefts, rights = [], [], []
for y in range(0, 60):
    xs = np.nonzero(a[y] > 128)[0]
    ys.append(y), lefts.append(xs.min()), rights.append(xs.max())
ys, lefts, rights = map(np.array, (ys, lefts, rights))
cx = float(np.mean((lefts + rights) / 2))
hw = (rights - lefts) / 2.0
A, B = np.polyfit(ys, hw ** 2, 1)  # hw^2 = A*y + B; apex where hw = 0
apex = -B / A
D = int(np.ceil(-apex)) + 6
print(f'crown centre x={cx:.0f}, apex y={apex:.1f}, shift {D}px')

ext = np.zeros((H + D, W, 4), np.float32)
ext[D:] = im
for k in range(1, D + 1):
    row = D - k
    srcrow = min(k - 1, H - 1)
    shift = int(round(-1.5 * (2 * k - 1)))  # continue the stripe's slant instead of mirroring it
    ext[row, :, :3] = np.roll(im[srcrow, :, :3], shift, axis=0)
    y = -k
    w2 = A * y + B
    if w2 <= 0:
        continue
    w = np.sqrt(w2)
    x = np.arange(W)
    d = w - np.abs(x - cx)
    ext[row, :, 3] = np.clip(d / 2.0 + 0.5, 0, 1) * 255 * (im[0, :, 3] > 0)
ext = ext[:H]

out = ext.copy()
out[out[:, :, 3] == 0, :3] = 0
Image.fromarray(out.astype(np.uint8)).save(out_full)
small = Image.fromarray(out.astype(np.uint8)).resize((256, 256), Image.LANCZOS)
s = np.array(small)
s[s[:, :, 3] == 0, :3] = 0
Image.fromarray(s).save(out_256)
