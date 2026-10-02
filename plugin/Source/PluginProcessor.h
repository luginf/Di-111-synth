#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
// MidiInput/MidiOutput and MidiDeviceInfo live here, not in juce_audio_processors: the
// panel opens a port itself so an external editor can reach the module the way it would
// reach the hardware, beside whatever the host routes in.
#include <juce_audio_devices/juce_audio_devices.h>
#include <mt32emu/mt32emu.h>
// D110_NATIVE_CORE (off by default) builds this processor against the native, MAME-free
// CPU port (Source/native/D110CoreNative.h) instead of the MAME-backed D110Core - see the
// "Native D-110 CPU core port" plan. D110CoreType is a plain type alias, not a new
// abstraction: every existing D110Core:: reference in this file becomes D110CoreType:: and
// is byte-for-byte the same code when the flag is off, since the alias just IS D110Core then.
#ifdef D110_NATIVE_CORE
#include "native/D110CoreNative.h"
using D110CoreType = D110CoreNative;
#else
#include "D110Core.h"
using D110CoreType = D110Core;
#endif
#include "D110KeyboardHost.h"
#include "AppDataPath.h"
#include "SoundbankDatabase.h"
#include "sequencer/D110SequencerEngine.h"
#include "sequencer/D110SequencerHost.h"
#ifdef D110_HAVE_JACK_MIDI
#include "JackMidiInput.h"
#endif
#include <array>
#include <map>
#include <memory>
#include <thread>

