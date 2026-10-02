# ROM files: full requirements, checksums, and re-initializing the firmware

See the [README](../README.md#get-started) for the short version. This is the detail:
what each file is for, how to confirm you have the right dump, and how to reset the
firmware back to factory settings if you ever need to.

You can get the D-110 roms at https://mdk.cab/ search for d110.7z


## Where to put the files

Checked in this order - whichever one actually has ROMs in it wins:

1. **A custom folder you point it at yourself** - Utility tab, "ROM FOLDER" (click to pick one,
   right-click to go back to automatic detection). Takes effect on the next power-on. If no ROMs
   were found at all, the same folder picker also pops up on its own as soon as the editor
   opens and takes effect immediately, without a manual power cycle.
2. **The default: Di-111's own folder** - one neutral per-user location, shared by every format (VST3,
   CLAP, AU, Standalone) rather than tied to one of them. The firmware memory (`nvram/`) sits beside the ROMs:
   - Windows: `%APPDATA%\Di-111\roms\`
   - macOS: `~/Library/Application Support/Di-111/roms/`
   - Linux: `~/.config/Di-111/roms/`
3. **Folders earlier versions used** - still found, so an existing install keeps working (and keeps its
   NVRAM) without moving anything, as long as the ROMs are there:
   - Colocated with the shared VST3 folder: `C:\Program Files\Common Files\VST3\D-110 Data\`,
     `~/Library/Audio/Plug-Ins/VST3/D-110 Data/`, `~/.vst3/D-110_Data/`
   - The older app-data folder: `%APPDATA%\D-110 Emulator\D-110 Data\`,
     `~/Library/Application Support/D-110 Emulator/D-110 Data/`, `~/.config/D-110 Emulator/D-110 Data/`
4. **Loose, directly beside the VST3 bundle itself** (the shared VST3 folder, but with no `D-110 Data`
   subfolder at all - e.g. straight in `~/.vst3/` on Linux) **or directly beside the Standalone binary**.
   Only the files themselves are picked up from there (copied into the default folder from #2 the first
   time they're found) - nothing else in that shared folder is ever touched or scanned recursively.

`D-110_Data` (underscore) is also accepted at the older locations in #3.

## Where files are matched by name vs. by content

One set of chip dumps serves both the control board and the sound engine, but the two halves
find their files differently, which matters for what you actually need to put in the folder:

- **The control board (both backends) needs three files present under their exact chip
  names** - it opens them by that literal filename, not by content:
  - `d-110.v1.10.ic19.bin` (32,768 bytes) - the control firmware
  - `r15179873-lh5310-97.ic12.bin` (131,072 bytes) - the presets ROM
  - `msm6222b-01.bin` (4,096 bytes) - the LCD's character generator ROM
- **The sound engine** (Control/PCM/BOSS reverb images) recognises files by **content, not
  name** - already-combined `mt32emu`-style Control and PCM images work (whether they match a
  known whole-image checksum, or are simply the two chips concatenated in either order - either
  is split back apart and matched chip by chip), and so do the loose chip dumps above plus:
  - `r15179880.ic8.bin` and `r15179878.ic7.bin` (524,288 bytes each) - PCM wave ROMs, joined
    IC8+IC7
  - `r15179879.ic6.bin` (32,768 bytes) - the BOSS reverb chip's program ROM, optional (only
    the reverb emulation needs it)

If the sound engine recognises a Control image that didn't already exist under the control
board's own two exact filenames above, the plugin writes those two files out itself (derived
from the image it just recognised) the next time it powers on - so you don't have to rename or
split anything by hand, whichever of the two file shapes you happened to download.

A `.zip` still in its original romset packaging is only recognised by the sound engine's
content scan, **not** by the control board's fixed-filename lookup above - extract it if you
want to use `D110EmulatorNative` (or `D110Emulator` built without MAME's own separate romset
search). Whatever you have, drop it in loose and let the plugin sort out what it recognises.

Right-click the panel to see what was recognised. An `nvram` folder that shows up in this
same folder afterwards is not a ROM you need to supply - it is the instrument's own battery
RAM, created and updated automatically as you use the plugin.

## Checksums

MD5 checksums, to confirm a dump is the right one before dropping it in (the two rows with two
filenames are the same content under two different naming conventions - either name works,
since files are matched by content, not name, everywhere except the three loose chip dumps
listed above):

| MD5 | File(s) |
|---|---|
| `d8aa6bb3628a35b6fe1cd205c2ab8f62` | `d-110.v1.10.ic19.bin` |
| `faa1960f26a73eed65175762c0527cd6` | `r15179873-lh5310-97.ic12.bin` |
| `c7596739df7e599be1a189a42c05a8b3` | `msm6222b-01.bin` |
| `ba6fa6a8f9892dacd6009f52225ac2a2` | `r15179878.ic7.bin` |
| `61ad8efa5e78c19be691b2b2e2ddda4b` | `r15179880.ic8.bin` |
| `253105885d590332a802157a0e609e59` | `r15179879.ic6.bin` (BOSS reverb, optional) |
| `169a6657650c3c5d861c67689dcf73cc` | `ctrl_d110_v1.10.bin` / `D-110_Control.bin` (pre-assembled Control image) |
| `f5e9349493b13d0d13313afc10803a98` | `pcm_d110.bin` / `D-110_PCM.bin` (pre-assembled PCM image) |

If you only have `D-110_PCM.bin` and `D-110_Control.bin`, the one extra file you need is
`msm6222b-01.bin` - the LCD character-generator chip is never part of a Control or PCM image,
so there is nothing to recognise it by content; it must be present under that exact name.

## Factory defaults and first boot

> **Note:** by factory default **nothing plays on MIDI channel 1**. Part 1 listens on channel
> **2**, part 2 on 3, and so on to part 8 on channel 9, with rhythm on 10. That is how the
> hardware behaves, not a bug - see
> [`factory_defaults.md`](factory_defaults.md) for the full factory state (channels,
> pan, key ranges, partial reserve) read straight off the firmware's own display.

The very first time the plugin is ever used it performs the documented cold start once, so it
comes up at factory settings instead of showing the empty patch a D-110 with blank battery RAM
really does show. The result is kept and every instance created afterwards is seeded from it,
so **loading the plugin never makes you sit through initialisation**.

## Re-initializing the firmware

To initialise it again later, do what you would do on the hardware - **hold WRITE/COPY while
switching on, then confirm with ENTER**:

1. Switch **POWER** off.
2. **Ctrl+click** (or Alt+click) **WRITE/COPY**. The cap latches down, exactly as if you were
   holding it.
3. Switch **POWER** on. The firmware comes up seeing the button held and asks to confirm.
4. Ctrl+click **WRITE/COPY** again to release it.
5. Click **ENTER** to confirm.

The order of the last two steps matters and is easy to get backwards - release WRITE/COPY
**before** confirming with ENTER, not after. Confirming while the cap is still latched down
does nothing, because the firmware is waiting to see WRITE/COPY come back up before it will
read ENTER as the answer to its own prompt.

Ctrl+click latches any of the sixteen caps, which is what makes "hold this button while
switching on" possible with a single mouse. It is on a modifier rather than on a long press on
purpose: **the firmware repeats a held button**, so a long press is how you scroll a value, and
that has to keep working.
