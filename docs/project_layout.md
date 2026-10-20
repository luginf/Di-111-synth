# Project layout

- `munt/` - the sound engine (`mt32emu`), vendored from the fork above with a couple of local
  fixes (see `munt/mt32emu/src/Display.cpp` for the LCD buffer/part-count corrections made for D-110).
- `plugin/Source/native/` - the default backend: the D-110's own CPU reimplemented from scratch,
  zero MAME dependency (`D110EmulatorNative`).
- `plugin/Source/D110Core.*` - the original, MAME-backed control board: runs MAME's `d110`
  machine on its own thread with a headless OSD, and exposes the display, the sixteen buttons
  and the firmware's parameter memory. Opt-in (`D110_BUILD_MAME_BACKEND`), not built by default.
- `plugin/Source/PluginProcessor.*`, `PluginEditor.*` - the JUCE plugin and the photo-composite panel.
  Utility tab's "PANEL SIZE" toggle (`processor.getCompactPanelMode()`) swaps in
  `docs/panel_reference_compact.png`, the same photo with the MEMORY CARD section spliced out
  and the window narrowed to match - see `panel_reference_notes.md`'s own "Compact mode" section
  for the seam geometry and `D110Panel::currentRefW()`/`kCompactShift`.
  Utility tab's "ALT VIEW" (`processor.getAltPanelView()`) swaps in `panel_reference_alt_256.png` /
  `panel_reference_compact_alt_256.png` (Alan's `panel_reference_alt.png` / `_compact_alt.png`
  resampled to the 256 px reference height by `tools/make_alt_panels.py`) with its own measured
  geometry (`Skin` in `PluginEditor.cpp`), a pointerless knob sprite plus a drawn pointer
  (`tools/make_alt_knob.py`), and the memory card disabled - see `panel_reference_notes.md`'s
  "Alt view" section. `UiTheme.h` also holds the drawer's text helpers (`bodyFont`, `headingFont`)
  and `sequencerBackground()`.
- `plugin/Source/D110Keyboard.h/.cpp` - the on-screen test keyboard (mouse piano + tracker-
  style PC keyboard input, MIDI channel/remap routing via right-click), extracted out of
  `PluginEditor.*` so it can be reused outside the plugin. Talks to its owner only through
  `plugin/Source/D110KeyboardHost.h` (note injection + its own persisted config);
  `D110AudioProcessor` implements it for the plugin. Keys light up both instantly
  when struck locally and, polled at ~30Hz, for any note reaching the app another way
  (external MIDI In, sequencer playback, a DAW host track) via `D110KeyboardHost::isNoteActive()`.