class D110AudioProcessor : public juce::AudioProcessor,
                           public D110SequencerHost,
                           public D110KeyboardHost,
                           private juce::MidiInputCallback,
                           private juce::Timer {
public:
	// The real D-110 has exactly 32 LA32 partial-generator channels shared across all 9
	// parts, and this project's sound engine (a vendored mt32emu fork) models that faithfully
	// by default (MT32Emu::DEFAULT_MAX_PARTIALS). Measured (plugin/multi_part_polyphony_probe.cpp,
	// plugin/partial_count_cpu_probe.cpp): fast, overlapping playing on just two parts at once
	// (e.g. bass + solo) genuinely exhausts that budget - mt32emu's own Part::playPoly()
	// silently drops the note (confirmed: "needed=4, free=0" at the moment of refusal) - this
	// is authentic Roland LA-architecture behaviour, not a bug in this project's own firmware/
	// LA32 work. Unlike the firmware's own 32-hardware-voice-slot table (real ROM logic, not
	// touched), mt32emu's partial count is a plain runtime parameter nothing in Part.cpp/
	// PartialManager.cpp hardcodes to 32 - so, deliberately, this native plugin's own sound
	// engine is opened with more: 128 measured comfortably below where the CPU cost becomes
	// noticeable (~10-15% of one audio thread even under an all-8-parts-at-once stress test,
	// plugin/partial_count_cpu_probe.cpp), and enough headroom that the two-part fast-playing
	// scenario that started this investigation no longer drops notes. This is a deliberate
	// choice to exceed the real hardware's polyphony ceiling now that this is our own engine,
	// not an attempt to hide the difference - see kExtendedPolyphonyLabel below.
	static constexpr MT32Emu::Bit32u kExtendedPartialCount = 128;
	static constexpr const char *kExtendedPolyphonyLabel = "128-VOICE POLYPHONY";

	D110AudioProcessor();
	~D110AudioProcessor() override;

	// --- MIDI ports chosen on the panel, independently of the host -------------
	// A hardware module is driven from whatever is plugged into its MIDI IN, and an editor
	// like MIDI Quest expects to reach it over a port rather than through the DAW's plugin
	// routing. Opening a port directly is what makes that possible; the host's own MIDI
	// keeps working alongside it, so this adds a route rather than replacing one.
	static juce::Array<juce::MidiDeviceInfo> midiInputs() {
		return juce::MidiInput::getAvailableDevices();
	}
	static juce::Array<juce::MidiDeviceInfo> midiOutputs() {
		return juce::MidiOutput::getAvailableDevices();
	}
	void setMidiInputDevice(const juce::String &identifier);
	void setMidiOutputDevice(const juce::String &identifier);
	juce::String getMidiInputId() const { return selInputId; }
	juce::String getMidiOutputId() const { return selOutputId; }

	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;
	void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) override;

	// One stereo MIX bus (always on, same as before this existed) plus six mono INDIVIDUAL
	// buses matching the real rear panel's MULTI OUT jacks - off by default, since most
	// hosts and most users only ever want the mix. A part's Output Assign value (System
	// Reverb byte's old neighbour, repurposed - see munt/mt32emu/src/Synth.cpp) chooses
	// which of the seven it feeds.
	static BusesProperties createBuses();
	bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

	juce::AudioProcessorEditor *createEditor() override;
	bool hasEditor() const override { return true; }

	const juce::String getName() const override { return "Di-111"; }
	bool acceptsMidi() const override { return true; }
	bool producesMidi() const override { return false; }
	bool isMidiEffect() const override { return false; }
	double getTailLengthSeconds() const override { return 3.0; }

	int getNumPrograms() override { return 1; }
	int getCurrentProgram() override { return 0; }
	void setCurrentProgram(int) override {}
	const juce::String getProgramName(int) override { return {}; }
	void changeProgramName(int, const juce::String &) override {}

	void getStateInformation(juce::MemoryBlock &destData) override;
	void setStateInformation(const void *data, int sizeInBytes) override;

	// Remembers the chosen path and (re)opens the synth once BOTH paths are known.
	// Safe to call with just one path set - it will not error until the other is also provided.
	void setControlRomPath(const juce::String &path);
	void setPcmRomPath(const juce::String &path);

	bool isSynthReady() const { return synth != nullptr; }

	// Re-attempts automatic ROM discovery from wherever getAutoRomFolder() now points (after
	// e.g. the ROM setup dialog or the Utility tab's "ROM FOLDER" just changed the custom
	// override) and, if that finds both ROMs, powers the machine on right away - so pointing
	// at a folder takes effect immediately instead of requiring a manual power cycle.
	void reloadRomsAndPowerOn() {
		tryAutoLoadRoms();
		if (isSynthReady()) setPoweredOn(true);
	}

	juce::String getControlRomPath() const { return controlRomPath; }
	juce::String getPcmRomPath() const { return pcmRomPath; }
	juce::String getControlRomDescription() const { return controlRomDescription; }
	juce::String getPcmRomDescription() const { return pcmRomDescription; }
	juce::String getLastError() const { return lastError; }
	static juce::File getAutoRomFolder();

	// A user-chosen ROM folder override (Utility tab, "ROM FOLDER"), checked before any of
	// getAutoRomFolder()'s own automatic locations - Alan's request 2026-08-21, for a ROM
	// location none of the automatic ones happen to cover. Deliberately a plain machine-wide
	// settings file (getCustomRomPathFile()), NOT part of getStateInformation/project state:
	// where ROMs sit on disk is a fact about this machine/install, not about a specific DAW
	// project, and a project saved with one baked in would misbehave the moment it's opened
	// somewhere else. Empty string = no override, same meaning as never having set one.
	static juce::String getCustomRomFolder();
	static void setCustomRomFolder(const juce::String &path);

	// Custom PCM wave samples (desktop only - see D110EditorPane's Tone tab, PCM field's
	// right-click menu). Overwrites one of the real, checksum-validated PCM ROM's 256 wave
	// slots' audio content in memory - see MT32Emu::Synth::setPCMWaveSamples()'s own comment
	// for why this can't just be a rebuilt ROM file. Reads any audio format JUCE recognises,
	// resamples/fits it to the target wave's own declared length, applies immediately (no
	// power cycle needed), and persists across getStateInformation/setStateInformation so it
	// survives a reload. Returns false on read/decode failure or an out-of-range waveIndex.
	bool loadCustomPcmWave(int waveIndex, const juce::File &audioFile);
	// Reverts one wave slot back to the real ROM's own original content. False if that slot
	// wasn't customized in the first place.
	bool restoreFactoryPcmWave(int waveIndex);
	bool hasCustomPcmWave(int waveIndex) const { return customPcmWaves.count(waveIndex) != 0; }
	// Temporarily overrides whether a customized wave loops - only meaningful while the slot is
	// customized (restoreFactoryPcmWave() brings back the factory wave's own loop setting along
	// with everything else). The real LA32 PCM engine only supports "loop the whole stored
	// sample from the start" or "play once" - no loop start point, no ping-pong, a genuine
	// hardware limitation carried into the emulation, not a missing feature of this override.
	// Only meaningful while waveIndex is customized (see hasCustomPcmWave()). Persisted.
	bool setCustomPcmWaveLoop(int waveIndex, bool loop);
	// What loadCustomPcmWave() set it to initially (the factory wave's own loop setting,
	// unchanged unless setCustomPcmWaveLoop() was called since) - undefined/meaningless if
	// !hasCustomPcmWave(waveIndex).
	bool getCustomPcmWaveLoop(int waveIndex) const {
		const auto it = customPcmWaveLoops.find(waveIndex);
		return it != customPcmWaveLoops.end() && it->second;
	}
	// The source file's own name (no extension) for a customized wave, for the PCM field to
	// show instead of the factory wave name - see D110EditorPane::textOf()'s own PCM case.
	// Empty if that slot isn't customized.
	juce::String getCustomPcmWaveName(int waveIndex) const {
		const auto it = customPcmWaveNames.find(waveIndex);
		return it == customPcmWaveNames.end() ? juce::String() : it->second;
	}
	// Directly sets a wave's pitch calibration (MT32Emu::Synth::setPCMWavePitchOffset()'s own
	// comment explains the units/meaning) - exposed so pcm_pitch_calibration_probe.cpp can
	// measure the constant loadCustomPcmWave() itself now applies automatically
	// (kNeutralPcmPitchOffset, PluginProcessor.cpp). Not meant for the UI - a custom sample's
	// calibration isn't a user-facing knob, it's fixed to whatever makes playback match the
	// recording's own pitch.
	bool setCustomPcmWavePitchOffset(int waveIndex, int pitchOffset);

	// Where custom PCM samples are picked from by name instead of browsing a file dialog every
	// time (D110EditorPane's "Sample Library" submenu) - a machine-wide setting, same reasoning
	// and storage pattern as getCustomRomFolder() above. Empty = not configured yet.
	static juce::String getCustomSampleFolder();
	static void setCustomSampleFolder(const juce::String &path);

	// The Soundbanks tab/browser's own patch database - see SoundbankDatabase.h. One instance
	// per processor, shared between the desktop tab and (Android) the swapped-in browser
	// panel, so it's never loaded twice. load() is cheap (see its own comment) and safe to
	// call every time the browser opens; rescan() is not and only ever runs from an explicit
	// button press.
	d110bank::Database &getSoundbankDatabase() { return soundbankDb; }

	// Favourited tones - see SoundbankDatabase.h's own Favorites class comment. Separate from
	// soundbankDb above on purpose (Alan's request, 2026-08-28): its own file, untouched by a
	// database rescan.
	d110bank::Favorites &getSoundbankFavorites() { return soundbankFavorites; }

	// Where the user's own SysEx patch-library folder is - the SOURCE Soundbanks rescan()
	// reads from, as opposed to the database itself (SoundbankDatabase::defaultRoot(), fixed,
	// not user-configurable). Same plain-text-file, machine-wide storage pattern as
	// getCustomRomFolder()/getCustomSampleFolder() above - this is a fact about this
	// machine/install, not project state.
	static juce::String getSoundbankSourceFolder();
	static void setSoundbankSourceFolder(const juce::String &path);

	// ON HOLD, 2026-08-28 - see SoundbankDatabase.h's own comment: the Soundbanks feature
	// scanned Patches first, Alan corrected it to Tones, and asked to keep the Patch-side code
	// (this included) for later rather than delete it. Not called anywhere right now.
	// Writes one already-decoded 128-byte Patch record into a specific Bank I slot (0-63) via
	// a real SysEx DT1 write. Wraps sendAreaData() with the Patch area's own address/stride
	// (D110CoreType::kSysexPatches/kPatchRecord) so callers don't need to know them.
	void injectSoundbankPatch(int slot, const juce::uint8 *data128);

	// Writes one already-decoded 256-byte Tone Memory record (10-byte name at offset 0, then
	// the 246-byte tone body - D110Core.h's kToneMemRecord/kToneRecord) into a specific
	// internal Tone slot (0-63, Roland's Tone Group "i") - what the Soundbanks browser's
	// "Inject to slot..." does. Name and body are two separate DT1 writes (sendName() plus a
	// chunked body send, mirroring the existing storeToneFromPart()/sendToneBlock() pattern -
	// PluginProcessor.cpp - since one DT1 message can't carry all 246 body bytes at once).
	void injectSoundbankTone(int slot, const juce::uint8 *data256);

	// Writes a 246-byte tone BODY (no name - Tone Temporary has none of its own, see
	// D110Core.h's kToneRecord vs kToneMemRecord) straight into a part's live/working tone,
	// immediately audible - what the Soundbanks browser's double-click "quick audition" does.
	// Same mechanism as auditionTone(part, slot) elsewhere in this class, except the bytes come straight from
	// the caller instead of an existing Tone Memory slot - lets a browsed-but-not-yet-injected
	// tone be heard without first spending one of the 64 internal slots on it.
	void auditionToneBytes(int part, const juce::uint8 *body246);

	// Whether a real external MIDI Out device is currently open (setMidiOutputDevice) - lets
	// the Soundbanks browser's "Send to real D-110..." context-menu item give useful feedback
	// ("pick a MIDI Out device first") instead of silently doing nothing.
	bool hasExternalMidiOutput() const {
		const juce::ScopedLock lock(osMidiLock);
		return osMidiOut != nullptr;
	}

	// Same 256-byte Tone record, same slot-addressed DT1 writes as injectSoundbankTone() above,
	// but sent straight out the external MIDI Out device (setMidiOutputDevice) instead of into
	// this emulator's own firmware (core.pushMidi) - Alan's request, 2026-08-28: right-click a
	// Soundbanks tone and write it into a slot on a REAL connected D-110, for building/testing
	// a library against actual hardware rather than only this emulator. sendAreaData() can't be
	// reused here since it always targets the firmware (core.pushMidi); this is the same
	// buildDt1Message()-based chunking, routed to osMidiOut instead. No-op (false) if no MIDI
	// Out device is open - callers should check hasExternalMidiOutput() first for a clean UI
	// message rather than a silent failure. NOTE: like every DT1 write this emulator sends, the
	// device ID is fixed at the Roland factory default (17/0x10) - if the real unit's own
	// Exclusive Unit# was changed from that, it won't respond; there's no way for this emulator
	// to know a real remote unit's setting to match it automatically.
	bool sendSoundbankToneToExternalMidi(int slot, const juce::uint8 *data256);

	// Same idea as sendSoundbankToneToExternalMidi() above, but the external-hardware
	// equivalent of auditionToneBytes(): writes straight into a Part's TONE TEMPORARY on the
	// real connected unit (kSysexToneTemp, not kSysexTones - different area, different address
	// stride) rather than a stored Tone Memory slot - instant, non-destructive audition on
	// actual hardware, no slot spent, "comme le font la plupart des éditeurs" (Alan's own
	// words, 2026-08-28 follow-up ask). `body246` is the tone record's own first 246 bytes,
	// name included - same convention auditionToneBytes() already uses (see its own comment).
	bool sendSoundbankToneToExternalMidiPart(int part, const juce::uint8 *body246);

	// Same idea as sendSoundbankToneToExternalMidi() above, but for a full 128-byte Patch
	// record (kSysexPatches/kPatchRecord) rather than a Tone - Alan's request, 2026-08-29:
	// right-click a row in the Patches tab's ALL PATCHES list and write it into the same-
	// numbered Patch Memory slot on a real connected D-110, so a patch edited/auditioned here
	// can be checked against actual hardware. `slot` is which of the 64 Patch Memory records
	// to write (matches the row's own patch number - the emulator and the real unit end up
	// with identical content in that slot), `data128` its current 128 bytes read from this
	// emulator's own RAM (D110CoreType::kRamPatches + slot*kPatchRecord). No chunking needed:
	// 128 bytes fits in a single DT1 message (kMaxSysexBytes leaves ~246 bytes of payload
	// room), unlike the 256-byte Tone case above.
	bool sendPatchToExternalMidi(int slot, const juce::uint8 *data128);

	// Snapshot of everything the virtual front panel needs to redraw itself each frame.
	// Built entirely from stable, always-current getters (not mt32emu's own getDisplayState() text,
	// which is designed to flash transient messages and revert after a couple of seconds, and whose
	// internal buffer turned out to leave stale bytes behind when a shorter message follows a longer
	// one - see the "info overlapping" bug this replaced).
	//
	// `text` holds raw character codes rather than a juce::String because the panel renders a real
	// 16x2 dot matrix and needs codes, not glyph-agnostic text: code 0x01 is the CGRAM full-block
	// the hardware substitutes for a Part number while that Part is sounding, which no string can
	// carry. Layout copies the real Patch Play screen photographed in docs/lcd_reference.png:
	//
	//     12345678R RomPly
	//     1:Macho Memory
	struct LcdSnapshot {
		static constexpr int kCols = 16;
		static constexpr int kLines = 2;
		static constexpr juce::uint8 kActivePartBlock = 0x01; // matches Display.cpp's ACTIVE_PART_INDICATOR

		bool midiLedOn = false;
		juce::uint8 text[kLines][kCols] = {};

		bool operator==(const LcdSnapshot &other) const {
			if (midiLedOn != other.midiLedOn) return false;
			for (int line = 0; line < kLines; ++line)
				for (int col = 0; col < kCols; ++col)
					if (text[line][col] != other.text[line][col]) return false;
			return true;
		}
		bool operator!=(const LcdSnapshot &other) const { return !(*this == other); }
	};
	LcdSnapshot getLcdSnapshot() const;

	// Mirrors pressing "Master Volume" on real hardware to return the LCD to its default view.
	void resetDisplayToMainMode();

	// The on-screen test keyboard's one path in: exactly the collector a real MIDI IN port
	// feeds (osMidiCollector, see handleIncomingMidiMessage(MidiInput*, ...) and
	// processBlock()), so a clicked key reaches the firmware, the panel and the sound engine
	// by the identical route a real keyboard would - nothing here talks to the synth directly.
	// Safe to call from the message thread; the collector is its own lock.
	void injectTestNote(int channel, int note, float velocity, bool on) override;
	int getGridRowHeight() const override { return gridRowHeight; }
	void setGridRowHeight(int pixels) override { gridRowHeight = pixels; }
	void auditionTrackNote(int track, int note, int velocity, bool on) override {
		injectTestNote(sequencerEngine.channelForTrack(track), note, static_cast<float>(velocity) / 127.0f, on);
	}

	// Same queue, same feed point, but for an arbitrary already-channelised MIDI message
	// (program change, CC, pitch bend - not just notes) - what a MIDI file player needs that
	// injectTestNote()'s note-only signature above doesn't cover. Used by the Android app's
	// standalone MIDI file playback, which has no host/DAW to route file events through.
	void injectMidiMessage(const juce::MidiMessage &message);

	// D110Keyboard's own config (MIDI channel/remap, PC-keyboard tracker input, QWERTY/AZERTY
	// layout) - stored here, not as plain members on the UI component itself, so it survives
	// getStateInformation/setStateInformation regardless of whether an editor happens to be
	// open at save/restore time (a host can create/destroy the editor independently of the
	// processor's own lifetime). D110Keyboard reads these once at construction and writes
	// back through these setters on every change - see its own constructor/showContextMenu().
	int getKeyboardMidiChannel() const override { return keyboardMidiChannel; }
	void setKeyboardMidiChannel(int channel) override { keyboardMidiChannel = juce::jlimit(1, 16, channel); }
	bool getMidiRemap() const override { return midiRemap; }
	void setMidiRemap(bool remap) override { midiRemap = remap; }
	bool getKeyboardPcInputEnabled() const override { return keyboardPcInput; }
	void setKeyboardPcInputEnabled(bool enabled) override { keyboardPcInput = enabled; }
	// 0 = QWERTY, 1 = AZERTY - a plain int rather than D110Keyboard's own private PcLayout
	// enum, so this header doesn't need to know that type exists.
	int getKeyboardPcLayout() const override { return keyboardPcLayout; }
	void setKeyboardPcLayout(int layout) override { keyboardPcLayout = juce::jlimit(0, 1, layout); }
	int getKeyboardNumOctaves() const override { return keyboardNumOctaves; }
	void setKeyboardNumOctaves(int numOctaves) override { keyboardNumOctaves = juce::jlimit(1, 4, numOctaves); }
	// See D110KeyboardHost.h and handleIncomingMidiMessage(const juce::MidiMessage &)'s own
	// comment for where remoteNoteActive actually gets written.
	bool isNoteActive(int note) const override {
		return note >= 0 && note < 128 && remoteNoteActive[static_cast<size_t>(note)].load();
	}

	// The custom-drawn part of the interface's light/dark theme (Utility tab -> THEME) -
	// the photographed panel and its LED-style indicators sit outside this, since those
	// colours are the hardware's own, not chrome. Stored here rather than as a plain
	// d110ui::Theme global the editor writes on its own, so it survives project save/load
	// the same way the keyboard config above does.
	bool getUiThemeLight() const { return uiThemeLight; }
	void setUiThemeLight(bool light) { uiThemeLight = light; }

	// "System" (follow the OS dark-mode setting) on top of the plain light/dark choice above -
	// Android's Options menu only, 2026-08-28. Kept as a separate bool rather than folding into
	// uiThemeLight (a tri-state) so old saved state without this attribute still loads exactly
	// as before (uiThemeLight alone, follow defaulting off) - see setStateInformation().
	bool getUiThemeFollowSystem() const { return uiThemeFollowSystem; }
	void setUiThemeFollowSystem(bool follow) { uiThemeFollowSystem = follow; }

	// Normal/Big text and control size (Utility tab -> FONT SIZE) - Alan's request,
	// 2026-09-03, for HDPI screens with no OS-level scaling. Just the persisted preference,
	// same status as uiThemeLight above; actually applying it (d110ui::setFontScale() plus
	// juce::Desktop::getInstance().setGlobalScaleFactor(), Standalone-only - see UiTheme.h's
	// own comment on why) is D110AudioProcessorEditor's job, done once at construction and
	// again on every toggle.
	bool getUiFontScaleBig() const { return uiFontScaleBig; }
	void setUiFontScaleBig(bool big) { uiFontScaleBig = big; }

	// Whether the sequencer drawer shows the classic graphical panel or the D-20-style
	// LCD+9-button retro view - see D110SequencerRetroPanel.h. Toggled from D110Panel's
	// Options menu, persisted the same way uiThemeLight is.
	bool getSequencerRetroMode() const { return sequencerRetroMode; }
	void setSequencerRetroMode(bool retro) {
		sequencerRetroMode = retro;
		if (retro) sequencerGridMode = false; // the three sequencer views are mutually exclusive
	}
	// The piano-roll/grid editor view (D110SequencerGridPanel) - the third view alongside
	// normal and retro, exclusive with retro (see setSequencerRetroMode()).
	bool getSequencerGridMode() const { return sequencerGridMode; }
	void setSequencerGridMode(bool grid) {
		sequencerGridMode = grid;
		if (grid) sequencerRetroMode = false;
	}

	// See D110SequencerHost::getRetroKeyBindings()'s own comment - just storage, the panel
	// owns the encode/decode. Persisted the same way uiThemeLight is.
	juce::String getRetroKeyBindings() const override { return retroKeyBindings; }
	void setRetroKeyBindings(const juce::String &encoded) override { retroKeyBindings = encoded; }

	// See D110SequencerHost::getRetroLcdCompactMode() - just storage. Persisted the same way
	// uiThemeLight is.
	bool getRetroLcdCompactMode() const override { return retroLcdCompactMode; }
	void setRetroLcdCompactMode(bool compact) override { retroLcdCompactMode = compact; }

	// Narrower front panel with the MEMORY CARD slot spliced out (Utility tab, "PANEL SIZE") -
	// Alan's request, 2026-08-20: the card is a real, rarely-used feature (Roland cards don't
	// exist for a plugin in practice), so hiding it buys back desktop space. See
	// D110Panel::currentRefW()/kCompactRefW for the geometry this drives.
	bool getCompactPanelMode() const { return compactPanelMode; }
	void setCompactPanelMode(bool compact) { compactPanelMode = compact; }

	// One shared "last used folder" for every file dialog in the app (SysEx bank import/
	// export, memory snapshot save/load, the sequencer's own .mid/.midiseq dialogs) - set
	// after each successful pick, offered as the starting point for the next one, so
	// browsing to a folder once doesn't mean re-navigating there for every subsequent
	// dialog. Persisted the same way the theme above is.
	juce::File getLastDialogDir() const override { return lastDialogDir; }
	void setLastDialogDir(const juce::File &dir) override { lastDialogDir = dir; }

	// Utility tab -> DEBUG. Gates the note-dropout diagnostic instrumentation in
	// processBlock() - off by default so nothing writes to disk on the audio thread unless a
	// user actually turns it on to help chase a real issue. See that instrumentation's own
	// comment for what it measures.
	bool getDebugModeEnabled() const { return debugModeEnabled; }
	void setDebugModeEnabled(bool enabled) { debugModeEnabled = enabled; }

	// The extended editor drawer's own height, in D110AudioProcessorEditor::kPaneRefH's
	// reference units - user-adjustable by dragging the keyboard drawer's handle band (the
	// boundary directly below it). Stored here for the same reason as the theme above: it
	// needs to survive project save/load regardless of the editor's own lifetime. Clamped by
	// the editor itself (kMinPaneRefH/kMaxPaneRefH) whenever it reads this back; stored
	// unclamped here in case those bounds are ever loosened later.
	float getEditorPaneRefH() const { return editorPaneRefH; }
	void setEditorPaneRefH(float refH) { editorPaneRefH = refH; }

	// Same idea, for the test keyboard and D-20-style sequencer drawers - both used to be a
	// fixed D110Keyboard::kRefH/D110SequencerPanel::kRefH, not user-adjustable at all, until
	// Alan asked (2026-08-19) for the sequencer drawer in particular to be resizable ("pas
	// très haute"). Clamped by the editor (kMin/MaxKeyboardPaneRefH, kMin/MaxSequencerPaneRefH)
	// whenever it reads these back, same reasoning as editorPaneRefH above.
	float getKeyboardPaneRefH() const { return keyboardPaneRefH; }
	void setKeyboardPaneRefH(float refH) { keyboardPaneRefH = refH; }
	float getSequencerPaneRefH() const { return sequencerPaneRefH; }
	void setSequencerPaneRefH(float refH) { sequencerPaneRefH = refH; }

	// The D-20-style sequencer drawer's one way in: it owns its transport, its 9 tracks
	// and its own MIDI-file/state (de)serialisation, and reads/writes it directly - see
	// Source/sequencer/D110SequencerEngine.h. processBlock() is the only other reader/
	// writer, on the audio thread; the UI's own access is safe as long as it stays to the
	// plain getters/setters (no long-held references across a block boundary).
	d110seq::D110SequencerEngine &getSequencer() override { return sequencerEngine; }

	// Per-Part Program Change/Bank override, sent once over the firmware's own MIDI IN (and
	// the direct MIDI Out port, same as any other sequencer note) when PLAY/REC starts - see
	// D110SequencerHost.h and processBlock()'s own sequencer block. Rhythm excluded: Program
	// Change picks one of the 128 stored Timbres for a melodic Part, and Rhythm has no
	// equivalent single-number selection - see supportsProgramChangeForTrack().
	bool supportsProgramChange() const override { return true; }
	bool supportsProgramChangeForTrack(int track) const override {
		return track >= 0 && track < d110seq::D110SequencerEngine::kRhythmTrack;
	}
	int getTrackProgram(int track) const override;
	void setTrackProgram(int track, int program) override;
	int getTrackBank(int track) const override;
	void setTrackBank(int track, int bank) override;
	// -1 = no hint. See sequencerLivePrograms's own comment for what this actually reads.
	int getTrackProgramHint(int track) const override;
	// Same idea as getTrackProgramHint(), backed by sequencerLiveVolumes/sequencerLivePans -
	// see their own comment.
	int getTrackVolumeHint(int track) const override;
	int getTrackPanHint(int track) const override;

	// Manual "resend now" escape hatch - see D110SequencerHost.h's own comment. Just flags a
	// request; processBlock() does the actual send (same code as the PLAY/REC edge), since only
	// the audio thread may touch the firmware/sound-engine bridge.
	void resyncProgramChanges() override { sequencerResyncRequested.store(true); }

	// Part Volume/Pan alongside the Program Change above - see D110SequencerHost.h's own
	// comment on why these are D-110-only and sent as SysEx (Part LEVEL/PAN), not MIDI CC.
	bool supportsTrackVolumePan() const override { return true; }
	// Unlike Program Change, Volume/Pan DOES cover the Rhythm track (2026-08-21, Alan's
	// request: a fixed default Volume for it) - Rhythm's own TimbreTemp record has a real
	// LEVEL/PAN, same as any melodic Part's (see D110EditorPane::layoutParts()'s own row for
	// it). supportsProgramChangeForTrack() stays melodic-only; this is the separate gate
	// getTrackVolume()/setTrackVolume()/getTrackPan()/setTrackPan() and their hints actually
	// check.
	bool supportsTrackVolumePanForTrack(int track) const override {
		return track >= 0 && track < d110seq::D110SequencerEngine::kNumTracks;
	}
	int getTrackVolume(int track) const override;
	void setTrackVolume(int track, int volume) override;
	int getTrackPan(int track) const override;
	void setTrackPan(int track, int pan) override;

	// Pull direction - see D110SequencerHost.h's own comment. Plain data copy
	// (sequencerLiveXxx -> sequencerTrackXxx), no firmware I/O involved, so unlike
	// resyncProgramChanges() this is safe to just do immediately wherever it's called from.
	bool supportsCaptureLivePatch() const override { return true; }
	void captureLivePatchIntoTracks() override;

	// Per-song sound snapshot - see D110SequencerHost.h's own comment on why this exists
	// alongside (not instead of) the per-track Program Change override above. Captures/
	// restores the instrument's ENTIRE memory (every Patch/Timbre/Tone/System byte), the
	// same image getStateInformation/exportMemorySnapshot() already know how to carry -
	// storeSoundSnapshotForSlot() just calls core.getRam() the same way
	// exportMemorySnapshot() does, and loadSoundSnapshotForSlot() replays
	// importMemorySnapshot()'s own power-cycle-and-replace-NVRAM approach (near-instant,
	// felt as a brief reboot - not a MIDI-speed SysEx transfer) directly from the stored
	// bytes instead of a file.
	bool supportsSoundSnapshots() const override { return true; }
	bool hasSoundSnapshot(int slot) const override;
	void storeSoundSnapshotForSlot(int slot) override;
	void loadSoundSnapshotForSlot(int slot) override;

	// The plugin opens powered OFF, and clicking POWER boots the real Roland firmware live,
	// in real time, exactly as the hardware does - the D110Core machine is started here and
	// the front panel then shows its own LCD coming up. Switching off stops the machine.
	bool isPoweredOn() const { return poweredOn; }
	void setPoweredOn(bool shouldBePoweredOn);
	void togglePower() { setPoweredOn(!poweredOn.load()); }

	// The emulated D-110 control board: the firmware, its menus, its MSM6222B display and
	// its 16 panel buttons. Supplies everything mt32emu has no notion of.
	D110CoreType &getCore() { return core; }

	// Whatever the sound engine's own mirror currently has loaded for a part's patch - the
	// same call the fallback LCD snapshot uses (see buildLcdSnapshot()'s "row 2" comment).
	// nullptr if no synth is open. Used for enumerating the factory patch list (send a
	// Program Change, let the firmware/mirror catch up, read the name back) rather than
	// hardcoding it from a manual - same "measured, not copied" bias as the rest of docs/.
	const char *getEnginePatchName(int part) const { return synth ? synth->getPatchName(static_cast<MT32Emu::Bit8u>(part)) : nullptr; }

	// Reads the LA engine's own copy of a parameter area back out. `packedAddress` is a
	// Roland address in mt32emu's packed form (three 7-bit bytes squeezed together, so
	// 0x040000 on the wire is 0x010000 here), NOT the three separate bytes emitRegionSysex
	// transmits. Returns false when no synth is open.
	//
	// Diagnostics only - the plugin never needs it - but it is what lets a test check the
	// firmware-to-engine bridge byte for byte instead of guessing from the sound. Both
	// halves load their tones from the same ROM, so where the bridge is correct the two
	// copies must be identical.
	bool readEngineMemory(juce::uint32 packedAddress, juce::uint32 length, juce::uint8 *out);

	// Where the firmware's battery RAM and memory card persist: beside the ROMs in the
	// plugin's own data folder, as with the other synths in this series. That folder sits
	// under Program Files and is not writable everywhere, so a write is tried once and the
	// user's app data stands in when it fails - nvramIsBesideRoms() says which is in use.
	//
	// ONE memory, shared and permanent, exactly as the instrument has one set of batteries.
	// It survives the host being closed, so the unit is found as it was left; a project
	// additionally carries its own copy, which is restored over this when it loads.
	static juce::File getNvramRoot();
	juce::File getNvramFolder() const;
	// False when the data folder turned out not to be writable and app data is being used
	// instead, so the panel's menu can say so rather than leave it a mystery.
	static bool nvramIsBesideRoms();

	// Set when a power-on was refused because another instance already holds the emulated
	// machine. Only one can run per process - see D110Core::start.
	bool isPowerBlocked() const { return powerBlocked; }
	// MAME rompath for the d110 romset: the plugin's own data folder first, then the
	// machine's standing MAME ROM folders so a development box works without copying.
	static juce::String getMameRomPath();

	// Where the custom ROM folder override (see getCustomRomFolder()) is stored - a plain
	// one-line text file, deliberately outside getNvramRoot()/getAutoRomFolder() (there's no
	// ROM folder to resolve yet at the point this itself needs to be read).
	static juce::File getCustomRomPathFile() {
		return d110appdata::resolve("custom_rom_path.txt");
	}

	// Same pattern as getCustomRomPathFile() above, for the custom-PCM-sample folder setting.
	static juce::File getCustomSamplePathFile() {
		return d110appdata::resolve("custom_sample_path.txt");
	}

	// Same pattern again, for the Soundbanks source-folder setting - see
	// getSoundbankSourceFolder().
	static juce::File getSoundbankSourcePathFile() {
		return d110appdata::resolve("soundbank_source_path.txt");
	}

	// A fixed, plain-filesystem staging folder for individually-picked SysEx/MIDI/.zip files -
	// as opposed to getSoundbankSourceFolder() above, which is ONE user-chosen folder path
	// (typically an existing personal library, scanned in place). Originally Android-only
	// (Main.cpp's copySoundbankFiles() - Android can't reliably list a picked SAF folder's
	// contents, so individual files are copied here instead and this folder is pointed at as
	// THE source); reused by the desktop SoundbankBrowser's own "CHOOSE FILES/FOLDER..." button
	// (Alan's request, 2026-08-28: importing a loose file or a .zip directly, without disturbing
	// whatever folder-based source is already configured) - see
	// SoundbankBrowser::startRescan()'s own comment for why RESCAN always scans this folder in
	// ADDITION to getSoundbankSourceFolder(), never replacing it.
	static juce::File getSoundbankImportsFolder() {
		return d110appdata::resolve("soundbank_source");
	}

	// The panel draws its own VOLUME knob out of the reference photograph, so it talks to the
	// parameter directly rather than through a SliderAttachment.
	float getMasterVolume() const;
	void setMasterVolume(float newValue); // 0..1, with host automation gestures

	// Extracts SysEx messages from a .syx (raw concatenated dumps) or .mid/.smf (SysEx meta-events
	// only - notes are ignored) file and queues them to be sent to the synth. Safe to call from the
	// message thread: the messages are picked up and actually sent from processBlock on the audio thread.
	void importSysexBank(const juce::File &file);
	juce::String getLastImportMessage() const { return lastImportMessage; }

	// A snapshot of the firmware's whole memory - every patch, timbre, system setting and the
	// memory card - as one file, separate from a DAW project and separate from a real Roland
	// SysEx bank (that's importSysexBank() above, which plays a file down the emulated MIDI
	// cable exactly as a librarian would). This instead saves and restores the plugin's own
	// exact memory image byte for byte, the same image a DAW project already carries in its own
	// state (see getStateInformation/setStateInformation) - just as a standalone file the user
	// can keep and reload independent of any project.
	void exportMemorySnapshot(const juce::File &file);
	// If the instrument is currently switched on, this powers it off (flushing and replacing
	// its memory), then powers it back on with the snapshot's memory loaded - so the change is
	// felt immediately rather than only on the next manual power cycle.
	void importMemorySnapshot(const juce::File &file);

	// The sequencer's 4 song slots, as one standalone file - tempo, time signature and all 9
	// tracks per slot, the same "song content" getStateInformation already carries (not the
	// workspace prefs like metronome/record mode/loop, which stay local). Redundant with the
	// per-song "SAVE/LOAD .mid" already on the sequencer panel when you only care about one
	// song, but this is what actually survives a move to a different machine's Standalone -
	// unlike a DAW project, the Standalone's own state lives in a local settings file
	// (~/.config on Linux) that copying the shared NVRAM folder does not carry along.
	void exportSequencerSongs(const juce::File &file) override;
	void importSequencerSongs(const juce::File &file) override;

	// The same memory, but as a real Roland "Data set 1" SysEx bank instead of this plugin's
	// own snapshot format - built directly from the current RAM image rather than captured by
	// recording a live transfer, so it's instant regardless of how big the bank is. Every
	// documented area of the map is covered (Patches, Timbre Temporary, Rhythm Setup, Tone
	// Temporary, Timbre Memory, System, Tone Memory - docs/sysex_address_map.md); the
	// undocumented span between System and Tone Memory is firmware working state, not
	// patch/tone data, and is deliberately left out, exactly as the live engine mirror already
	// leaves it out. The result plays back into the instrument through the existing "IMPORT
	// SysEx / MIDI BANK" path, exactly as it would from real hardware or a librarian.
	void exportSysexBank(const juce::File &file);

	// Patch browsing, matching the real D-110's PART + VALUE/NUMBER workflow: PART selects which
	// of the 8 Parts you're browsing, VALUE/NUMBER steps that Part's Patch/Program up or down.
	// Assumes the default factory MIDI channel assignment (Part i -> channel i+1), since the
	// SYSTEM page that would let you reassign channels isn't implemented yet.
	void selectNextPart();
	void selectPreviousPart();
	void stepPatch(int direction); // direction: +1 or -1

	// "MIDI Panic" - kills every currently sounding voice immediately, on both the sound
	// engine and the firmware, for the rare case something leaves notes hung. All Notes Off
	// (CC 123) plus Hold Pedal Off (CC 64, so sustained notes actually stop too) on all 16
	// MIDI channels, not just the factory Part 1-8/Rhythm map, since channels can be
	// reassigned. Safe to call from the message thread (a button click) - queued the same
	// way stepPatch()'s program changes already are, not sent directly.
	// What the sequencer's STOP calls (D110SequencerHost): CC64/CC123 on every channel, the engine's own
	// note-offs, and the direct MIDI Out. It does NOT touch the firmware's voice-slot table - see midiPanicHard().
	void midiPanic() override;
	// The explicit MIDI PANIC (Utility tab): everything midiPanic() does, plus clearing every LA32 voice slot
	// behind the firmware's back for 1.5 s, which frees notes the firmware itself never releases. That is NOT
	// safe while the firmware is busy: DEBUG logs of 2026-10-02 show a quick STOP then PLAY leaving it wedged
	// (all slots idle, CPU looping in the voice-chain walk or parked at the dispatch wait, front panel dead until
	// a power cycle), hence the split.
	void midiPanicHard() override;
	void midiPanicImpl(bool resetSlotTable);

	// --- the extended editor ---------------------------------------------------
	// Everything the drawer edits goes to the INSTRUMENT, as a Roland DT1 into its own
	// MIDI IN - exactly what an external editor would send to the hardware. The firmware
	// changes its memory, the mirror carries that to the sound engine, and the panel's own
	// display shows it too. Nothing here writes to the sound engine directly, so an edit
	// made in the drawer and an edit made on the panel are the same event.
	//
	// Measured, not assumed: plugin/editor_write_probe.cpp sends one write into each area
	// and checks which byte of the firmware's battery RAM moved. All of them land where
	// Roland's map says, and Mem Protect does not stand in the way of exclusive writes.
	void sendAreaData(juce::uint32 sysexAddress, int offset, const juce::uint8 *data, int length);
	// The SysEx device ID the firmware currently answers to (its Exclu Unit#, RAM 0x2DB6), so
	// internally generated DT1 writes still land when that isn't the factory 0x10. Falls back
	// to 0x10 when the firmware isn't running or the byte isn't a valid ID (0-31).
	juce::uint8 firmwareDeviceId() const;
	void sendTimbreTempParam(int part, int field, juce::uint8 value);
	void sendToneTempParam(int part, int offset, juce::uint8 value);
	void sendRhythmParam(int slot, int field, juce::uint8 value);
	void sendSystemParam(int field, juce::uint8 value);
	void sendTimbreMemoryParam(int slot, int field, juce::uint8 value);
	void sendPatchMemoryParam(int patch, int field, juce::uint8 value);
	// Wired to sequencerEngine's setSysExPreambleSource() in the constructor - see
	// sequencerLiveInternalTone/sequencerLiveToneMemory's own comment for what the Tone part of
	// it reads. Always includes one channel-assign message for a melodic track (Alan's report,
	// 2026-08-29: a song exported with a customised channel map played on the wrong Part when
	// loaded into a real D-110 whose own SYSTEM page still had a different map - nothing was
	// tying the file's channel numbers to the receiving unit's own) - the Tone Memory dump +
	// TimbreTemp pointer are the rest, and stay conditional on the track actually being on an
	// Internal tone (the common case has neither). Each returned element is one DT1 message's
	// own payload bytes (Roland header through checksum, no F0/F7 - see buildDt1Message()),
	// ready for juce::MidiMessage::createSysExMessage(), which adds the wrapper itself.
	std::vector<std::vector<juce::uint8>> buildTrackSysExPreamble(int track) const;
	// The load-side mirror: wired to sequencerEngine's setLoadedTrackSetupSink() in the
	// constructor. loadMidiFile() hands back whatever SysEx/Program Change/Volume/Pan it found
	// in a track (see that setter's own comment for why - Alan noticed reimporting a track could
	// change how it sounded, 2026-08-21). Program Change and the Internal-tone SysEx preamble
	// are replayed as plain live MIDI through osMidiCollector; Volume/Pan (CC7/CC10) instead go
	// out as direct sendTimbreTempParam() writes, confirmed necessary against a real DAW
	// session - live CC7/CC10 has no audible effect on this instrument at all, see this
	// method's own .cpp comment for the full story (including an intermediate, reverted
	// attempt at live-replaying these too, same day).
	void applyLoadedTrackSetup(int track, std::vector<juce::MidiMessage> setup);
	void queueTimbreTempParam(int part, int field, juce::uint8 value);
	// Names are the same ten characters the display shows: printable ASCII only, padded with
	// spaces. The unit knows no others, and a byte above 0x7F is impossible in an exclusive message.
	void sendName(juce::uint32 sysexAddress, int offset, const juce::String &name);
	// A message on the unit's display - Roland's standard command at address 0x200000.
	void sendDisplayMessage(const juce::String &text);
	// The exact same byte-for-byte SysEx that sendDisplayMessage() sends, as a space-separated
	// hex string ("41 10 16 12 ...") - for right-click on Send, to copy to the clipboard and
	// paste into another program (e.g. MuSE). Doesn't need core.isRunning().
	static juce::String displayMessageSysexHex(const juce::String &text);

	// Changing a part's timbre is an ordinary Program Change on its own MIDI channel, as from an
	// external keyboard. The channel is taken from the firmware's map, not computed by formula:
	// the factory layout "part N on channel N+1" can be changed, and a formula would miss.
	void selectTimbreForPart(int part, int timbre);
	// Same live-channel lookup selectTimbreForPart() does internally (System Area, chanAssign -
	// the factory "Part N -> channel N+1" formula is changeable, so this reads the map rather
	// than assuming it), exposed on its own for anything that needs to send a real MIDI note to
	// a Part rather than a Program Change - the Android app's Soundbanks test-note button
	// (Main.cpp), 2026-08-30. Returns the channel 1-16 (matching injectTestNote()'s own
	// juce::MidiMessage convention), or -1 if the part is off (no channel assigned) or the
	// firmware isn't running.
	int liveChannelForPart(int part) const;
	// Switching to a patch by PRESSING THE PANEL'S OWN BUTTONS: Patch, then Bank+ and Number+ as
	// many times as needed. The firmware selects the patch - unpacking it into the temporary
	// areas, writing its own text on the display and raising the mirror - rather than us doing
	// it on its behalf.
	//
	// Bank+ moves the number by 8, Number+ by 1 (measured by editor_write_probe), so any of the
	// 64 patches is at most fourteen presses away.
	void selectPatch(int patch);
	bool isSelectingPatch() const { return patchSteps > 0; }
	// Number of the patch the unit is playing right now (0..63), or -1 if memory has not been read yet.
	int currentPatchNumber() const;

	// Editing a field of a STORED patch, audible at once - if the patch being edited is the one
	// the unit is currently playing.
	//
	// On the unit these are two different things: it plays from the temporary areas, and patch
	// memory is only a snapshot that lands there when a patch is selected. So editing memory
	// alone is silent, and an editor pretending otherwise lies twice: it is mute where you expect
	// sound, and it changes what cannot be heard. Here BOTH copies are written when the current
	// patch is concerned, and only memory for any other.
	void editPatchField(int patch, int field, juce::uint8 value);

	// Audition a tone from memory: its 246 bytes go into the part's temporary area, and the part
	// starts playing it immediately. This is exactly what "Recall" does, just without the button.
	void auditionTone(int part, int slot);
	// And the reverse: the tone the part is playing now is stored into a memory slot.
	void storeToneFromPart(int part, int slot);

	// Log of received messages for the MONITOR tab - a lock-free ring of 64 entries.
	struct MidiLogEntry {
		juce::uint8 status = 0, data1 = 0, data2 = 0;
		juce::uint16 size = 0;
	};
	int getMidiLog(MidiLogEntry *out, int max) const;

	juce::AudioProcessorValueTreeState parameters;

