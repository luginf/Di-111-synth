#pragma once

// Abstract interface D110Keyboard talks to its owner through - same decoupling pattern
// as D110SequencerHost (see sequencer/D110SequencerHost.h) uses for D110SequencerPanel.
// D110AudioProcessor implements this for the plugin's own keyboard drawer, NonetSeqHost
// for the independent Nonet Sequencer app's keyboard.
class D110KeyboardHost {
public:
	virtual ~D110KeyboardHost() = default;

	virtual void injectTestNote(int channel, int note, float velocity, bool on) = 0;

	// D110Keyboard's own config (MIDI channel/remap, PC-keyboard tracker input,
	// QWERTY/AZERTY) - read once at construction and written back on every change, so it
	// survives as long as the host does.
	virtual int getKeyboardMidiChannel() const = 0;
	virtual void setKeyboardMidiChannel(int channel) = 0;
	// When true, every note this keyboard plays (and, in the D-110 plugin, all incoming host/
	// port MIDI too - see PluginProcessor.cpp's own comment) is forced onto getKeyboardMidi
	// Channel() instead of going out unchanged. Renamed from "Omni" (2026-08-25, Alan's
	// request) - the old name suggested "arrives on every channel", which is backwards from
	// what turning it OFF actually does (broadcast to all 16 / pass external MIDI through
	// untouched); MIDI Remap names the ACTUAL effect instead of borrowing a MIDI-spec term
	// that didn't quite fit either state.
	virtual bool getMidiRemap() const = 0;
	virtual void setMidiRemap(bool remap) = 0;
	virtual bool getKeyboardPcInputEnabled() const = 0;
	virtual void setKeyboardPcInputEnabled(bool enabled) = 0;
	// 0 = QWERTY, 1 = AZERTY.
	virtual int getKeyboardPcLayout() const = 0;
	virtual void setKeyboardPcLayout(int layout) = 0;
	// How many octaves showContextMenu()'s "4-octave keyboard (wide)" toggle currently shows -
	// 2 (the long-standing default) or 4 (Alan's request, 2026-09-02, for D-50 PCM listening
	// tests spanning a wider range than two octaves comfortably reach). Persisted the same way
	// as the settings above, added 2026-09-03 once Alan asked for it to survive a relaunch
	// rather than reset to 2 every time.
	virtual int getKeyboardNumOctaves() const = 0;
	virtual void setKeyboardNumOctaves(int numOctaves) = 0;

	// Velocity (1-127) of the notes the on-screen keyboard plays - Alan's request, 2026-10-20,
	// after the same option went into the JiV-881's keyboard. Default 100. Persisted like the
	// settings above.
	virtual int getKeyboardVelocity() const = 0;
	virtual void setKeyboardVelocity(int velocity) = 0;

	// For the on-screen keyboard's own activity LEDs: true if `note` (0-127, any channel) is
	// currently sounding anywhere in the app - external MIDI In, sequencer playback, or (in
	// the plugin) the DAW host's own track - as opposed to what was struck directly on this
	// keyboard, which it already tracks itself (held mouse/PC key state, lit instantly with
	// no need to go through this). Backed by a small lock-free array the host's audio/MIDI
	// thread writes to at the same point every one of those sources funnels through anyway;
	// read here from the keyboard's own low-Hz timer, on the message thread.
	virtual bool isNoteActive(int note) const = 0;
};
