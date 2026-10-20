#!/usr/bin/env python3
"""Rescale the alt panel photos to the 256 px reference height the plugin's layout assumes.

docs/panel_reference_alt.png (2166x269) and docs/panel_reference_compact_alt.png (1598x269)
are Alan's hand-made sources. D110Panel::kRefH is 256 and is used by the drawer layout, so
the embedded copies are the same pictures resampled to exactly 256 px high:
docs/panel_reference_alt_256.png and docs/panel_reference_compact_alt_256.png.
All alt geometry in PluginEditor.cpp is measured on those two files.
"""
import sys
from PIL import Image

for src, dst in (("docs/panel_reference_alt.png", "docs/panel_reference_alt_256.png"),
                 ("docs/panel_reference_compact_alt.png", "docs/panel_reference_compact_alt_256.png")):
    im = Image.open(src).convert("RGBA")
    w = round(im.width * 256 / im.height)
    out = im.resize((w, 256), Image.LANCZOS)
    out.save(dst)
    print(dst, out.size)