private:
	void closeSynth();
	void rebuildSampleRateConverter();
	void handleIncomingMidiMessage(const juce::MidiMessage &message);
	// Hands a host MIDI message to the emulated control board as well as to the sound
	// engine, so the firmware's own display tracks what is being played.
	void forwardMidiToFirmware(const juce::MidiMessage &message);

public:
	// Whether note on/off reach the control board. Always yes; turning it off remains only for
	// test rigs and is not exposed in the UI.
	//
	// Enabled IS the unit's behaviour: the firmware applies its own key ranges, part layout and
	// voice allocation, lights the indicators on the top line of the LCD and reports back which
	// note it actually took on which part; only then does the note reach the sound engine.
	//
	// Turning it off was once a workaround: MAME emulates the LA32 for none of Roland's LA
	// machines, and on reaching voice allocation the firmware stopped scanning the front panel -
	// sound came out, but buttons and display died. D110Core::StuckPolicy::La32Stub, enabled
	// unconditionally in setPoweredOn(), fixes that; details in docs/la32_interface.md. Rigs that
	// need the firmware without notes (plugin/longrun_test.cpp, plugin/hang_probe.cpp) use this
	// setter directly.
	void setForwardNotesToFirmware(bool shouldForward) { forwardNotes = shouldForward; }
	bool getForwardNotesToFirmware() const { return forwardNotes; }

	// Sounds one part directly, with no firmware involvement - diagnostics only. It is the
	// only way to ask what the ENGINE does with a part, separately from whether the
	// firmware is sending it anything. Safe to call from a test's own thread because
	// nothing else is writing to the synth at the time; not for use from the plugin.
	void playNoteOnPartForTest(uint8_t part, uint8_t note, uint8_t velocity) {
		if (synth) synth->playMsgOnPart(part, 0x9, note, velocity);
	}
	void playNoteOffOnPartForTest(uint8_t part, uint8_t note) {
		if (synth) synth->playMsgOnPart(part, 0x8, note, 0);
	}
	// Which parts the ENGINE currently has sounding, as a bitmask - the counterpart to the
	// firmware's own indicators. Where the two disagree, the note reached the engine and
	// the engine chose not to sound it, which is a different fault from losing the note.
	// What the engine holds for a part, so it can be compared against what the firmware
	// holds for the same part. Diagnostics: a difference here is the bridge failing to
	// carry something across, which is invisible from either side alone.
	void engineReadMemory(uint32_t sysexAddr, uint32_t len, uint8_t *out) const {
		if (synth) synth->readMemory(sysexAddr, len, out);
	}
	// Writes into the engine's memory the same way the bridge does, but with no firmware behind
	// it - diagnostics only. A value read back does not by itself prove the read path works; a
	// known value written and read back does - that check run is the whole reason this method
	// exists. Same threading rule as playNoteOnPartForTest: the test's own thread, and nobody
	// else writing to the synth.
	void engineWriteSysexForTest(const uint8_t *data, int len) {
		if (synth) synth->playSysex(data, static_cast<MT32Emu::Bit32u>(len));
	}
	// Whether the sound engine opened at all. Any reading of the form "the engine holds zeros"
	// must rule this out first: readMemory() on a closed synth returns without touching the
	// buffer, and that reads as data if the buffer was zeroed to begin with.
	bool engineIsOpen() const { return synth != nullptr; }
	uint32_t enginePartStates() const { return synth ? synth->getPartStates() : 0u; }
	uint32_t enginePartialCount() const { return synth ? synth->getPartialCount() : 0u; }
	// How many times Part::abortFirstPoly()'s fallback release has fired since the engine
	// opened - see that function's own comment (munt/mt32emu/src/Part.cpp) and
	// project_sequencer_channel_collision_fix memory. Logged alongside the debugModeEnabled
	// tally below so a real session either does or doesn't show this path firing, instead of
	// only ever being tested in a synthetic repro attempt.
	uint32_t engineAbortFallbackCount() const { return synth ? synth->getAbortFallbackCount() : 0u; }
	// How many partials are busy right now. If this sits at the ceiling while a part is
	// silent, the part is being starved rather than ignored.
	int engineActivePartials() const {
		if (!synth) return -1;
		const uint32_t total = synth->getPartialCount();
		std::vector<MT32Emu::Bit8u> states(total, 0);
		synth->getPartialStates(states.data());
		int busy = 0;
		for (uint32_t i = 0; i < total; ++i)
			if (states[i] != 0) ++busy; // 0 is INACTIVE
		return busy;
	}

