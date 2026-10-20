# D-110 front panel reference — measured geometry

Everything below is in the reference image's own pixels and was obtained by
**profiling the images**, not by eye: cap edges are the runs of above-threshold
luminance between the black recesses, the LCD grid came out of the gaps between
character cells, and the knob's centre/radii came out of a radial profile.

## Assets in this folder

| File | What it is |
| --- | --- |
| `panel_reference_original.png` | the author's render, untouched — 2124 × 256 |
| `panel_reference.png` | the working asset the plugin embeds; two retouches, below |
| `lcd_reference.png` | photograph of a **real, powered-on D-110 LCD**, 373 × 118 |

### Retouches applied to `panel_reference.png`

1. **The four rack-ear mounting slots are painted black.** The studio backdrop
   shows through them as white ovals in the render; on a real rack unit they read
   as dark holes. Boxes faded to `#090909`: `(10,10,52,40)`, `(10,203,52,42)`,
   `(2062,10,54,40)`, `(2062,201,54,44)`.
2. **The LCD opening is reshaped from 247 × 83 (3.0 : 1) to 247 × 60
   (4.11 : 1).** A real 16 × 2 module — and `lcd_reference.png` — is 4.11 : 1, so
   a correctly-proportioned character matrix could not fill the render's opening
   without either large empty margins or stretched glyphs. The bezel is extended
   over `y 80..94` and `y 155..172`, mirrored out of the surrounding bezel rows so
   the brushed texture carries through; slivers at `x 598..603` and `x 851..856`
   guarantee no green fringe survives at the ends.

## Panel

`kRefW = 2124`, `kRefH = 256` — aspect 8.297 : 1, a genuine 1U 19″ rack strip.

## LCD

**In `lcd_reference.png` (the real hardware):** glass `x 35..334, y 23..95` →
**300 × 73 px = 4.11 : 1**.

| Quantity | Value | How it was measured |
| --- | --- | --- |
| character cell pitch | **17.5 px** | char 1 starts x = 60, char 15 starts x = 305 → 245 / 14 |
| horizontal dot pitch | **2.917 px** | cell / 6 (5 dots + 1 gap column) |
| line pitch | **32.0 px** | line 1 row 0 top y = 28, line 2 row 0 top y = 60 |
| vertical dot pitch | **3.28 px** | `R`'s stem spans y 28..50 = 23 px over 7 rows |
| dot aspect | **1 : 1.12** | passes the 1 : 1.1–1.25 sanity check |
| char 0 cell left edge | x = 42.3 | `1`'s lit columns are 45..52 and `1` uses dot cols 1-3 |
| margins inside the glass | 7.3 left, 5 top, 12 bottom | |

The vertical dot pitch is **measured off a full-height glyph**, never derived as
`linePitch / 8` — that mistake stretched every TX81Z glyph by 57 %.

**In `panel_reference.png` (after the retouch):** glass = **`(604, 95, 247, 60)`**.
Scale from the real photo `s = 247 / 300 = 0.82333`, so:

```
kLcdX      604      kLcdY       95      kLcdW  247   kLcdH  60
kCharX0    610.0    (= 604 + 7.3*s)
kCellW      14.408  (= 17.5*s)
kLine0Y     99.1    (= 95 + 5*s)
kLineStep   26.35   (= 32*s)
kDotW        2.401  (= kCellW/6)
kDotH        2.700  (= 3.28*s)
```

### Colours, sampled from `lcd_reference.png`

| | Value |
| --- | --- |
| lit glass (blank cells, averaged) | `#3E7515` |
| ink (dark dots, averaged) | `#053A02`, darkest `#002B00` |
| bezel outside the glass | `#0A0D08` |

**The panel deliberately does NOT use the sampled glass value.** The photograph was taken
under room light with the camera stopped down so the glyphs would not blow out, which drags
the whole field dark; rendered at that value the display read as a *failing* one rather than
a working module. What ships is `#6AB81F` glass over `#05230A` ink, arrived at by rendering
the same screen at several settings and comparing each against `lcd_reference.png` directly
(`plugin/lcd_render_check.cpp`, target `d110_lcd_check`, writes the candidates as PNGs).

The other half of that fix is the **gap between dots**, which matters more than the colour.
It used to be about a fifth of a dot, which broke every stroke into separate squares. On the
reference photograph the dots within a stroke visibly run together, so the gap is now around
a tenth, and the supersampling factor went 4 → 8 to keep the smaller gap clean. Glyph shapes
are unchanged - they still come from the machine's own mask CGROM.

