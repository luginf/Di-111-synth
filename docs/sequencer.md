# The sequencer

A D-20-style multitrack MIDI sequencer, a foldable drawer under the D-110 panel (closed by
default). It has its own internal clock - it is **not** synced to the host's tempo or transport,
the same way the original hardware's own sequencer wasn't.

**It also exists as a fully independent standalone app, no D-110/firmware/ROM dependency at
all: Nonet Sequencer (binary `Nonet-Seq`).** Same engine, same panel, same feature set - see
[Nonet Sequencer - the independent app](#nonet-sequencer---the-independent-app) below for what's
different about running it on its own.

![screenshot](nonet-seq.png)

## Tracks and channels

9 tracks: **Part 1-8**, then **Rhythm** (16 in Nonet Sequencer - see [Extra
tracks](#extra-tracks-10-16)). A track always plays on whatever MIDI channel the corresponding
D-110 part is currently set to on the SYSTEM page (Rhythm is fixed on channel 10). **In Nonet
Sequencer**, which has no SYSTEM page, the channel readout is clickable instead - a 1-16 picker
per track.

Each row: **MUTE**, **SOLO** (soloing any track silences every non-soloed one), **ARM**
(record-enable; arming one track disarms whatever else was armed), a channel readout, and a bar
showing whether the track has recorded events. Right-click anywhere on a row for **Rename
track...**, quantize, and the destructive per-track operations (clear/delete/copy/transpose
bars).

## Transport

**STOP** / **PLAY** / **REC**, a draggable **TEMPO** field (20-300 BPM; drag vertically, mouse
wheel, or right-click to type an exact BPM), a **TAP** button (click a few times at the beat you
want), a **time signature** field (click cycles common presets, right-click picks one directly or
**Custom...**), and bar navigation (prev/next, a draggable **BAR n/total** readout).

**STOP** always sends a MIDI panic (all notes off) as well as halting the transport, so a note
never gets left stuck sounding. **Right-click STOP** sends that same panic without stopping.
**Right-click PLAY** starts from bar 1 instead of resuming from the current position.

Right-click the **BAR** readout for *Go to bar...*, punch in/out, and all-tracks Delete/Copy/
Transpose bar(s).

### Metronome, precount, loop and punch

**METRO** toggles the metronome; right-click for visual/audio/both, whether it fires real notes
on the rhythm channel, and a volume submenu. **PRECOUNT** cycles 0-2 bars of click-only lead-in
(doesn't advance the bar counter). **LOOP** cycles OFF -> BAR -> PUNCH (loops the punch-in/out
range); while PUNCH is active, recording is also restricted to that range.

## Recording

### Real-time

Arm a track, press **REC** (or click it while stopped). **REC: mode** (click to cycle) picks how
the take is folded into the track:

**Nonet Sequencer only:** capture accepts any incoming MIDI channel while a track is armed - a
single controller plugged into MIDI In records onto whichever track is armed regardless of what
channel it actually sends on. The D-110 plugin, which can have a DAW driving several Parts on
their own distinct channels at once, still requires the incoming channel to match the armed
track's own.

- **Overdub** - adds new notes; nothing already there is removed.
- **Replace** - erases only the span you actually recorded.
- **Replace to end** - erases everything on the track from the take's start onward.

### Step recording

Enter notes with no regard for real time: play a note or chord and let go (or press **REST**),
and the write cursor advances by one step.

- **STEP** arms/leaves step mode. Starting it stops a real-time take in progress, and vice versa.
- **Step duration** sets how far one step advances (whole down to 1/32, plus triplets).
- **DOT** multiplies the current step's length by 1.5, like notation.
- **Chords**: hold several notes together - they land on the same step, committed once every
  note is released.
- **BACK** undoes the most recently committed step and moves the cursor back.
- A live **"Bar n step m/N"** readout shows the cursor's position.

Notes played while recording are still audible through the firmware as they're entered.

## Quantize

Right-click any track row for **Quantize**: off, or a grid from 1/4 down to 1/32 (plus
triplets). What a grid actually does depends on the workspace-wide **Quantize mode** (D-110
plugin: Utility tab or the sequencer's own REC MODE menu; Nonet Sequencer: OPTIONS or the
sequencer's own OPTIONS list):

- **HARD** (default) - moves note-on/off for real onto the nearest grid line, note length
  preserved. Rewrites the track's own data (reachable back through UNDO only as long as nothing
  else has been done since).
- **SOFT** - the recording is never touched; every note is snapped live on playback instead.
  Picking a track's quantize back to **off** instantly restores the exact original performance.

## Editing bars

From the per-track right-click menu, or the BAR readout's menu (every track at once):

- **Clear track** - erases every recorded event on one track.
- **Delete bar(s)** - removes a bar range and closes the gap, shifting everything after it
  earlier. Scoped to one track only ripples that track.
- **Copy bar(s) to...** - copies a bar range and inserts it at a destination bar (never
  overwrites). Can target a different track; only notes travel, never the source's channel.
- **Transpose bar(s)** - shifts pitch in a bar range by semitones, in place.

**NEW** clears every track in the current song slot, resets tempo to 120, and clears any fixed
per-track Program Change/Bank/Volume/Pan override - only for the slot you're on.

## Editing single notes

**"Edit events in bar N..."**, at the bottom of the per-track right-click menu, opens a
scrollable list of every note in the current bar on that track: beat position, note name,
velocity, duration. Click a row to change pitch, or its **X** to delete. A **"< Bar N >"** strip
moves to the previous/next bar without closing the dialog.

## Undo

**UNDO** reverts the most recent of: quantize, clear track, delete/copy/transpose bars, new
song, or copy-song-to-slot - up to 20 levels deep. Doesn't touch playback/transport state.

## Song slots

4 independent songs (tempo, time signature, all tracks), switched with the 4 numbered buttons -
a dot marks which slots have content. Right-click any slot button to copy the current song into
another slot.

### Per-song sound snapshot (D-110 plugin only)

Different songs usually want different instruments. Right-click a song-slot button for **Store
current sounds in Slot N** / **Load Slot N's stored sounds**: Store captures the instrument's
entire memory (every Patch, Timbre, Tone, System byte) into that slot; Load writes it back and
power-cycles the instrument to apply it (asks for confirmation first - not undoable). Separate
from [Per-track Program Change](#per-track-program-change) below, which only nudges one Part to
an already-stored Timbre.

## Per-track Program Change

Click a track's **CH** readout for **Program Change...** - pick a program 1-128 and a bank
1-128, or leave it blank for none. Sent once, the moment PLAY/REC starts. **Rhythm** (D-110
plugin only) has no Program Change equivalent, so its dialog reads **CC Change** instead -
Volume/Pan only.

**Nonet Sequencer only:** load a MusE-style instrument definition file (**OPTIONS > Instrument
Definition (.idf)**) to pick programs by name instead of by number. Once loaded, the Program
Change dialog gets a **Pick instrument...** button listing every patch the file defines,
grouped the same way the file groups them; picking one fills in Program (and Bank/Bank LSB,
when the file sets them) - still just plain numbers in those fields afterwards, so a pick can
be nudged by hand before OK.

Wire format differs between the two apps (Nonet Sequencer sends real MIDI Bank Select/CC7/CC10;
the D-110 plugin uses its own native SysEx addressing, since the real D-110 predates MIDI Bank
Select and has no MIDI Channel Volume/Pan concept at all). This is per-song-slot data, exactly
like a track's own notes.

**SYNC** (mouse view: button next to UNDO/REDO; retro view: OPTIONS) moves data between every
track's stored Program Change/Bank/Volume/Pan and the live patch: **Send** re-sends stored
values to the live patch; **Capture** (D-110 plugin only) reads what's live on each part and
overwrites every track's stored settings with it.

## MIDI Out (driving external gear)

Whatever the sequencer plays back also reaches the direct **MIDI Out** port, if one is selected
(right-click the panel, *MIDI Out* submenu) - a real OS MIDI port, independent of host routing,
so it works identically standalone or inside a DAW. Each track goes out on whatever channel its
D-110 part is live on.

## Nonet Sequencer - the independent app

**Nonet Sequencer** (binary `Nonet-Seq`) is the sequencer on its own - no firmware, no ROMs, no
plugin wrapper, not even a sound engine. A plain 9-track MIDI sequencer, not tied to any one
instrument. Same panel and engine as inside the plugin, in a bare window with a toolbar: **MIDI
In**/**MIDI Out** (click to pick a system port), and **OPTIONS** (theme, Program Change/Bank
offset corrections, extra-tracks toggle, audio device). Every track defaults to the factory
D-110 channel map (Part 1-8 -> channels 2-9, Rhythm -> 10), so a real D-110 on factory defaults
just works from this app's MIDI Out. Unlike the plugin, this app also thrus MIDI In straight to
MIDI Out, since there's no internal synth to hear what you're playing while you record.

**Extracted into its own repository 2026-09-22** -
[github.com/luginf/nonet-sequencer](https://github.com/luginf/nonet-sequencer). Used to be a
build target of this repo purely because that's where the code originated; the D-110 plugin
above keeps its own copy of the same engine/panel (`D110Sequencer*`) for its in-plugin drawer,
unaffected by the split. See that repo's own README for build instructions.

Underneath the transport is the same on-screen test keyboard the plugin has - a two-octave mouse
piano plus optional PC keyboard input, right-click for MIDI routing (a fixed channel, or MIDI
Remap off to broadcast to all 16 at once).

Its own state lives in its own settings file, separate from the plugin's:

- Linux: `~/.config/Nonet Sequencer.settings`
- macOS: `~/Library/Application Support/Nonet Sequencer.settings`
- Windows: `%APPDATA%\Nonet Sequencer.settings`

Everything else - recording, step recording, quantize, bar editing, undo, song slots, Load/Save
- works exactly as described above. Standalone only, no VST3/plugin-format build.

### Extra tracks (10-16)

Nonet Sequencer only - the D-110 plugin stays fixed at 9 tracks. Right-click the blank strip
above the track rows for **Activate extra tracks (16 total)**: two buttons appear, **1-9** and
**10-16**, switching the track list between the original 9 and 7 more plain MIDI tracks. Extra
tracks work exactly like the other 9. Undo, song slots and file save/load always cover all 16
regardless of whether the toggle is on.

## Load / Save

A plain click on **LOAD**/**SAVE** loads/saves just the current song as a standard `.mid` file,
including Program Change/Bank and Volume/Pan where set. Each track's own MIDI channel is also
asserted via a small SysEx message at the start of the file, so the exported channel numbers
still land on the right Part even when the receiving D-110's own SYSTEM-page channel map
doesn't happen to match the one the song was made with. Reimporting into the D-110 plugin
restores all of it (Program Change, custom internal tones, Volume/Pan, channel map); Nonet
Sequencer has no firmware to restore this into.

**Right-click** LOAD/SAVE for all 4 song slots at once, as a single portable `.midiseq` file.

## Retro mode (D-20 style LCD)

A second, complete UI for everything above - a small text LCD plus 9 hardware-style buttons
(STOP/PLAY/REC, a D-pad, ENTER, BACK) instead of the mouse-driven grid. It replaces the
sequencer drawer entirely when switched on. Pick it in the **Sequencer > Classic / Retro / Grid**
submenu (panel right-click in the D-110 plugin; right-click on the toolbar or window in Nonet
Sequencer, or its OPTIONS > Sequencer view row; Options in the hamburger menu on Android - the
JV-880 emulator offers the very same submenu, see `sequencer/SequencerViewMenu.h`). The D-pad also
works from a real keyboard once the panel has focus, with fully customizable key bindings
(OPTIONS > KEY BINDINGS).

HOME is one scrollable list holding every top-level item: transport quick-bar, TEMPO/SIG/METRO,
PRECOUNT/LOOP, SONG (slot switching), BAR, then one row per track. BACK always gets you back to
a known place - it's a full STOP while playing/recording, otherwise it pops one menu level, and
jumps to HOME's top row once there's nothing left to pop.

## Grid mode (piano roll)

A third view of the same drawer, for entering and correcting notes by mouse instead of playing
them: one track and one bar at a time, pitches down the side (the piano-key column plays the
note when clicked), steps across (16 per bar in 4/4 at the default 1/16 grid), and a velocity
lane underneath. Switch to it from the same **Sequencer** submenu as Retro mode; the three views
are mutually exclusive. It shows the same song as
the other two: edits appear there and the other way round.

- **Transport row**: STOP/PLAY/REC, tempo, time signature, METRO, PRECOUNT, LOOP and the bar
  buttons sit at exactly the same place as in the normal view (REC records into the selected
  track). UNDO/REDO are on the third row, at the same columns as the normal view.
- **Song slots**: the four numbered buttons on the third row switch between the 4 songs (a dot
  marks the ones holding notes), at the same columns as in the normal view. Copying a song to
  another slot and the per-slot sound snapshots stay in the normal view's right-click menu.
- **Bar and track**: `<` / `>` move the view bar (it is the transport's current bar, so LOOP
  loops what's on screen and playback pages the view along by itself; `>` goes one bar past the
  end so a song can be extended). The numbered buttons pick the track, MUTE/SOLO act on it.
- **Add**: click an empty cell. The note is as long as the LEN button says, and dragging right
  before releasing stretches it (the next click then reuses that length).
- **Remove**: click a note without moving, or right-click it.
- **Move**: drag a note's body (step and pitch). **Resize**: drag its right edge.
- **Middle button**: the same move/resize, but ignoring the grid (as if GRID were OFF), and a
  plain middle click leaves the note alone.
- **Velocity**: drag a note's stick in the lane up or down; new notes use the VEL button's value.
- **GRID** picks the step size (1/4, 1/8, 1/16, 1/32 and the 1/8 and 1/16 triplets), or **OFF**:
  no snapping at all, notes are added, moved and resized at any position (the 1/16 columns stay
  as a guide, and LEN counts those columns).
- **ROW** steps the height of the pitch rows (14, 20, 28, 38 px; Ctrl+wheel does the same on a
  desktop) and is remembered between launches. The taller sizes are for fingers: the piano-key
  column, scroll bar, velocity lane and hit zones grow with it. Dragging the piano-key column
  scrolls the pitch range, so no wheel is needed on a touchscreen.
- The wheel or the scroll bar moves the visible pitch range. Notes held over from an earlier bar
  are drawn dimmed and can be edited from the bar they start in.

On Android it is the "Piano Roll Sequencer" entry of Options (rows start at 28 px there), the
bar-menu button in the last transport column opens the app menu, and holding a transport button
for half a second acts as its right-click. Tapping a note removes it, and GRID OFF replaces the
middle-click free move.

Every gesture is one UNDO step, and edits made while playing are heard as soon as the playhead reaches them. A
note is never made to overlap another note of the same pitch, since that would cut one of them
short on playback.

## Verification

`plugin/sequencer_probe.cpp` is the headless test suite covering timing, quantize, both
recording modes, loop/punch, bar editing, step recording, undo, song slots and the grid editor's
note primitives (including editing a note while it sounds).
`plugin/sequencer_state_probe.cpp` round-trips the engine's full state and checks it comes back
identical.


## Importing a MIDI file written for a re-channelled D-110

Files exported from a D-110 (or by this app) start each track with the SysEx that remaps that
Part's MIDI channel. On import (D-110 plugin) each track's Program Change / Volume / Pan are
therefore sent on the channel that SysEx establishes, not on the channel the Part has *right now*,
and the SysEx is replayed first. Display Message SysEx in a file is skipped: it is cosmetic, and a
message still pending when the instrument is saved used to leave the firmware answering to a
different SysEx device ID on the next start (see `.claude/dev-notes/architecture.md`). Replayed
writes use the device ID the firmware currently answers to (its *Exclu Unit#*).