private:
	std::atomic<bool> forwardNotes{true};

	// Messages arriving on the directly-opened port. They are queued here and merged into
	// the next processBlock rather than acted on straight away, because that call arrives
	// on the OS's own MIDI thread and both the engine and the control board expect their
	// input from one place.
	void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &) override;
	std::unique_ptr<juce::MidiInput> osMidiIn;
	std::unique_ptr<juce::MidiOutput> osMidiOut;
	juce::String selInputId, selOutputId;
	juce::CriticalSection osMidiLock;
	juce::MidiMessageCollector osMidiCollector;

#ifdef D110_HAVE_JACK_MIDI
	// Standalone only (checked at runtime in prepareToPlay(), where wrapperType is reliably
	// set - see its own comment): a real JACK MIDI input port, separate from the ALSA device
	// picker above. Feeds the very same handleIncomingMidiMessage() path, so the MIDI Remap
	// setting behaves identically regardless of which of the two a message arrived through.
	std::unique_ptr<JackMidiInput> jackMidiIn;
	bool jackMidiSetupAttempted = false;
#endif

	// Opens the synth from whatever is currently in controlRomData/pcmRomData.
	bool openSynthIfReady();
	// Scans getAutoRomFolder() for a Control ROM and PCM ROM by content (not filename) and
	// loads them automatically if both are found - so the user doesn't have to pick files by hand.
	bool tryAutoLoadRoms();
	// Second chance for the auto-scan: mt32emu wants one combined Control image and one
	// combined PCM image, but a MAME romset ships the individual chips instead. This looks
	// for those chips - loose in the folder or inside a d110 .zip - and joins them in memory
	// into exactly the images mt32emu expects. Returns whether both were assembled.
	bool tryAssembleRomsFromChipDumps(const juce::File &folder);
	// Reads `file` and hands it to mt32emu, returning its ROMInfo type via `typeOut`.
	static bool identifyRomData(const juce::MemoryBlock &data, MT32Emu::ROMInfo::Type &typeOut);
	// See this method's own .cpp comment: copies any recognisable whole ROM image found loose
	// next to the VST3 bundle/Standalone binary into `dest`, if `dest` doesn't already have one.
	static void materializeLooseRomsIfNeeded(const juce::File &dest);
	// D110CoreNative::start() (unlike the content-based loading above) looks up its firmware
	// and presets files by exact, hardcoded name - see setPoweredOn(). Writes those two exact
	// filenames, derived from controlRomData (already identified/assembled above, in whatever
	// shape/name it was actually supplied under), if they aren't already present as their own
	// files. No-op if controlRomData isn't populated yet.
	void materializeNativeRomFiles();

	juce::MemoryBlock controlRomData, pcmRomData;
	// The BOSS reverb chip's own 32 KiB program ROM (IC6). Optional: absent, the engine
	// falls back to its own four built-in modes, same as before this was wired in.
	juce::MemoryBlock bossRomData;
	std::unique_ptr<MT32Emu::ArrayFile> controlRomFile;
	std::unique_ptr<MT32Emu::ArrayFile> pcmRomFile;
	const MT32Emu::ROMImage *controlROMImage = nullptr;
	const MT32Emu::ROMImage *pcmROMImage = nullptr;
	std::unique_ptr<MT32Emu::Synth> synth;
	std::unique_ptr<MT32Emu::SampleRateConverter> sampleRateConverter;

	juce::String controlRomPath, pcmRomPath;
	juce::String controlRomDescription, pcmRomDescription;
	juce::String lastError;

	double currentSampleRate = 44100.0;
	std::vector<float> interleavedScratch;
	// Audio-thread scratch storage for processBlock(), pre-sized in prepareToPlay() so the
	// per-block MIDI merging/remapping and queue draining never allocate - see the
	// comments at each use site in processBlock().
	juce::MidiBuffer midiRemapScratch, portMidiScratch, sequencerOutScratch;
	std::vector<d110seq::D110SequencerEngine::MetronomeClick> sequencerClicks;
	std::vector<std::vector<MT32Emu::Bit8u>> pendingImportsToSendScratch;
	std::vector<MT32Emu::Bit32u> pendingShortMessagesToSendScratch;
	std::vector<juce::uint8> pendingPanicBytesToSendScratch;

	std::atomic<float> *masterVolumeParam = nullptr;
	std::atomic<float> *reverbEnabledParam = nullptr;
	std::atomic<float> *superModeParam = nullptr;
	bool lastSuperModeApplied = false;
	std::atomic<bool> poweredOn{false};

	// Guards `synth`/`sampleRateConverter` (and the ROM image/file objects behind them)
	// against being swapped out from under processBlock() by openSynthIfReady() running on
	// the message thread - see the note beside superModeReopenPending in processBlock().
	// Only ever held for the O(1) pointer handover on the writer side and for one block's
	// processing on the reader side, never for the (re)build itself.
	juce::CriticalSection synthAccessLock;
	// Set while an async re-open triggered by a live Super Mode toggle is in flight, so a
	// mismatch seen on a later block before it lands doesn't queue a second one.
	std::atomic<bool> superModeReopenPending{false};
	// Refcount-only; openSynthIfReady()'s async re-open holds a weak_ptr to this so it can
	// tell the processor was destroyed before the message thread got to run it, rather than
	// call into a dangling `this`.
	std::shared_ptr<char> lifeToken = std::make_shared<char>();

	// Guards both pending queues below. Synth's MIDI queue only tolerates a single writer thread
	// (the audio thread, via processBlock), so anything triggered from the message thread (button
	// clicks, file loads) is queued here and drained/applied from processBlock instead of calling
	// synth->playMsg()/playSysex() directly.
	juce::CriticalSection engineActionLock;
	std::vector<std::vector<MT32Emu::Bit8u>> pendingSysexImports;
	std::vector<MT32Emu::Bit32u> pendingShortMessages;
	// Raw bytes queued for the FIRMWARE's own MIDI input by midiPanic() - separate from
	// pendingSysexImports because those go through synth->playSysex() too, which expects a
	// proper F0...F7 sysex frame, not the short channel-voice messages a panic sends.
	std::vector<juce::uint8> pendingPanicBytes;
	// midiPanic()'s own request to also call core.releaseStuckNoteContexts() (see its own
	// comment) - guarded by engineActionLock like the two queues above, since that method
	// touches `core` state (noteQueue_ etc.) that only the audio thread may otherwise touch.
	// One-shot: applied on the very next processBlock() and then cleared.
	bool pendingForceReleaseStuckVoices = false;
	// Same idea, for core.resetVoiceSlotTable() - NOT one-shot, see that method's own comment
	// for why a single call loses a race with the firmware's own delayed response to the
	// panic's CC64/CC123 bytes. Counts down in SAMPLES (exact regardless of block size) from
	// wherever midiPanic() sets it (message thread); processBlock() (audio thread) calls
	// resetVoiceSlotTable() again on every block while this is still > 0, so the LAST call -
	// after the firmware's own response has had time to either genuinely finish or genuinely
	// never finish - is the one that sticks. std::atomic rather than the lock the two queues
	// above use: a plain write-here/read-there int would be the same kind of unsynchronised
	// cross-thread access already found and fixed once this session (see RescanThread's own
	// comment in SoundbankBrowser.cpp) - a single relaxed word is enough here, no payload data
	// to keep consistent alongside it the way the locked queues have.
	std::atomic<int> pendingSlotTableResetSamplesRemaining{ 0 };
	juce::String lastImportMessage;

	// Active custom PCM wave overrides, keyed by wave index (0-255) - the log-format samples
	// last handed to synth->setPCMWaveSamples(), persisted so a reload/power-cycle can reapply
	// them (see openSynthIfReady()'s own comment; pcmROMData is decoded fresh from the real ROM
	// file every time, wiping any override). See loadCustomPcmWave()/restoreFactoryPcmWave().
	std::map<int, std::vector<MT32Emu::Bit16s>> customPcmWaves;
	// Source file name (no extension) per customized wave, purely cosmetic (see
	// getCustomPcmWaveName()) - persisted alongside customPcmWaves in getStateInformation.
	std::map<int, juce::String> customPcmWaveNames;
	// Current loop on/off per customized wave (see setCustomPcmWaveLoop()) - persisted.
	std::map<int, bool> customPcmWaveLoops;
	// The real ROM's own original samples for whatever's in customPcmWaves, snapshotted the
	// first time each slot is touched each time the synth (re)opens - not persisted (cheap to
	// re-derive from the ROM itself, and it's exactly what powers restoreFactoryPcmWave()).
	std::map<int, std::vector<MT32Emu::Bit16s>> factoryPcmWaveBackup;
	// Same idea, for the wave's pitch calibration (see MT32Emu::Synth::setPCMWavePitchOffset()'s
	// own comment) - not persisted, re-snapshotted alongside factoryPcmWaveBackup.
	std::map<int, MT32Emu::Bit16u> factoryPcmWavePitchBackup;
	// Same idea, for the wave's loop flag - kept separate from customPcmWaveLoops (the current,
	// possibly user-overridden setting) since restoreFactoryPcmWave() needs the ORIGINAL value
	// specifically, not whatever the user last toggled it to.
	std::map<int, bool> factoryPcmWaveLoopBackup;
	// Applies every entry in customPcmWaves to the (already-open) synth, snapshotting each
	// slot's current (factory) content into factoryPcmWaveBackup first. Called from
	// openSynthIfReady() (every power-on/ROM reload wipes pcmROMData back to factory) and from
	// setStateInformation() (so a project reload takes effect immediately on an already-running
	// instance, not just on the next power cycle). No-op if synth isn't open.
	void reapplyCustomPcmWaves();

	D110CoreType core;

	bool powerBlocked = false;

	// D110Keyboard's own config - see the accessors above for why this lives here rather than
	// on the UI component itself.
	int keyboardMidiChannel = 1;
	// Default OFF since 2026-08-25 (Github issue #4; named "Omni" until the same day - see
	// D110KeyboardHost.h's own comment on the rename). With this ON, processBlock() and
	// handleIncomingMidiMessage() force EVERY incoming channel-voice message - host-routed or
	// from a directly-opened port, not just the on-screen keyboard's own notes - onto
	// keyboardMidiChannel above. That's the right behaviour for a controller hardwired to one
	// channel (the reason this remapping exists at all), but the wrong default for anyone
	// sending real per-Part multichannel MIDI from a DAW, which is the common case. Still
	// fully persisted/toggleable exactly as before - see getStateInformation's "kbMidiRemap"
	// (with a legacy-"kbOmni" fallback) and D110Panel::showOptionsMenu()'s own MIDI Remap
	// entry.
	bool midiRemap = false;
	bool keyboardPcInput = false;
	int keyboardPcLayout = 0;
	int keyboardNumOctaves = 2;

	// See isNoteActive() above - one flag per MIDI note number, written from the audio thread
	// (handleIncomingMidiMessage(const juce::MidiMessage&), the single point every note
	// reaching the firmware already passes through: host MIDI, the Standalone port, the
	// on-screen keyboard's own notes, and sequencer playback), read from the message thread.
	std::array<std::atomic<bool>, 128> remoteNoteActive{};

	// See getUiThemeLight()/setUiThemeLight() above.
	bool uiThemeLight = false;
	// See getUiThemeFollowSystem()/setUiThemeFollowSystem() above.
	bool uiThemeFollowSystem = false;
	// See getUiFontScaleBig()/setUiFontScaleBig() above.
	bool uiFontScaleBig = false;

	// See getSoundbankDatabase() above.
	d110bank::Database soundbankDb;
	// See getSoundbankFavorites() above.
	d110bank::Favorites soundbankFavorites;
	// See getSequencerRetroMode()/setSequencerRetroMode() above.
	bool sequencerRetroMode = false;
	bool sequencerGridMode = false;
	int gridRowHeight = 0; // see D110SequencerHost::getGridRowHeight()
	bool compactPanelMode = false;
	juce::String retroKeyBindings;
	bool retroLcdCompactMode = false;
	bool debugModeEnabled = false;
	// See getLastDialogDir()/setLastDialogDir() above.
	juce::File lastDialogDir;
	// See getEditorPaneRefH()/setEditorPaneRefH() above. 750.0f mirrors
	// D110AudioProcessorEditor::kPaneRefH's own default - duplicated rather than shared
	// because PluginEditor.h isn't (and shouldn't become) a dependency of this header.
	float editorPaneRefH = 750.0f;
	// See getKeyboardPaneRefH()/getSequencerPaneRefH() above - 130.0f/347.0f mirror
	// D110Keyboard::kRefH/D110SequencerPanel::kRefH's own defaults, same duplication reasoning.
	float keyboardPaneRefH = 130.0f;
	float sequencerPaneRefH = 347.0f;

	// --- D-20-style sequencer ---------------------------------------------------
	d110seq::D110SequencerEngine sequencerEngine;
	// Live MIDI channel (1-16, or -1 = unknown/off) for tracks 0-7, refreshed from the
	// firmware's own System-area channel map once per processBlock - see the constructor,
	// which wires sequencerEngine's channel source to read this array rather than hit the
	// core directly (that would mean one getRam() call per note emitted, not per block).
	// Index kRhythmTrack is never read this way; the rhythm track is fixed on channel 10.
	std::array<int, d110seq::D110SequencerEngine::kNumTracks> sequencerLiveChannels{};
	// Same idea, for the raw 0-127 Program Change value that would reproduce each melodic
	// part's live sound (-1 = unknown, or a sound with no such value - internal tone memory/
	// rhythm, see the fill site's own comment) - what sequencerEngine's program source reads
	// to embed a Program Change into MIDI exports (see D110SequencerEngine::setProgramSource).
	// Index kRhythmTrack is never read this way; the rhythm part has no single "current tone"
	// the way a melodic part does.
	std::array<int, d110seq::D110SequencerEngine::kNumTracks> sequencerLivePrograms{};
	// Same idea, LEVEL/PAN (TimbreTemp fields 8/9) - what captureLivePatchIntoTracks() reads.
	// -1 = unreadable (same fallback reasoning as sequencerLivePrograms).
	std::array<int, d110seq::D110SequencerEngine::kNumTracks> sequencerLiveVolumes{};
	std::array<int, d110seq::D110SequencerEngine::kNumTracks> sequencerLivePans{};
	// Same block-refresh, TimbreTemp field 0 (TONE GROUP): -1 unless it reads exactly 2
	// (Internal - see sequencerLivePrograms' own comment on why group 2/3 report no Program
	// Change hint), in which case this holds the Tone Memory slot (0-63, field 1) instead, and
	// sequencerLiveToneMemory below holds that slot's own 256 raw bytes, snapshotted the same
	// block. Both exist for exactly one reason - buildTrackSysExPreamble(), which
	// setSysExPreambleSource() reads at MIDI-export time to embed a DT1 dump of the custom tone
	// into the file, since a plain Program Change can never reach group 2 on a real unit either.
	std::array<int, d110seq::D110SequencerEngine::kNumTracks> sequencerLiveInternalTone{};
	std::array<std::array<juce::uint8, D110CoreType::kToneMemRecord>, d110seq::D110SequencerEngine::kNumTracks>
		sequencerLiveToneMemory{};
	// Reused across blocks so refreshing sequencerLiveChannels/sequencerLivePrograms doesn't
	// reallocate 32KB every time - see their use beside sequencerLiveChannels above.
	std::vector<juce::uint8> sequencerRamScratch;
	// The fixed per-track Program Change/Bank/Volume/Pan override itself now lives directly on
	// D110SequencerEngine (per song slot - see D110SequencerEngine::getTrackProgram()'s own
	// comment, 2026-08-21). getTrackProgram()/setTrackProgram()/etc. below just delegate to it;
	// no storage of its own here any more.
	// Set by resyncProgramChanges() (any thread, hence atomic - the UI thread is who actually
	// calls it), cleared by processBlock() once it's acted on it - see its own comment.
	std::atomic<bool> sequencerResyncRequested{false};

	// Per-song sound snapshot - see hasSoundSnapshot()/storeSoundSnapshotForSlot()/
	// loadSoundSnapshotForSlot(). Empty MemoryBlock (default) = no snapshot stored for that
	// slot yet. Indexed by song slot, not by track - unlike sequencerTrackPrograms above.
	std::array<juce::MemoryBlock, d110seq::D110SequencerEngine::kNumSongSlots> songSoundSnapshots;
	bool wasSequencerPlayingForProgramSend = false;
	// Only active when debugModeEnabled (Utility tab -> DEBUG) - see processBlock()'s own
	// diagnostic instrumentation comment. Tracks the hit/miss rate of individual per-part
	// note-on attempts vs that SAME part actually showing up in the engine's own
	// enginePartStates() a few blocks later.
	struct DiagPendingCheck { int part; int checksLeft; };
	std::vector<DiagPendingCheck> diagPendingChecks;
	int diagAttempts = 0, diagHits = 0, diagMisses = 0;
	juce::int64 diagLastFlushMs = 0;
	// Metronome click envelope state, carried across processBlock calls since a click's
	// short decay can span more than one audio block.
	int metronomeSamplesRemaining = 0;
	double metronomePhase = 0.0;
	double metronomeFreq = 1000.0;

	// Writes the saved firmware memory into this instance's folder, ready for the machine
	// to pick up next time it starts, and reads it back out again.
	void writeNvramFiles(const juce::MemoryBlock &rams, const juce::MemoryBlock &memcs) const;
	juce::MemoryBlock readNvramFile(const juce::String &name) const;

	// core.stop() (called from setPoweredOn(false)) is normally the only place the shared
	// nvram files get written - see setPoweredOn()'s own comment. That only fires on an
	// explicit POWER OFF, so an instance destroyed while still powered on would otherwise
	// lose every edit since the last power-off. Called from the destructor as a safety net.
	void flushLiveNvramToDisk();

	// The sequencer's 4-slot song content (tempo/timesig/tracks/mute/solo/quantize, NOT the
	// workspace prefs) as XML attributes on whatever element is passed in - shared by
	// getStateInformation/setStateInformation (which write onto the whole-plugin state
	// element) and exportSequencerSongs/importSequencerSongs (which use a small standalone
	// element of their own), so the two paths can't drift apart.
	void writeSequencerSongsXml(juce::XmlElement &xml) const;
	void readSequencerSongsXml(const juce::XmlElement &xml);

	// Where MAME keeps `rams` and `memcs` - one folder, shared, persistent.
	static juce::File getMachineNvramFolder();

	std::atomic<int> selectedPartIndex{0};
	std::array<int, 8> currentProgramPerPart{};

	// --- switching patches with the panel buttons --------------------------------
	// Queue of buttons still to press, and the phase of the current press (0 - down,
	// 1 - up). Lives on the message timer: emulated time must pass between presses,
	// otherwise the scan matrix simply never sees the second half.
	void timerCallback() override;
	std::vector<int> patchQueue;
	int patchSteps = 0;
	int patchPhase = 0;

	// --- log of received messages -------------------------------------------------
	// Fixed-size ring, one writer (the audio thread) and one reader (the UI), hence no
	// locks: the writer stores the entry and only then advances the counter.
	static constexpr juce::uint32 kMidiLogSize = 64;
	void logIncomingMidi(const juce::MidiMessage &message);
	std::array<MidiLogEntry, kMidiLogSize> midiLog{};
	std::atomic<juce::uint32> midiLogWrite{0};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessor)
};