- `plugin/Source/sequencer/` - the D-20-style multitrack sequencer (`D110SequencerEngine` +
  `D110SequencerPanel`), a third foldable drawer alongside the editor and test keyboard. See
  [`sequencer.md`](sequencer.md). `plugin/sequencer_probe.cpp` and
  `plugin/sequencer_state_probe.cpp` are its headless tests (engine timing/quantize/step-
  recording/undo/file-I/O, and the state-save round trip, respectively). `D110SequencerHost`
  is the whole interface the panel needs from whatever embeds it (12 methods); `D110AudioProcessor`
  implements it for the plugin.
  `D110SequencerSongsFile.h/.cpp` (de)serializes the 4 song slots to/from XML, shared by the
  plugin's own project state and its `.midiseq` export/import.

  **Nonet Sequencer** - the same engine/panel, standalone (no firmware/ROMs/plugin wrapper),
  used to be built from this repo too (`NonetSeqHost` + `NonetSeqMain.cpp` implementing/
  wrapping `D110SequencerHost` and `D110KeyboardHost`, target `Nonet-Seq`) - extracted into
  its own repository 2026-09-22, see [github.com/luginf/nonet-sequencer](https://github.com/luginf/nonet-sequencer)
  (there renamed to drop the `D110*` prefix: `SequencerEngine`/`SequencerPanel`/`Keyboard`
  instead of `D110SequencerEngine`/`D110SequencerPanel`/`D110Keyboard`). This repo's own
  `D110Sequencer*`/`D110Keyboard*` files, and the D-110 plugin's in-plugin drawer they power,
  are unaffected by that split - only `NonetSeqHost.h/.cpp`, `NonetSeqMain.cpp`,
  `keyboard_activity_probe.cpp` and the `Nonet-Seq`/`d110_keyboard_activity_probe` CMake
  targets moved.
- `plugin/mame.cmake` - the MAME library/include/define lists and how the subset was built.
- `docs/` - the measured panel geometry and the SysEx address map, both derived by profiling
  rather than by eye. Every number in the code is justified there.
- `plugin/Source/JackMidiInput.h/.cpp` - optional (Linux + libjack dev headers found at configure
  time), Standalone-only real JACK MIDI input port (`D-110 Emulator:midi_in`), separate from the
  ALSA-based "MIDI ports opened directly" picker in the Options menu. See
  [`host_compatibility.md`](host_compatibility.md) for both this and known VST3 host quirks
  (Carla/Qtractor).
- `rom_test/` - a console tool that renders a test chord from a Control + PCM ROM pair.
- `plugin/audio_test.cpp` - offline check with no DAW: powers the plugin on, plays a note, proves
  a panel edit changes the sound, checks that re-sending an *unedited* state changes nothing, and
  sweeps all sixteen MIDI channels reading the part indicators off the firmware's own display.
- `plugin/tone_probe.cpp` - locates the Tone Temporary Area by reading the engine's copy of each
  tone back out and matching it against the firmware's RAM. An exact match is simultaneously the
  measurement and the null test.
- `plugin/tone_edit_survives_probe.cpp` - edits a tone from the panel on an ordinary patch, then
  changes a part parameter, and checks the sound engine still holds the edited tone. Finds both
  menu pages by pressing buttons and watching which RAM byte moves, and runs the experiment twice
  - with the tone re-assert on and off - so the result has a control that can show the failure.
- `plugin/state_test.cpp` - edits a parameter, saves the plugin state, restores it into a fresh
  instance, and checks the edit came back; also checks a plugin loaded with no saved state finds
  the memory the last one left, and that a second instance refuses to power on.
- `plugin/two_instance_test.cpp` - records what really happens when two machines run in one
  process. Diagnostic, not a fix.
- `plugin/panel_render.cpp` - snapshots the real panel to PNG, as a storyboard every 100 ms,
  so the card's travel is judged by looking at it rather than from constants. It drives the
  card by clicking the slot, so the whole path from the mouse to the frame is what gets checked.
- `plugin/card_probe.cpp` - the memory card. Puts four different cards in the slot, each
  differing from the next by one property, and prints what the firmware said about each; then
  formats a blank card, saves to it, wipes the instrument with a factory reset and loads it back,
  comparing three snapshots of the battery RAM. See [`memory_card.md`](memory_card.md).
- `plugin/bridge_probe.cpp`, `core_test.cpp` - the harnesses used to map the firmware's RAM and
  to exercise the control board on its own.
- `plugin/pcm_custom_sample_probe.cpp` - loads a synthetic WAV into a PCM wave slot, checks
  `hasCustomPcmWave()` flips on load and off after `restoreFactoryPcmWave()`, and round-trips the
  override through `getStateInformation`/`setStateInformation` into a second instance. See
  [`architecture.md`](architecture.md#custom-pcm-samples-desktop-only).
- `plugin/pcm_pitch_calibration_probe.cpp` - a one-off measurement tool for the PCM pitch
  calibration constant, kept for reference rather than as a trusted regression test - its own
  zero-crossing frequency measurement has an unexplained bug (identical readings across
  unrelated Tone-parameter changes) and, separately, once suggested a "better" constant that a
  real listening test showed was actually worse. Don't ship a pitch-constant change on this
  probe's numbers alone.
