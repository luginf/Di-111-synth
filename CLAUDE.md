# D-110 VST Emulator - notes for Claude

A VST3/standalone emulator of the Roland D-110 multi-timbral sound module. It runs the
**real Roland firmware** (not a reimplementation of the menus/editor logic) against an
emulated LA32 sound engine (`munt`/mt32emu, D-110 fork). See `README.md` for the full
user-facing picture - architecture, ROM requirements, known limits, panel/editor
behaviour. Don't duplicate that here; this file is about how to work in the repo.

## Two CPU backends - know which one you're touching

- `plugin/Source/native/` - **`D110EmulatorNative`, the default and only actively
  developed backend.** The D-110's i8x9x/MCS-96 CPU reimplemented from scratch, zero
  MAME dependency, stepped inline on the audio thread (fixes 0-18ms MIDI jitter the
  MAME backend has). This is what CI builds (Windows/macOS) and what ships in releases.
- `plugin/Source/D110Core.*` - the original MAME-backed backend (`D110Emulator`), runs
  the firmware inside an embedded MAME `roland_d10` driver instance. Opt-in only
  (`-DD110_BUILD_MAME_BACKEND=ON`), needs a separately built MAME 0.288 tree with a
  local patch (`patches/mame_mcs96_stale_irq_level.patch`). Kept in the tree as a
  dormant fallback, not deleted, but **not to be shipped in packaging/releases
  anymore** (Alan's call, 2026-08-05) - see the memory note `feedback_native_only_releases`.
  Don't propose reviving it in `.deb`/CI unless explicitly asked.
- Both share the same JUCE plugin shell (`PluginProcessor.*`, `PluginEditor.*`) and the
  same firmware ROMs; they build as two separate plugin targets that can sit side by
  side in a DAW.

## Layout quick-reference

- `plugin/Source/PluginProcessor.*` - JUCE `AudioProcessor`: MIDI in/out routing to the
  firmware, `osMidiCollector` (`juce::MidiMessageCollector`) is the queue real MIDI
  input and UI-injected notes (`injectTestNote`) both feed, drained in `processBlock`.
  Also owns `sequencerEngine` (see below) and drives it from `processBlock`.
- `plugin/Source/PluginEditor.*` - the whole UI. `D110Panel` = the photographed panel
  with invisible hit-regions + the offscreen-rendered dot-matrix LCD. `D110EditorPane` =
  the ten-tab extended editor drawer (`Tab` enum: Parts, Tone, Rhythm, Patches,
  Timbres, Tones, System, Monitor, Soundbanks, Utility), opens downward, independently foldable.
  `D110SequencerPanel` (see below) is a third such drawer, closed by default.
  `D110MemoryCard` = the memory card slot widget.
- `plugin/Source/D110Keyboard.h/.cpp` - the on-screen test keyboard drawer (mouse piano +
  tracker-style PC keyboard input, MIDI channel/omni via right-click), independently
  foldable in the plugin, open by default. Keys light up for two independent reasons: struck
  directly here (mouse/PC keyboard - instant, no polling) or `D110KeyboardHost::isNoteActive()`
  (any note reaching the app another way - external MIDI In, sequencer playback, a DAW host
  track - a small lock-free per-note array the host's audio/MIDI thread writes to, polled by
  this component's own 30Hz timer). `midiPanic()` clears that whole array, since its CC
  64/123 "all notes off" is a controller message, not literal note-offs the array would
  otherwise ever see. `plugin/keyboard_activity_probe.cpp` covers both write paths (direct
  injection and sequencer playback) against `NonetSeqHost`. Lives outside `PluginEditor.*`
  and talks to its owner only through `plugin/Source/D110KeyboardHost.h` (note injection + its
  own persisted config), so it's also embedded, unfoldable, in `Nonet-Seq` (see below) -
  `D110AudioProcessor`
  implements that interface for the plugin, `NonetSeqHost` for Nonet Sequencer.
- `plugin/Source/sequencer/` - a D-20-style multitrack MIDI sequencer, added 2026-08-05,
  since grown to also cover step recording, undo, bar-range delete/copy/transpose, a MIDI
  Out path, and a standalone-only build of its own - see `docs/sequencer.md` for the full
  feature list, this is just the code layout. `D110SequencerEngine` is the transport/data
  model - deliberately D-110-agnostic (own internal clock, note-only
  `juce::MidiMessageSequence` per track, MIDI-file, quantize and step-recording logic),
  talking to whatever embeds it only through a `channelForTrack` callback. `D110SequencerPanel`
  is the JUCE UI drawer, talking to its host only through `D110SequencerHost` (20 methods, plus the optional `auditionTrackNote()`) -
  `D110AudioProcessor` implements that interface for the plugin, `NonetSeqHost` implements
  it for `Nonet-Seq` (**Nonet Sequencer** - CMake target and binary both `Nonet-Seq`), the
  independent sequencer app, deliberately named apart from the D-110 - Standalone-only, no
  VST3, no firmware/ROMs/plugin wrapper, just the panel/engine plus the same `D110Keyboard`
  the plugin has (for direct test-play/MIDI-routing, no fold - always visible), direct
  system MIDI In/Out, and its own settings file. 9 tracks (D-110 Parts 1-8 by their
  live SYSTEM-area channel inside the plugin, or a fixed factory-default channel map in the
  independent app, plus a rhythm track fixed on channel 10) - Nonet Sequencer only
  (`supportsExtraTracks()`) can go up to `kMaxTracks` (16): right-click above the track rows
  for "Activate extra tracks", see `docs/sequencer.md`. The plugin's own engine instance
  never enables this, so it stays exactly 9 tracks/`kRhythmTrack` pinned at index 8
  regardless. Both hosts now implement `supportsProgramChange()`: any track can get a fixed
  Program Change/Bank (click its CH readout), sent once when PLAY/REC starts - over MIDI Out
  in Nonet Sequencer, over the firmware's own MIDI IN (plus MIDI Out) in the plugin. That
  override (Program Change/Bank/BankLsb/Volume/Pan, plain fields on
  `D110SequencerEngine::Track`) is per song slot, like the track's own notes/mute/solo/
  quantize/name - it used to be one workspace-wide value shared by all 4 songs, until Alan
  pointed out (2026-08-21) that doesn't make sense, a song's own instrumentation being part of
  what makes it that song. `newSong()` resets it (and tempo, to 120) for the slot it's called
  on only. Rhythm (`kRhythmTrack`, D-110 plugin only) has no Program Change equivalent - its
  sounds are per-key, not a single patch number - but does get Volume/Pan (`supportsProgram
  ChangeForTrack()` excludes it, `supportsTrackVolumePanForTrack()` doesn't; 2026-08-21, Alan's
  request), surfaced as "CC Change" instead of "Program Change" in both UIs. The plugin-only
  per-song **sound snapshot** (`supportsSoundSnapshots()`, right-click
  a song-slot button) is a separate, much bigger per-slot recall on top of that: each of the 4
  slots can store/recall the instrument's ENTIRE memory (every Patch/Timbre/Tone/System byte),
  applied with a power cycle. State persists in
  `getStateInformation`/`setStateInformation` the same way the firmware NVRAM does (plugin)
  or its own settings file (independent app), both via the shared `D110SequencerSongsFile.h/.cpp`.
  `D110SequencerGridPanel` is the third view of that same drawer (piano roll: one track/one bar,
  click to add/remove, drag to move/resize, velocity lane), exclusive with the retro one via
  `get/setSequencerGridMode()` on both hosts; its edits go through the engine's `addNote()`/
  `updateNoteEvent()`/`deleteNoteEvent()`, which take `editLock` (as do `renderInto()`, undo/redo
  and `pushUndoSnapshot()`) and queue a note-off for any note edited while it sounds.
  `plugin/sequencer_probe.cpp` and `plugin/sequencer_state_probe.cpp` are the sequencer's headless tests
  (engine timing/quantize/step-recording/undo/file-I/O, and the state-save round trip,
  respectively) - both native-core-only, no MAME dependency.
- `plugin/Source/SoundbankDatabase.h/.cpp` + `SoundbankBrowser.h/.cpp` - the SOUNDBANKS tab
  (desktop) / hamburger-menu view (Android), added 2026-08-28 - see `docs/soundbanks.md` for
  the full feature list. `Database` is the model: scans a folder/`.zip` of Roland SysEx dumps
  for DT1 messages addressed inside the internal Tone area (SysEx `08 00 00`, 256-byte
  records - **Tones**, the actual sounds, not Patches, which only arrange existing Tones across
  parts; an earlier Patch-based cut was corrected to Tones, and that decoder kept dormant
  rather than deleted, for reuse later), dedupes, and persists one small file per tone plus an
  `index.json` under a fixed app-data root - `load()` only ever reads that index (instant tab
  open regardless of library size), `rescan()` (explicit only) does the real folder walk.
  `Favorites` is a second, independent persisted list (own file, never touched by `rescan()`),
  exportable as standalone SysEx bank file(s) (`exportTonesAsSysex()`, split one file per 64
  tones). `SoundbankBrowser` is the shared JUCE UI (talks straight to `D110AudioProcessor&`, no
  Host interface, since both apps use that same class) - double-click/PART-row auditions a tone
  into a part's live Tone Temporary; right-click (desktop) / long-press (Android, via
  `juce::Timer::callAfterDelay` since touch has no second button) opens Favorites/Send-to-Part/
  Inject-to-slot/**Send to real D-110** (writes to an actual connected unit's Part or Tone
  Memory slot over the existing external MIDI Out device,
  `D110AudioProcessor::sendSoundbankToneToExternalMidi[Part]()` - separate from the emulator's
  own `injectSoundbankTone()`/`auditionToneBytes()`, which only ever reach `core.pushMidi()`).
  `plugin/soundbank_probe.cpp` is the headless test (scan/dedup/recursion/`.zip`/Favorites/
  export round-trips, no firmware/ROMs needed, same reasoning as the sequencer's own probes).
- `plugin/CMakeLists.txt` - two plugin targets (native always, MAME opt-in) plus a long
  list of headless test/probe executables (`plugin/*.cpp` at the top level, ~60 of
  them) used to measure firmware RAM layout and verify behaviour empirically rather
  than by reading Roland docs (there aren't any that go this deep). `d110_editor_shot` /
  `d110_native_editor_shot` render the whole editor UI to a PNG headlessly - the fast
  way to visually check UI changes without opening a DAW.
- `munt/` - vendored sound engine with a couple of local fixes for D-110 (LCD
  buffer/part-count, see `munt/mt32emu/src/Display.cpp`; single-assign/POLY-1-2 retrigger
  drop, see `munt/mt32emu/src/Part.cpp`'s `playPoly()` - `Synth::abortingPoly` is a single
  synth-wide flag, so it used to block a retrigger on partials that were already free, not
  just ones genuinely still busy fading - see `plugin/native_assign_mode_probe.cpp`).
- `docs/` - measured ground truth (panel pixel geometry, SysEx address map, factory
  defaults, memory card protocol). Code comments reference these; check here before
  guessing at firmware behaviour.
- `android/` - Standalone-only Android port (CMake target `D110Android`, no VST3), reusing the
  same panel/keyboard/sequencer sources unchanged. Functional on real hardware; shipped as a
  release APK (`./gradlew assembleRelease`, debug-keystore-signed - no dedicated release
  keystore exists) since v0.9.5-fixes16 (2026-08-28, `D-110_android_debug.apk`; renamed to
  `D-110_android.apk` from fixes17 on, `Di-111_android.apk` since the 2026-10-01 rename). `docs/android.md` is the short user-facing guide; `.claude/dev-notes/
  android.md` has the full development history and JUCE-on-Android file-I/O gotchas worth
  reading before touching that code again (`juce::URL::isLocalFile()` lies on Android for
  content:// results, a long-press timer can race a `juce::FileChooser` there in a way desktop
  never hits, and `juce::AndroidDocumentIterator` doesn't actually work for `content://` trees
  in this JUCE checkout despite its own doc comment).
- `.github/workflows/build-{windows,macos}.yml` - CI, native-core only. Deliberately
  **not** wired to `main`/`linux-port` push - triggered by `workflow_dispatch` or by
  pushing to the `ci/windows-macos-builds` branch specifically (fast-forward `main`
  onto it to run CI: `git push origin main:ci/windows-macos-builds`).

## Conventions

- **Code comments: English.** The project historically carried Russian comments (see the
  header of `plugin/CMakeLists.txt`); the rule is now to translate them to English as they
  are encountered. `plugin/Source/*` was fully translated on 2026-09-18 (PluginEditor.*,
  PluginProcessor.*, D110Core.*). Still Russian: the probe/test programs in `plugin/*.cpp`,
  `plugin/CMakeLists.txt` and `patches/README.md` - translate when you touch them.
- `processBlock()` must not allocate: its MIDI merge/remap buffers, the sequencer
  click list and the three drained `pending*` queues are member scratch storage
  pre-sized in `prepareToPlay()` (2026-09-18). Add new per-block temporaries the
  same way, not as locals.
- Everything in the extended editor writes to the **firmware** via real Roland SysEx
  messages (DT1), never directly to the sound engine - the mirror to the sound engine
  is a separate, one-directional path. If you add a new editor field, follow this
  pattern: find/verify the firmware RAM address empirically (a `*_probe.cpp` tool), add
  a `Cell`, send a real SysEx write.
- Verification bias in this codebase: claims about firmware behaviour are backed by a
  purpose-built probe (`plugin/*_probe.cpp`, `*_test.cpp`) that measures RAM/output
  directly, not by inference. Follow that pattern rather than guessing when touching
  firmware-adjacent code. `plugin/audio_test.cpp`'s pass/fail is explicitly documented
  as unreliable (timing races) - don't trust it as a sole signal.
- Linux packaging: `.deb` built locally (not by CI), staged at repo root (gitignored),
  named `d110-emulator_<version>-fixesNN_amd64.deb`, `Depends` computed via
  `dpkg-shlibdeps`, built with `fakeroot dpkg-deb` for correct ownership. Maintainer
  field is `luginf`, not an email address.

## Build

Default (native only, no MAME needed):
```
cd plugin && cmake -B build -S . && cmake --build build --config Release
```
Full requirements, MAME-backend opt-in build, and ROM setup: see `README.md`.

## Nonet Sequencer extracted to its own repository (2026-09-22)

Alan asked to separate the independent Nonet Sequencer app from this repo, keeping this
repo's own in-plugin sequencer integration (D110Sequencer*/D110Keyboard*) untouched. Done:

