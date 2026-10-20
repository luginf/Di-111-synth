#!/usr/bin/env python3
"""Cut docs/volume_knob_alt_disc.png out of docs/panel_reference_alt_256.png.

The alt photo's VOLUME knob carries a baked white pointer (parked at MIN). The panel draws its
own pointer, so the disc is lifted from the photo with that pointer inpainted from the body's
own radial profile (the body is flat, ~38 luma), and given a soft circular alpha so it can be
laid over the photo at an integer offset with no clip. Constants match PluginEditor.cpp's alt
skin: sprite origin (348,123), knob centre (381.63,156.91) in continuous coordinates.
"""
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

OX, OY, SZ = 348, 123, 68
CX, CY = 381.63, 156.91          # continuous coordinates (pixel i spans [i, i+1))
RADIUS = 30.5

im = np.array(Image.open("docs/panel_reference_alt_256.png").convert("RGBA")).astype(float)
crop = im[OY:OY + SZ, OX:OX + SZ].copy()
lum = crop[..., :3].mean(axis=2)
ys, xs = np.mgrid[0:SZ, 0:SZ]
dx, dy = xs + 0.5 - (CX - OX), ys + 0.5 - (CY - OY)
r = np.hypot(dx, dy)
ang = np.degrees(np.arctan2(dx, -dy))        # clockwise from straight up

sector = (ang > -165) & (ang < -120)
mask = ndi.binary_dilation(sector & (r < 30.5) & (lum > 42), iterations=3) & (r < 30.5)
for rr in range(0, 32):
    ring = (r >= rr) & (r < rr + 1)
    src = ring & ~mask & ~sector
    if not src.any():
        continue
    fill = np.median(crop[src], axis=0)
    crop[ring & mask] = fill

alpha = np.clip(RADIUS + 0.5 - r, 0.0, 1.0)
crop[..., 3] = alpha * 255
Image.fromarray(crop.astype(np.uint8), "RGBA").save("docs/volume_knob_alt_disc.png")
print("masked", int(mask.sum()), "px")