**It is a positive display: dark ink on a lit green field** — the opposite of the
TX81Z's bright-dots-on-dark-glass. And **there is no visible unlit dot grid**: a
blank cell profiles as a smooth 72 → 92 luminance ramp (camera vignetting) with
zero periodicity at either 2.9 or 3.3 px. Blank cells are plain glass, so the
renderer draws ink dots only and never an "off" dot. No halo either.

### The character generator

The controller is an **Oki MSM6222B**. Its glyphs match the `kFont5x7` table used
in the TX81Z plugin exactly — `R`, `l` and `y` were decoded out of
`lcd_reference.png` and compared row by row. Note in particular that **`y`'s
descender stops at dot row 6**, the 7-row rendition this table uses, *not* the
true HD44780 A00 ROM (whose descenders drop into row 8). Row 7 is never used.

**The real mask ROM has since been obtained and the table verified against all 96
printable glyphs.** `msm6222b-01.bin`, 4096 bytes,
SHA1 `e108b520e6d20459a7bbd5958bbfa1d551a690bd`, shipped inside MAME's `d110`
romset (the non-merged `d110.zip` bundles it). Results:

- **92 of 96 glyphs already matched exactly.**
- Four did not, and have been corrected in `PluginEditor.cpp`: `!` (the ROM's stem
  is one row shorter), `<` and `>` (each sat one column off), and **`0x5C`, which
  on this Japanese ROM is the YEN sign, not a backslash**.
- **Row 7 is zero for every one of the 96 glyphs**, confirming from the ROM itself
  what the photograph implied: this display uses only 7 dot rows, and descenders
  stay inside them.

The plugin now loads the ROM at startup if it is present in the data folder
(loose or still inside a `.zip`), identified by content via the `A` and `R` glyph
patterns, and falls back to the corrected built-in table otherwise. The right-click
menu reports which is in use.

`roland_d10.cpp`'s `screen_update` confirms the bit order: one byte per dot row,
**bit 4 = leftmost dot, bit 0 = rightmost**, 8 rows per cell, and the driver
draws 9 rows per line (8 cell rows + 1 spacer).

### Display contents

`lcd_reference.png` shows the real Patch Play screen:

```
12345678R RomPly     <- exactly 16 characters
1:Macho Memory
```

Row 1 = the 9 part-status slots, a space, then the mode word. Row 2 =
`<part>:<patch name>`.

**A sounding part is shown as a solid block, not a dim digit.** From munt's own
`Display.cpp`: *"the current state of the first 5 parts and the rhythm part is
represented by replacing the part symbol with the full rectangle character (#1
from the CGRAM)… shown as long as at least one partial is playing in a
non-releasing phase"*, with `ACTIVE_PART_INDICATOR = 1`. So code `0x01` is
rendered as an all-dots-on 5 × 7 block.

## Buttons

Two rows of eight flat caps, all **63 × 26**.

```
row 1 (top)     y = 80      recess above it y 76..79
row 2 (bottom)  y = 168     recess above it y 164..167
column x:  959  1033  1107  1180  1253  1327  1399  1473     (pitch 73.43)
```

