#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <set>
#include <vector>

#include "D110KeyboardHost.h"

// On-screen test keyboard: a two-octave mouse piano plus optional tracker-style PC
// keyboard input (QWERTY/AZERTY, FastTracker2/Impulse Tracker convention), MIDI
// channel/remap set via right-click. Talks to its owner only through D110KeyboardHost,
// so it works identically inside the plugin's own drawer and inside the independent
// Nonet Sequencer app's window.
//
// Two ways to strike a key: the mouse, on the drawn keys, and - opt-in, right-click to
// enable - the computer keyboard, in the layout every tracker (FastTracker2, Impulse
// Tracker, OpenMPT, Renoise...) has used since the 1990s: two overlapping rows, the lower
// one ZSXDCVGBHNJM,L.;/ starting at the current base octave, the upper one Q2W3ER5T6Y7UI9O0P
// one octave above it. QWERTY and AZERTY differ only in which CHARACTER a given physical
// key sends, not in the note it plays, so trackerKeys() carries both and the chosen layout
// just picks which column to compare incoming key text against.
class D110Keyboard : public juce::Component, private juce::Timer {
public:
	explicit D110Keyboard(D110KeyboardHost &);
	~D110Keyboard() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	bool keyStateChanged(bool isKeyDown) override;
	void focusLost(juce::Component::FocusChangeType) override;

	// Reference height, in the same units as D110Panel::kRefH - what the owning window
	// adds to its own layout for this drawer/strip.
	static constexpr float kRefH = 130.0f;

	// Two octaves is the long-standing default (desktop plugin, Nonet Sequencer - neither
	// calls this, so both keep behaving exactly as before). One octave exists for the Android
	// app's own Options menu (Alan's request, 2026-08-22): a phone-portrait keyboard drawn
	// with two octaves' worth of keys crammed into that width leaves each key too narrow for a
	// finger, and only one octave fits comfortably. Four exists for showContextMenu()'s own
	// "4-octave keyboard" toggle (Alan's request, 2026-09-02, for PCM listening tests in the
	// D-50 editor that span a wider range than two octaves comfortably reach). Clamped to
	// [1,4] - nothing in rebuildKeys() (the trailing-C
	// guard, the tracker PC-keyboard mapping, which spans a fixed semitone range independent
	// of how many are actually drawn) assumes a particular count. Only ever called from
	// showContextMenu() below and from the constructor's restore of host.getKeyboardNumOctaves()
	// - callers that don't go through the host's persisted value (Android's Options menu, still
	// session-only) pass n directly without touching that accessor.
	void setNumOctaves(int n) {
		n = juce::jlimit(0, 4, n); // 0 = resizable: fixed key width, as many keys as the width allows
		if (n == numOctaves) return;
		numOctaves = n;
		rebuildKeys();
		repaint();
	}
	int getNumOctaves() const { return numOctaves; }

	// Right-click on desktop; on Android there's no right mouse button and (see
	// D110Keyboard.cpp's own mouseDown) a long-press on the keys themselves can't stand in for
	// it either - holding a key down is an ordinary sustained note, not a gesture, so timing it
	// out into "must mean a menu" would just break holding notes. The Android app instead
	// reaches this same menu from its own hamburger menu ("Keyboard channel..."), calling this
	// directly rather than reimplementing channel/remap/PC-layout selection a second time - that
	// call path has no specific key in mind, hence the default.
	//
	// `noteForHold`, when >= 0 (mouseDown()'s own right-click handler passes keyAt(e.position)),
	// is which key the click actually landed on - Alan's request, 2026-08-30: prepends a "Hold
	// note" item ahead of everything else, letting that one note sustain indefinitely (see
	// toggleHoldNote()'s own comment) rather than only for as long as the mouse button stays
	// down. Android's own call (no mouse click behind it) and a desktop right-click that misses
	// every key both leave this at -1, which just skips that one item - everything else about
	// the menu is unchanged either way.
	void showContextMenu(int noteForHold = -1);

private:
	int numOctaves = 2;
	static constexpr int kLowestNote = 48; // C3
	static constexpr float kResizableKeyW = 34.0f; // white key width in resizable mode

	enum class PcLayout { qwerty, azerty };

	struct KeyRect { juce::Rectangle<float> bounds; int note; bool black; };

	// One physical key of the tracker layout: the note it plays, relative to the keyboard's
	// current base octave, and which character it sends under each PC layout this offers.
	struct TrackerKey { int semitoneFromBase; juce::juce_wchar qwerty; juce::juce_wchar azerty; };
	static const std::vector<TrackerKey> &trackerKeys();

	void rebuildKeys();
	int keyAt(juce::Point<float>) const;
	// -1 releases. Keyed by MouseInputSource::getIndex() (0 for the mouse; each simultaneous
	// touch gets its own distinct index on a multi-touch platform) rather than one shared note,
	// so several fingers can each hold their own key at once - Alan's request, 2026-08-28: on
	// Android, a single shared "heldNote" made the on-screen keyboard monophonic, since a
	// second finger's mouseDown silently stole/replaced the first finger's note instead of
	// adding a second one (a chord was never actually possible there before this).
	void setHeldNoteForSource(int sourceIndex, int note);
	void releaseAllTouchNotes(); // every source at once - octave change, losing the component, ...
	void changeOctave(int delta);
	void sendNote(int note, float velocity, bool on); // honours channel/midiRemap
	void releaseAllPcNotes();
	bool isPcKeyDownForNote(int note) const;
	// showContextMenu()'s own "Hold note" item - see its own comment. Independent of
	// heldNoteBySource/pcKeyDown (a note here keeps sounding regardless of what the mouse/touch/
	// PC keyboard are doing), so it needs no note-argument validation beyond >= 0: showContextMenu()
	// only ever offers this for a real key.
	void toggleHoldNote(int note);
	void releaseAllHeldNotes(); // every menu-held note at once - component destruction
	void timerCallback() override; // polls host.isNoteActive() for remote/incoming activity

	D110KeyboardHost &host;
	int octaveShift = 0;
	std::map<int, int> heldNoteBySource; // touch/mouse source index -> the note it's holding
	// Right-click menu's "Hold note" - a note in here keeps sounding until toggleHoldNote()
	// removes it again (menu toggle) or timerCallback() notices host.isNoteActive() has already
	// gone false on its own (MIDI PANIC, or anything else external that silenced it) and drops
	// it to keep the menu's own checkbox honest - see both their own comments.
	std::set<int> heldNotes;

	int midiChannel = 1;        // 1..16 - which channel injectTestNote() targets
	// When on, every note is forced onto midiChannel; when off, it broadcasts to all 16 at
	// once instead (named "Omni" until 2026-08-25 - see D110KeyboardHost.h's own comment on
	// why that name was dropped).
	bool midiRemap = false;
	bool pcKeyboardEnabled = false;
	PcLayout pcLayout = PcLayout::qwerty;
	// One flag per trackerKeys() entry, so keyStateChanged() (a single "something changed"
	// callback with no indication of which key) can diff against last-known state and fire
	// note-on/off only for the keys that actually moved. Polling juce::KeyPress rather than
	// overriding keyPressed(): that only fires on press, and a tracker needs release too.
	std::vector<bool> pcKeyDown;

	juce::Rectangle<float> captionBounds, octaveDownBounds, octaveUpBounds, keysBounds;
	std::vector<KeyRect> whiteKeys, blackKeys;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110Keyboard)
};