- New repo, own history (not a subtree split - a plain copy plus one commit, Alan's choice):
  [github.com/luginf/nonet-sequencer](https://github.com/luginf/nonet-sequencer), local clone
  `~/src/nonet-sequencer`. Not pushed yet as of this note - only committed locally, Alan wants
  to look at it first.
- Files copied and renamed to drop the `D110*` prefix (Alan's choice - nothing in that repo
  depends on this project anymore): `D110SequencerEngine/Panel/RetroPanel/GridPanel/Host/
  SongsFile` -> `Sequencer*`, `D110Keyboard(Host)` -> `Keyboard(Host)`, namespace `d110seq` ->
  `nonetseq`. `SequencerViewMenu.h` and `UiTheme`/`DotMatrixFont` were already neutral, kept
  as-is. Verified: builds clean (CMake FetchContent, JUCE 8.0.15, reused this repo's own
  `plugin/build/_deps/juce-src` as `FETCHCONTENT_SOURCE_DIR_JUCE` to skip the clone), and runs
  in an isolated Xvfb (window shows, right-click "Sequencer >" menu works, screenshot taken).
- **This repo**: removed `plugin/Source/sequencer/NonetSeqMain.cpp`, `NonetSeqHost.h/.cpp`,
  `plugin/keyboard_activity_probe.cpp`, and the `Nonet-Seq`/`d110_keyboard_activity_probe`
  CMake targets (the probe depended on `NonetSeqHost.cpp`, so it went with it - no equivalent
  re-created here, the file is small enough that a similar probe could live in the new repo if
  ever needed). `docs/project_layout.md` and `docs/sequencer.md` updated to point at the new
  repo. Rebuilt `D110EmulatorNative_Standalone` after the `CMakeLists.txt` edit to confirm no
  regression - succeeds.
- **Not yet done** (pending Alan's go-ahead): `git push` the new repo, and committing this
  repo's own working-tree changes (the file removals above, plus the still-uncommitted
  retro-sequencer/unified-menu work from the previous session).