Names and the scan-matrix bits they will drive once the engine work starts, taken
straight from `INPUT_PORTS_START(d110)` in MAME's `src/mame/roland/roland_d10.cpp`
(2 × 8 matrix through Roland's `mb63h149` key-scan chip):

| col | top row | SC0 bit | bottom row | SC1 bit |
| --- | --- | --- | --- | --- |
| 0 | EXIT | 0x80 | EDIT | 0x80 |
| 1 | PATCH | 0x40 | PART | 0x40 |
| 2 | TIMBRE | 0x20 | SYSTEM | 0x20 |
| 3 | PART ▲ | 0x10 | PART ▼ | 0x10 |
| 4 | GROUP ▲ | 0x08 | GROUP ▼ | 0x08 |
| 5 | BANK ▲ | 0x04 | BANK ▼ | 0x04 |
| 6 | NUMBER ▲ | 0x02 | NUMBER ▼ | 0x02 |
| 7 | WRITE/COPY | 0x01 | ENTER | 0x01 |

The driver header also documents the cold-start procedure: hold Write/Copy while
resetting, confirm with Enter — which is why the panel supports double-click
latching.

## VOLUME knob

```
centre      (367.5, 149.0)
spin radius 31       (disc body ends at r 28; a dark gap runs r 29..33)
hit radius  34
printed ticks live at r 34..43 and must NOT rotate
minDeg -146.74   maxDeg +149.96   photographed pointer -151.14
```

The radial profile is unambiguous: r 2..12 dark disc face, r 13..28 the pointer
and the disc's sheen, **r 29..33 a clean dark gap**, r 34..43 the printed tick
ring. Clipping the spin at r = 31 therefore takes the whole knob and none of the
fixed scale.

The knob **already carries a printed white pointer**, so it rides on the cut-out
and no synthetic pointer dot is needed (unlike MU-100R, whose knob had none at
that resolution). Its own angle in the photograph therefore has to be subtracted
before any rotation is applied.

### The scale, swept all the way round

21 ticks, spacing 15.09° (least-squares over all of them), spanning:

| | angle |
| --- | --- |
| outermost MIN tick | **−146.74°** |
| outermost MAX tick | **+149.96°** |
| midpoint of the two | +1.61° |
| mean of all 21 tick centres | +0.03° |
| photographed pointer | −151.14° (just past the MIN stop) |

**Measure the two end ticks in their own angular windows.** One wide sweep merges
the last two ticks at each end into a single run and pulls the answer inwards by
several degrees (it reported −145.5 / +143.8). Isolated windows plus an
intensity-weighted centroid over r 35..42 give the values above, and they are
stable to ±0.1° across four different radius bands.

`minDeg`/`maxDeg` are set to those two tick angles, because **the pointer must
land exactly on the printed MIN and MAX ticks at the ends of its travel** — a
standing requirement from the earlier panels. The 1.61° that the midpoint is off
vertical is under a pixel at this radius, so the knob still reads as pointing
straight up in the middle of its travel.

For that middle to be the *resting* position, `masterVolume`'s range was changed
from 0..1.5 to **0..2.0, default 1.0** — unity gain is now the exact centre of the
range, so the default loudness is unchanged but the knob points at 12 o'clock at
load instead of leaning right.

## Right-hand cluster

```
MIDI MESSAGE slot      (1915, 47, 83, 17)      LIT ELEMENT (1940, 56.5, 26, 3.5)
POWER cap face         (1920, 104, 75, 91)     recess/bezel (1913, 96, 85, 106)
```

**Only a short element in the middle of the MIDI MESSAGE slot is the actual
lamp.** Profiling the slot picks it out cleanly as a lighter block at x 1940..1965,
y 57..59; the bright line running the full width at y 54..55 is a specular
reflection off the window's top edge, not the part. Lighting the whole slot would
be wrong. The lamp reads **unlit** in the render, so nothing has to be painted out
before drawing the live one.

### What drives the lamp

In the current mt32emu engine, from `Display::checkDisplayStateUpdated()`: it is on
if a MIDI message has been played since the last reset — an 80 ms blink,
`BLINK_TIME_MILLIS` — **or** if any of the eight **voice** parts is currently
sounding. The source notes explicitly that "the LED represents activity of the
voice parts only", so the Rhythm part does not light it.

On the real hardware the firmware drives it directly: **bit 0 of the SO register at
0x0200**, per `so_w()` in MAME's `roland_d10.cpp` (`// bit 0 = led`). That is what
the lamp should be rebound to once the firmware runs, and it will also settle
whether it flashes during power-on self-test — which cannot be answered from
mt32emu, since mt32emu never executes the startup code.

## Decorative only (no hit region)

```
PHONES jack       centred (204, 150), Ø 56
MEMORY CARD slot  around  (1588..1846, 100..166)
```

## Compact mode (Utility tab, "PANEL SIZE")

`docs/panel_reference_compact.png`, 1497 × 256, is `panel_reference.png` with
three sections cut out and the three remaining strips rejoined:

- `[0, 244)` - the Roland wordmark and the PHONES jack (purely decorative, no hit
  region at all - see "Decorative only" above), plus the left rack ear. Cut right
  after "Roland"'s own "d" (ends x 237) and before "D-110"'s own "D" (starts
  x 259, cyan text detected by `b>150 and b>r+30`), leaving the "D-110 MULTI
  TIMBRAL SOUND MODULE" wordmark and the VOLUME knob as the new leftmost content -
  Alan's request, 2026-08-20, matching a mockup he supplied ("only the
  essentials").
- `[1560, 1865)` - the whole MEMORY CARD section (label, slot, and its
  surrounding dead brushed-metal margin), added 2026-08-20 in the first pass at
  this feature.
- `[2046, 2124)` - the right rack ear, trimmed by Alan himself in a second pass
  the same day, re-editing `panel_reference_compact.png` directly rather than
  asking for a code change. Measured back out of his file by cross-correlating
  it against `panel_reference.png` (`np.abs(candidate - strip).mean()` over
  candidate offsets, zero-diff exact matches at every seam) rather than assumed -
  both earlier seams came back identical to the first pass, only this third cut
  was new. Safe with no further code change since nothing hit-tested or painted
  ever sat past x 2046 (`kBezelX+kBezelW=1998`, `kLampX+kLampW=1966`, both well
  inside).

All three seam pairs were picked (his by eye, the first two originally by
profiling) for flat background clear of the button grid (which ends at x 1536,
measured off cap-edge luminance jumps), the VOLUME label/tick ring
(x 315..425), and the MEMORY CARD/MIDI MESSAGE text (x 1690..1810 and x 1887..
respectively). Not regenerated at runtime - `panel_reference_compact.png` is a
plain checked-in asset, editable by hand like any other.

`D110Panel::mapX(refX, compact)` is what every reference-space X coordinate used
to paint or hit-test something - `kButtons[i].x`, `kKnobCx`, `kLcdX`, `kPowerX`,
`kBezelX`, `kLampX` - goes through instead of being used raw: subtract
`kCompactLeftCutEnd` (244) always, and `kCompactCardShift` (305, = 1865 − 1560)
on top of that for anything at or past the card section. The MEMORY CARD slot's
own hit region and the `D110MemoryCard` component are simply skipped/hidden
rather than mapped, there being nothing left of them to click.
`D110Panel::kCompactRefW` (1497) is the actual measured width of the checked-in
file, not derived from the two `mapX` cuts alone (the third, right-edge trim
isn't one of them) - if the file is ever re-cropped again, this constant has to
be updated to match, by measurement, the same way. `currentRefW()` (`kRefW` or
`kCompactRefW`) is what every window-sizing calculation in `PluginEditor.cpp`
reads instead of the bare
`kRefW` constant, so the aspect ratio, zoom percent and drawer widths all narrow
along with the panel itself.

## Alt view (issue #10, 2026-10-20)

Utility -> ALT VIEW (`processor.getAltPanelView()`, saved as `altPanelView`) swaps in Alan's flat
artwork `panel_reference_alt.png` (2166 x 269) and `panel_reference_compact_alt.png`
(1598 x 269). Those two differ from the photographs in size and layout, so everything below was
re-measured, on copies resampled to the 256 px reference height (`tools/make_alt_panels.py` ->
`panel_reference_alt_256.png` 2061 x 256, `panel_reference_compact_alt_256.png` 1521 x 256; the
256 px height is what `D110Panel::kRefH` and the drawer layout assume). The alt skin is the
`kSkinAlt` struct in `PluginEditor.cpp`; the normal skin is `kSkinNormal`.

| Item | Value (alt256 pixels) | How |
| --- | --- | --- |
| button outer rects | x 985, 1062, 1140, 1216, 1293, 1369, 1445, 1521; w 73; rows y 74 and 168, h 30 | black 2-3 px outline columns/rows at fixed luma |
| LCD glass | `(621, 95, 264, 69)` | bounding box of the green field |
| glyph grid | normal grid x 264/247 = 1.0688, line 0 at y 101 | uniform scale, window is 3.83:1 |
| POWER face / hit | face `(1884,104,71,99)`, outline `(1880,100,79,107)` | outline columns/rows |
| MIDI lamp | `(1884, 52, 77, 6)` | inner fill of the slot |
| VOLUME knob | centre `(381.6, 156.9)`, tick ring R 43.1, 21 ticks, MIN -141.9, MAX +143.17 deg | circle fit on the tick centroids |

Compact alt is a pure splice of the full alt (checked column by column: offset 250, then 474
after the card section): left cut `[0,250)`, card cut `[1626,1850)`, width 1521, so `mapX()` has
its own numbers for it (`kAltCompact*` in `PluginEditor.h`).

The alt knob photo carries a baked pointer, so `volume_knob_alt_disc.png`
(`tools/make_alt_knob.py`) is the knob lifted from the photo with that pointer inpainted from
the body's radial profile, laid over it at integer offset (348,123); the panel then draws its
own pointer. The normal knob works the same way (`volume_knob_disc.png`, no rotation of the
disc, so its lighting stays put - only the pointer turns).

ALT VIEW also disables the MEMORY CARD: the slot is not hit-tested and `D110MemoryCard` stays
hidden, whatever its insert/eject state.
