#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "D110Keyboard.h"
#include "PluginProcessor.h"
#include "SoundbankBrowser.h"
#include "sequencer/D110SequencerGridPanel.h"
#include "sequencer/D110SequencerPanel.h"
#include "sequencer/D110SequencerRetroPanel.h"
#include "sequencer/SequencerViewMenu.h"

#include <array>
#include <vector>

// The front panel IS the reference photograph. docs/panel_reference.png is drawn as the
// background at 1:1 and every control is an invisible hit-region placed at coordinates measured
// off that photo (see docs/panel_reference_notes.md), so position and photorealism are both
// correct by construction rather than by hand-drawing.
//
// Only two things are ever painted over it:
//
//  * the LCD, replaced inside exactly the rectangle its window occupies in the photo. Its content
//    is the emulated MSM6222B's own rendered dot matrix, glyphs and all, so nothing here consults
//    a font - only the dot geometry and colours are ours (docs/lcd_reference.png);
//  * the MIDI MESSAGE lamp, the only indicator a D-110 has.
//
// Buttons and the VOLUME knob are never redrawn: the cap or the disc is cut straight out of the
// photograph, and the cut-out itself recedes into its recess or spins about its own axis.
//
// Emulator settings live on a right-click, never a control drawn onto the panel - the hardware
// has no such thing. Standalone-only items (Audio/MIDI Settings, save/load state, reset) are
// folded into the same menu too, now that the standalone window uses a native title bar and no
// longer has JUCE's own Options button to carry them (see D110AudioProcessorEditor's
// parentHierarchyChanged and D110Panel::showOptionsMenu).
class D110Panel : public juce::Component, private juce::Timer {
public:
	// Reference space = the photo's own pixels.
	static constexpr int kRefW = 2124;
	static constexpr int kRefH = 256;

	// Compact mode (Utility tab, "PANEL SIZE") splices sections out of the photograph entirely -
	// docs/panel_reference_compact.png, built by cutting [0,244) (the Roland wordmark and the
	// PHONES jack, both purely decorative - PHONES has no hit region at all, see
	// panel_reference_notes.md's "Decorative only") and [1560,1865) (the MEMORY CARD section)
	// out of panel_reference.png and rejoining the three remaining strips (Alan's request,
	// 2026-08-20, matching a mockup he supplied - "only the essentials": VOLUME, the LCD, the
	// button grid, POWER, the MIDI MESSAGE lamp). Every reference-space X coordinate used to
	// paint or hit-test something goes through mapX() below rather than being used raw, which
	// folds both cuts into one lookup: subtract kCompactLeftCutEnd always, and kCompactCardShift
	// on top of that for anything at or past the card section. currentRefW() is what every
	// window-sizing calculation in PluginEditor.cpp reads instead of the bare kRefW constant, so
	// the whole editor - window aspect ratio, zoom percent, drawer widths - narrows along with
	// the panel itself.
	//
	// Alan re-cropped the file himself afterwards (2026-08-20) to also trim the last 78px of the
	// photo's own right edge - the decorative right rack ear, past POWER/the MIDI MESSAGE lamp
	// (kBezelX+kBezelW=1998, kLampX+kLampW=1966, both comfortably clear) - so kCompactRefW is
	// the actual measured width of his file (cross-checked pixel-for-pixel against
	// panel_reference.png: [244,1560) + [1865,2046)), not just kRefW minus the two cuts above;
	// no mapX() change needed since nothing hit-tested/painted ever sat past x=2046.
	static constexpr float kCompactLeftCutEnd = 244.0f;
	static constexpr float kCompactCardCutStart = 1560.0f;
	static constexpr float kCompactCardCutEnd = 1865.0f;
	static constexpr float kCompactCardShift = kCompactCardCutEnd - kCompactCardCutStart;
	static constexpr int kCompactRefW = 1497;
	static int currentRefW(bool compact) { return compact ? kCompactRefW : kRefW; }
	static float mapX(float refX, bool compact) {
		if (!compact) return refX;
		float x = refX - kCompactLeftCutEnd;
		if (refX >= kCompactCardCutEnd) x -= kCompactCardShift;
		return x;
	}

	explicit D110Panel(D110AudioProcessor &);
	~D110Panel() override;

	void paint(juce::Graphics &) override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseDoubleClick(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

	// Click on the memory card slot. The card itself no longer belongs to the panel - it travels
	// over the whole window, drawer included, so it lives as its own component (D110MemoryCard).
	// The SLOT, however, is part of the photograph of the unit, so the panel is what hit-tests it.
	std::function<void()> onCardSlotClicked;
	// Options menu's Retro Sequencer toggle - the editor swaps which sequencer view is
	// visible in response (see D110AudioProcessorEditor's own resized()); this panel has
	// no reference to the sequencer drawer itself, hence the callback rather than a
	// direct call.
	std::function<void()> onSequencerModeChanged;
	// The slot's bezel, not the opening itself: hitting a strip thirty points tall with the mouse
	// is hard, and the bezel is exactly what a person sees as "the slot".
	static constexpr float kSlotHitX = 1588.0f, kSlotHitY = 109.0f;
	static constexpr float kSlotHitW = 260.0f, kSlotHitH = 52.0f;

	// Pulls from the machine everything the panel shows: the display, the lamp, the knob position,
	// the card's travel. Normally the panel's own timer does this; it is called separately where
	// there is no message loop - e.g. when shooting the panel to a file (plugin/editor_shot.cpp).
	void refreshFromInstrument() { timerCallback(); }

	// The window/reference-artwork ratio the editor is about to apply as a Component
	// transform. The LCD's offscreen render is supersampled relative to THIS, not to a
	// fixed reference-space multiplier, so the resample the transform then performs is
	// always a modest, fixed ratio - see rebuildLcdImage() for why that matters.
	void setDisplayScale(float scale);

	// Public so D110EditorPane's OPTIONS button (standalone only - see its
	// onOptionsButtonClicked) can reuse the exact same menu the panel's own right-click
	// already shows, content included, rather than keeping a second copy in sync.
	void showOptionsMenu();

private:
	// One front-panel cap as it sits in the photograph. `scanPort`/`scanBit` are this button's
	// position in the real 2x8 key-scan matrix, straight out of INPUT_PORTS_START(d110) in MAME's
	// src/mame/roland/roland_d10.cpp - these now close the actual switch in the running firmware.
	struct PanelButton {
		float x, y, w, h;
		const char *name;
		int scanPort;        // 0 = SC0 (top row), 1 = SC1 (bottom row)
		juce::uint8 scanBit;
	};

	struct ButtonMotion {
		bool held = false;    // mouse currently down on it
		bool latched = false; // double-clicked: stays held so combos are possible
		float depth = 0.0f;   // eased 0..1, how far the cap has sunk
	};

	enum class Drag { none, volume };

	void timerCallback() override;
	int buttonAt(juce::Point<float>) const; // index into kButtons, kPowerIndex, or -1
	void setButtonState(int index, bool down); // closes/opens the real scan-matrix switch

	juce::Image cutOut(juce::Rectangle<float>) const;
	juce::Colour recessColourOf(juce::Rectangle<float>) const;

	void rebuildLcdImage();
	void paintLcd(juce::Graphics &) const;
	void paintButton(juce::Graphics &, int index) const;
	void paintPowerSwitch(juce::Graphics &) const;
	void paintVolumeKnob(juce::Graphics &) const;
	void paintMidiLamp(juce::Graphics &) const;

	static juce::Rectangle<float> pressedRect(juce::Rectangle<float> face, float depth,
	                                          float shrink, float drop);
	static void paintPressedCap(juce::Graphics &, const juce::Image &cap, juce::Colour recess,
	                            juce::Rectangle<float> face, juce::Rectangle<float> dst, float depth);

	D110AudioProcessor &processor;

	juce::Image panelImage;
	juce::Image panelImageCompact; // see kCompactCutStart/End above

	juce::Image lcdImage;                    // offscreen dot-matrix render, rebuilt only on change
	float lcdDisplayScale = 1.0f;            // last scale passed to setDisplayScale()
	std::vector<juce::Image> capImages;      // one cut-out per button
	std::vector<juce::Colour> recessColours; // the recess each cap sinks into
	juce::Image powerCap;
	juce::Colour powerRecessColour;
	juce::Image volumeDisc;                  // the knob, lifted out to be spun about its own axis

	// The display exactly as the real MSM6222B renders it: one byte per dot row, bit 4
	// leftmost. No font table and no character codes are involved on this path - the
	// glyphs come from the controller's own mask CGROM inside the emulated machine.
	juce::uint8 lcdRows[D110CoreType::kLcdBytes] = {};
	bool lcdLive = false;

	static constexpr int kNumButtons = 16;
	static constexpr int kPowerIndex = -2;   // pseudo-index returned by buttonAt()
	static const PanelButton kButtons[kNumButtons];

	std::array<ButtonMotion, kNumButtons> motion;
	ButtonMotion powerMotion;

	float volumeDisplayed = -1.0f;           // eased towards the parameter, <0 = not yet initialised
	Drag drag = Drag::none;
	juce::Point<float> dragStart;
	float dragStartValue = 0.0f;

	D110AudioProcessor::LcdSnapshot lastSnapshot; // to skip repaints when nothing changed
	bool lastPowerOn = false;
	bool lcdInitialised = false;

	std::unique_ptr<juce::FileChooser> fileChooser;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110Panel)
};


// The M-256D memory card - a separate component lying ON TOP of the whole window.
//
// The panel used to draw it itself, so the card could only slide past the frame and vanish:
// the panel is the height of the unit, 256 points, and the card is 370. Now there is a drawer
// below the unit, and an ejected card has somewhere to lie. It slides out of the slot, comes
// to rest fully visible on the drawer and stays there; you can grab it with the left button
// and move it wherever is convenient.
//
// A component, rather than painting over the children, precisely for that: only what receives
// mouse events itself can be dragged, and the drawer under the card is a live component with
// its own fields. While the card sits in its socket it does NOT intercept the mouse - a click
// on the slot then goes to the panel, as before.
class D110MemoryCard : public juce::Component, private juce::Timer {
public:
	explicit D110MemoryCard(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;

	// Eject or insert - the same thing as clicking the slot on the unit.
	void toggle();
	void insert();
	// Whether it is inserted (by position, not by the firmware's opinion): the window owner needs
	// this to put the card back into its socket when the drawer is closed - it has nowhere to lie then.
	bool isOut() const { return target > 0.5f; }

	// Panel scale and full window height in reference points. Set by the window owner on every
	// resize: the card lives in the same reference points as the panel, so it travels with the
	// panel at any scale.
	void setGeometry(float panelScale, float totalRefHeight);

	// Ejecting asks for the drawer to be opened: the card comes to rest on it, and on a closed
	// drawer it simply has nowhere to be.
	std::function<void()> onEjectNeedsDrawer;

	// Geometry taken from the panel photograph (docs/panel_reference_notes.md): the slot opening
	// is at 1600, 120, sized 236 x 30. The card's width at the unit's true scale is the same 236
	// as the opening's width, and that coincidence doubles as a check of the scale.
	static constexpr float kCardX = 1600.0f;
	static constexpr float kCardWidth = 236.0f;
	static constexpr float kCardHeight = 370.0f;
	static constexpr float kSlotBottom = 150.0f;   // floor of the opening: below it the card is already outside
	static constexpr float kCardSeatedY = kSlotBottom - kCardHeight;   // end-on in the opening
	// Top of the clipping. The card does not hide behind the slot entirely: inserted, it stands
	// end-on in the opening with eighteen points of its edge visible - otherwise an occupied
	// socket would look no different from an empty one. Eighteen is about four millimetres at
	// panel scale (4.4 points per millimetre), i.e. the thickness of the card's shell at the grip.
	static constexpr float kCardClipTop = 132.0f;
	// Inside the opening the card is in shadow. The shadow is painted over it as a gradient rather
	// than baked into the picture: the card travels through the opening, so it is the place that
	// must darken, not the card.
	static constexpr float kSlotShadeAlpha = 0.58f;
	// Fraction of the path per frame at the fastest point of the travel; the motion law is in timerCallback.
	static constexpr float kCardStep = 0.032f;

private:
	void timerCallback() override;
	// Where the card goes when ejected: onto the drawer, below the handle strip and below the row
	// of tabs, so it does not cover them. This point is only the STARTING one: after that the
	// mouse decides.
	static constexpr float kRestX = kCardX;
	static constexpr float kRestY = 360.0f;
	juce::Point<float> position() const;   // top-left corner, in reference points
	void updateBounds();

	D110AudioProcessor &processor;
	juce::Image cardImage;

	// How far the card is out: 0 - sitting in the socket, 1 - lying on the drawer. Between those
	// two points it travels in a straight line, so the position is a blend of them rather than a
	// separate pair of coordinates: the motion law stays the one captured from the storyboard.
	float travel = 0.0f;
	float target = 0.0f;
	juce::Point<float> rest{ kRestX, kRestY };

	float scale = 1.0f;
	float totalRefH = 1190.0f;

	bool dragging = false;
	juce::Point<float> dragGrab;   // where exactly the card was grabbed, in reference points

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110MemoryCard)
};

// The minimal on-screen test keyboard (D110Keyboard, see D110Keyboard.h) sits under its
// own handle band here, foldable like the extended editor's drawer but by a separate
// action, and open by default since it's the most direct way to hear the instrument
// without any MIDI cabling. In this plugin it reaches the firmware through
// D110AudioProcessor::injectTestNote(), which hands notes to the same collector
// (osMidiCollector) that handleIncomingMidiMessage(MidiInput*, ...) does.

// The extended editor - a drawer sliding out from under the unit.
//
// It is DRAWN IN CODE, a deliberate departure from the panel itself: the panel is a photo
// composite because it depicts an existing thing, whereas the D-110 has no editor at all. The
// unit has a two-line, sixteen-character display for everything, and reaching the fifty-eight
// values of a partial through it takes dozens of presses; the drawer shows them all at once.
//
// No field touches the sound engine directly. An edit goes to the UNIT as an exclusive
// message - the same way an external editor would send it to a real D-110 - the firmware
// changes its own memory, and the mirror carries that to the engine. So an edit made here
// and an edit made on the panel are one and the same event, and both show up in both places.
//
// Every address this pane writes to has been MEASURED, not taken from the manual on trust:
// plugin/editor_write_probe.cpp sends one write into each area and watches which byte of the
// battery-backed RAM moves.
class D110EditorPane : public juce::Component, private juce::Timer {
public:
	explicit D110EditorPane(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseMove(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
	// Arrow-key navigation: TONES tab walks the 64-slot grid the same way a click does
	// (select + audition); PATCHES tab's PARTS OF PATCH nudges whichever cell was last
	// clicked up/down the way the mouse wheel already does. Returns false for anything else
	// so it bubbles up to D110AudioProcessorEditor::keyPressed() unchanged (retro sequencer
	// D-pad, etc.) - see that method's own comment on the bubbling.
	bool keyPressed(const juce::KeyPress &) override;

	// Re-reads the unit's memory. Normally the timer does this; it is called separately by the
	// panel-to-file snapshot, which has no message loop to run.
	void refreshFromInstrument();
	void selectTab(int index);

	// Utility tab's WINDOW SIZE control (a percentage of the reference width, e.g. 100) - set
	// by the owning D110AudioProcessorEditor, which is the one that actually knows how to
	// resize itself. A plain callback rather than this pane reaching upward through some
	// back-reference, matching how panel.onCardSlotClicked/card.onEjectNeedsDrawer are already
	// wired in D110AudioProcessorEditor's own constructor.
	//
	// Exists instead of a maximise button: on the owner's own window manager, several attempts
	// at correcting what a native maximise actually does on the wire each traded one
	// window-manager-specific bug for another that couldn't be reproduced or verified from
	// here. A percentage resize is exactly the same setSize() call a manual drag-resize
	// already makes reliably, just computed from a chosen number instead of a mouse
	// position - no window-manager or monitor-geometry involvement at all.
	std::function<void(int percent)> onRequestZoom;

	// Utility tab's THEME toggle flips a process-wide d110ui::Theme (see UiTheme.h) that
	// every custom-drawn drawer's paint() reads on its own - this callback exists only to
	// tell the owner to repaint them all immediately rather than waiting for whichever one
	// happens to redraw next.
	std::function<void()> onThemeChanged;

	// Utility tab's FONT SIZE toggle - fired after processor.getUiFontScaleBig() has already
	// flipped. Only D110AudioProcessorEditor can act on it (juce::Desktop::getInstance().
	// setGlobalScaleFactor() is process-wide, Standalone-only - see UiTheme.h's own comment).
	std::function<void()> onFontScaleChanged;

	// Utility tab's SEQUENCER toggle mirrors D110Panel's own right-click Options entry (same
	// processor.getSequencerRetroMode() flag) - this callback exists so the owner can swap
	// which sequencer drawer view is visible, same as D110Panel::onSequencerModeChanged does
	// for the right-click path.
	std::function<void()> onSequencerModeChanged;

	// Utility tab's PANEL SIZE toggle (processor.getCompactPanelMode()) - fired after the flag
	// itself has already flipped, same "just tell the owner" shape as onSequencerModeChanged,
	// except this one also changes the window's own width/aspect ratio, which only
	// D110AudioProcessorEditor (holding getWidth()/setSize()/the panel/the constrainer) can do -
	// see its own wiring for the actual resize math.
	std::function<void()> onCompactPanelModeChanged;

	// OPTIONS button (standalone only - see optionsButtonBounds below) - the owner wires this
	// to D110Panel::showOptionsMenu() so it's the exact same menu as the panel's own
	// right-click, content included, rather than a second copy of it living here too.
	std::function<void()> onOptionsButtonClicked;

private:
	void timerCallback() override;
	// Holds recently sent, not-yet-confirmed edits on top of a freshly read ram - see the comment
	// at PendingEdit. Called right after every getRam() in refreshFromInstrument(), both places.
	void reapplyPendingEdits();

	// Where a field writes. The areas are Roland's own, and each one's address in the firmware's
	// RAM has been measured; see D110Core.
	enum class Area { TimbreTemp, ToneTemp, Rhythm, System, Timbres, Patches, Tones };

	struct Cell {
		juce::Rectangle<float> bounds;
		Area area = Area::TimbreTemp;
		int index = 0;   // part, key, memory slot, or offset within the System area
		int field = 0;   // offset within the record; unused for System
		int lo = 0, hi = 127;
	};

	// Right-click on TONE GROUP/TONE - a list of all tones (a/b/i/r, 64 in each) instead of
	// stepping through them one by one with the wheel. Shared between the live part area (Parts)
	// and the patch record (PARTS OF PATCH inside Patches) - both have the same group+number byte
	// pair, only where to send it (index) and the record offset (groupField - the address of the
	// group byte; the number is the next byte) differ.
	void showToneListMenu(Area area, int index, int groupField);
	// Right-click on DRUM SOUND on the Rhythm tab - a list of all rhythm timbres and memory
	// timbres, reading their names the same way showToneListMenu does.
	void showRhythmSoundMenu(int slot);
	// Right-click on a partial's PCM field (Tone tab) - a list of all 128 ROM samples by name,
	// instead of stepping through the number with the wheel. Github issue #2.
	void showPcmWaveMenu(const Cell &pcmCell);

	enum class Tab { Parts, Tone, Rhythm, Patches, Timbres, Tones, System, Monitor, Soundbanks, Utility };
	static constexpr int kNumTabs = 10;

	// The PATCHES tab's own two views, switched by a small sub-tab strip under the main
	// one - the 64-patch list and the 8-part breakdown of whichever one is selected used to
	// share the tab as a fixed 58/42 vertical split; shrinking the drawer past a certain
	// height clipped the bottom of whichever section didn't have its own scroll (only the
	// patch list does, via patchScroll). Splitting into two full-height sub-tabs instead
	// means neither view is ever squeezed below what it needs, whatever the drawer's height.
	enum class PatchesSubTab { AllPatches, PartsOfPatch };

	// One partial parameter: caption, offset within its 58-byte record, and limit.
	struct ToneParam {
		const char *name;
		int offset;
		int hi;
	};
	// The four Tone Edit detail columns (WG/pitch-env, TVF, TVA) - together the whole 58-byte
	// partial record. Defined in the .cpp, right above layoutTone(); randomizeTone() below
	// also walks them.
	static const ToneParam kWg[];
	static const ToneParam kPitchEnv[];
	static const ToneParam kTvf[];
	static const ToneParam kTva[];

	struct Button {
		juce::Rectangle<float> bounds;
		juce::String text;
		int id = 0;
	};

	// The caption, computed together with the fields. They are kept in one list so that the
	// column header and the column itself are never computed twice by different formulas and
	// drift apart one day.
	struct Label {
		juce::Rectangle<float> bounds;
		juce::String text;
		bool heading = true;
		juce::Justification just = juce::Justification::centredLeft;
	};

	void layout();
	void layoutParts(juce::Rectangle<float> area);
	void layoutTone(juce::Rectangle<float> area);
	void layoutRhythm(juce::Rectangle<float> area);
	void layoutPatches(juce::Rectangle<float> area);
	void layoutPatchesList(juce::Rectangle<float> area);
	void layoutPatchesParts(juce::Rectangle<float> area);
	void layoutTimbres(juce::Rectangle<float> area);
	void layoutTones(juce::Rectangle<float> area);
	void layoutSystem(juce::Rectangle<float> area);
	void layoutUtility(juce::Rectangle<float> area);
	void layoutParamColumn(juce::Rectangle<float> column, int partialBase,
	                       const ToneParam *params, int count);
	void paintMonitor(juce::Graphics &, juce::Rectangle<float> area);
	// Tone tab's DEGRADE/RANDOM buttons - see the .cpp for the actual weighting. fullyRandom
	// picks each field completely fresh (RANDOM); otherwise it nudges a minority of fields by
	// a small amount each, leaving the rest untouched (DEGRADE).
	void randomizeTone(bool fullyRandom);
	// This drawer's own label/value font size, relative to how it looked at the app's default
	// window size - see the .cpp for why this isn't simply getWidth()/1500.
	float fontScale() const;
	// PARTIAL MUTE (Tone tab, field 12) - four per-partial ON/OFF toggles instead of the raw
	// 0-15 bitmask a generic Cell would show. Github issue #1: cycling a 16-value drag/wheel
	// field to find one bit was impractical. Still a genuine Cell/byte underneath (see
	// setValue's ToneTemp case) - only the paint/hit-test are special-cased.
	void paintPartialMuteCell(juce::Graphics &, const Cell &, bool hover) const;
	bool isPartialMuteCell(const Cell &) const;
	juce::Rectangle<float> partialMuteSegment(const Cell &, int partial) const;
	void buttonPressed(int id);
	// showLaReferencePopup() - shows docs/D20infos.png (embedded via D110PanelData/BinaryData)
	// in its own pop-up window - is now a free function in PluginEditor.cpp's own anonymous
	// namespace, not a member: D110Panel::showOptionsMenu() needed to call it too (Github
	// issue #3), and neither component owns the other.

	int cellAt(juce::Point<float>) const;
	size_t addressOf(const Cell &) const;
	int valueOf(const Cell &) const;
	void setValue(const Cell &, int value);
	juce::String textOf(const Cell &) const;
	// The name from the unit's memory: ten characters, as the display shows them.
	juce::String nameAt(size_t ramOffset) const;
	// A tone's name from its (group, number) pair. The preset groups and rhythm live in ROM, so
	// their names come from the sound engine, which loaded the same ROM; internal tones come from
	// the firmware's own memory.
	juce::String toneName(int group, int number) const;

	D110AudioProcessor &processor;

	Tab tab = Tab::Parts;
	std::array<juce::Rectangle<float>, kNumTabs> tabBounds{};
	// Standalone only, sits in the leftover space right of the tab row (empty in the plugin
	// builds, and in standalone whenever the window's too narrow for it to fit) - fires
	// onOptionsButtonClicked, somewhere visible instead of needing to know the panel's
	// right-click menu exists.
	juce::Rectangle<float> optionsButtonBounds{};

	// Whose records the tabs that have a "current part" show.
	int part = 0;
	std::array<juce::Rectangle<float>, 8> partBounds{};
	juce::Rectangle<float> toneNameBounds;
	// Utility tab's WINDOW SIZE button - stored separately from the generic `buttons` list
	// (which only dispatches left-clicks) because right-click on it also needs handling, in
	// mouseDown()'s popup-menu branch.
	juce::Rectangle<float> zoomBounds;
	juce::Rectangle<float> romFolderBounds;
	juce::Rectangle<float> soundbankFolderBounds;
	int tonePartial = 0;
	std::array<juce::Rectangle<float>, 4> tonePartialBounds{};
	// Tone tab's LOCK PARTIALS toggle - while on, editing a Partial 1 field also sets the same
	// field, to the same value, on Partials 2-4 (same feature/wording as ~/src/D110/edisyn's
	// RolandD110Tone "Lock Partials" checkbox). Editor-local UI state, not sent to the
	// instrument or saved - matches tonePartial/part above. See setValue()'s own comment for
	// the propagation itself.
	bool lockPartials = false;
	// Set around randomizeTone()'s own loop so its setValue() calls skip the lockPartials
	// propagation above - each of the 4 partials gets its own independent DEGRADE/RANDOM
	// result even while locked, matching Edisyn's own choice (its Randomize/Mutate never goes
	// through the lock hook either, only a value the user dials in by hand does).
	bool suppressPartialLock = false;

	// Long lists scroll with the wheel when it is not over a field.
	int rhythmScroll = 0;
	int timbreScroll = 0;
	int patchScroll = 0;
	int toneScroll = 0;
	int patchSlot = 0;    // patch whose parts are shown on the PARTS OF PATCH sub-tab
	int toneSlot = 0;     // selected tone memory slot
	// Row count of the TONES grid's own 3 columns, as last computed by layoutTones() - kept
	// around so keyPressed() can walk the grid without re-deriving it from tableArea/rowHeight.
	int toneRows = 1;
	// PATCHES tab's PARTS OF PATCH: index into `cells` of whichever field was last clicked,
	// so Up/Down arrow keys can nudge it the same way the mouse wheel already does. -1 means
	// nothing focused (cleared whenever a click misses every cell). Stable across repaints/
	// timer refreshes since those never call layout() (which is what rebuilds `cells`).
	int focusedPatchCell = -1;

	// See PatchesSubTab's own comment.
	PatchesSubTab patchesSubTab = PatchesSubTab::AllPatches;
	std::array<juce::Rectangle<float>, 2> patchesSubTabBounds{};

	// UTILITY's own scroll - unlike the other tabs (which page a fixed-size row list), this
	// one stacks sections of unequal, growing height, so it scrolls in raw pixels rather than
	// row units. utilityContentHeight is measured as a side effect of layoutUtility() itself
	// (the space its sections actually consumed, regardless of scroll position); the two
	// rectangles are the track/thumb hit regions layoutUtility() leaves behind for
	// mouseDown/mouseDrag, empty when everything already fits without scrolling.
	// The same pixel scroll also serves the fixed-layout tabs whose content can run taller than the
	// drawer when the window is short (PARTS, TONE, SYSTEM, PARTS OF PATCH): they are laid out in
	// a tall virtual rectangle (see layout()) and the result is shifted up by this offset. It
	// resets to 0 whenever the tab or sub-tab changes.
	float utilityScrollOffset = 0.0f;
	float utilityContentHeight = 0.0f;
	juce::Rectangle<float> utilityScrollTrack, utilityScrollThumb;
	bool draggingUtilityScroll = false;
	float utilityScrollDragStartY = 0.0f, utilityScrollDragStartOffset = 0.0f;

	// True for the tabs scrolled in raw pixels through utilityScrollOffset: UTILITY itself and the
	// fixed-layout tabs listed above.
	bool pixelScrollTab() const;
	bool usesPixelScroll() const { return tab == Tab::Utility || pixelScrollTab(); }

	// The row-paged tabs (RHYTHM, ALL PATCHES, TIMBRES, TONES) scroll a window of rows with the wheel;
	// this adds a visible scrollbar for the same variable. rowScrollVar is set by the tab's layout
	// (null = nothing to scroll), the rest is derived in layout().
	int *rowScrollVar = nullptr;
	int rowScrollMax = 0;      // highest value of *rowScrollVar
	int rowScrollVisible = 1;  // how many units one screen shows (the page size)
	juce::Rectangle<float> rowScrollTrack, rowScrollThumb;
	bool draggingRowScroll = false;
	float rowScrollDragStartY = 0.0f;
	int rowScrollDragStartValue = 0;

	juce::Rectangle<float> tableArea;
	juce::Rectangle<float> contentArea;   // for tabs that are painted as a whole
	float rowHeight = 0.0f;

	// SOUNDBANKS tab's whole content - a real child Component (unlike every other tab, which
	// paints from cells/labels/buttons above) since it's the exact same shared component
	// Android's own hamburger menu swaps in - see SoundbankBrowser.h's own comment.
	SoundbankBrowser soundbankBrowser;

	std::vector<Label> labels;
	std::vector<Cell> cells;
	std::vector<Button> buttons;

	// One text entry field for the whole editor: every name and display message is typed into it.
	juce::TextEditor textEntry;
	int textEntryTarget = 0;   // 0 - nowhere, 1 - the part's tone name, 2 - a display message,
	                           // 3 - a patch name, 4 - a tone memory name
	int textEntryButton = -1;  // the button the entry field is standing in for
	int hovered = -1, dragging = -1;
	float dragStartY = 0.0f;
	int dragStartValue = 0;

	// Snapshot of the unit's memory, refreshed on a timer: the editor always shows what is really
	// in the unit, including edits made from its own panel.
	std::vector<uint8_t> ram;
	uint64_t ramGen = 0;
	bool ramValid = false;

	// Edits sent by the user and not yet confirmed by a real answer from the unit. setValue()
	// puts the optimistic value into `ram` at once so the field under the cursor does not lag -
	// but the edit reaches the firmware by the same path as a note, with the same real latency
	// (0-18 ms measured today), and the firmware's memory keeps changing for unrelated reasons
	// (cursor blink, MIDI lamp), each such change bumping the global generation counter. Without
	// this list the next timer tick re-reads ALL of memory and wipes an edit the firmware has not
	// accepted yet back to its real value - the jitter on fast wheel scrolling is exactly that.
	// The list holds the optimistic value until the firmware itself confirms it or the wait
	// times out.
	struct PendingEdit { size_t address; uint8_t value; juce::int64 sentMs; };
	std::vector<PendingEdit> pendingEdits;

	// ROM tone names are asked of the engine once each: they never change, and reading them on
	// every repaint would mean reaching into another thread ten times a second.
	mutable std::array<juce::String, 4 * 64> romToneNames;
	mutable std::array<bool, 4 * 64> romToneNameKnown{};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110EditorPane)
};

// The unit, the drawer and the handle strip between them.
//
// The drawer opens DOWNWARD, like on the other synths in this series: the unit itself stays
// whole and the editor slides out from under it. The handle is full-width so it reads as a
// drawer rather than a button, and sits BELOW the photograph, not on it: the unit's front has,
// and can have, no controls the hardware does not.
class D110AudioProcessorEditor : public juce::AudioProcessorEditor {
public:
	explicit D110AudioProcessorEditor(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseMove(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	// Forwards to the retro sequencer's own D-pad navigation when it doesn't otherwise reach
	// it - see the .cpp for why this is needed (D110Keyboard grabs keyboard focus on every
	// click, so a physical EXIT/ENTER/arrow press after playing a note on it would otherwise
	// silently do nothing).
	bool keyPressed(const juce::KeyPress &) override;
	// Standalone only: switches the plugin wrapper's window from JUCE's own custom-drawn
	// title bar to the OS's native one, to match Nonet Sequencer's window (Alan's request) -
	// see the .cpp for why this is done here rather than at window construction.
	void parentHierarchyChanged() override;
	// Standalone only: sets the window/taskbar icon - see the .cpp.
	void applyWindowIcon();

	// Re-read the unit with both halves at once - panel and drawer. Needed by the whole-editor
	// snapshot, which has neither a window nor a message loop to run their timers.
	void refreshFromInstrument() {
		panel.refreshFromInstrument();
		editorPane.refreshFromInstrument();
	}
	// Open the drawer without the mouse - by that same snapshot.
	void setExpanded(bool open) { expansion = expansionTarget = open ? 1.0f : 0.0f; }
	// Same, for the sequencer drawer - closed by default in normal use, but editor_shot's
	// whole-editor snapshot wants to show it open, the way it already does for the others.
	void setSequencerExpanded(bool open) { sequencerExpansion = sequencerExpansionTarget = open ? 1.0f : 0.0f; }

	// Height of the handle strip in panel reference points.
	static constexpr float kHandleRefH = 34.0f;
	// Default/initial height of the drawer, in the same reference units - what a new project
	// (or one saved before this was adjustable) opens with. Used to be chosen so the UTILITY
	// tab fit entirely without a scrollbar - but its list of sections only grows, and resizing
	// the WHOLE drawer's (and so the whole window's) height for every new section isn't the
	// right trade-off. UTILITY now has its own scrollbar instead (layoutUtility()'s
	// utilityScrollOffset), so this only needs to stay comfortable for the nine-part table
	// (the most demanding of the other tabs) - about 530 points at the usual window width
	// (1500 points, scale 0.71). The actual live height is editorPaneRefH below, which starts
	// at this default but can be dragged - see the KEYBOARD handle band's dual role in
	// mouseDown/mouseDrag.
	static constexpr float kPaneRefH = 750.0f;
	static constexpr float kMinPaneRefH = 260.0f;
	static constexpr float kMaxPaneRefH = 1600.0f;
	// Handle band above the test keyboard - slimmer than the editor's own, in keeping with
	// the keyboard being the minimal add-on rather than the main drawer. Doubles as the
	// keyboard pane's own resize handle exactly the way this band doubles for the editor
	// pane above it - see keyboardPaneRefH below.
	static constexpr float kKeyboardHandleRefH = 26.0f;
	static constexpr float kMinKeyboardPaneRefH = 70.0f;
	static constexpr float kMaxKeyboardPaneRefH = 400.0f;
	// Handle band above the sequencer drawer - same slim treatment as the keyboard's, and the
	// same dual role for the KEYBOARD pane above it.
	static constexpr float kSequencerHandleRefH = 26.0f;
	// The sequencer drawer is the last one, with no further drawer below it to lend it a handle
	// band - so it gets its own thin resize-only grip instead, right under it (Alan asked for
	// this 2026-08-19: the drawer "n'est pas très haute" and had no way to grow at all before).
	static constexpr float kSequencerResizeGripRefH = 10.0f;
	static constexpr float kMinSequencerPaneRefH = 200.0f;
	static constexpr float kMaxSequencerPaneRefH = 1400.0f;
	// Only relevant in a VST3/AU build (see kSequencerResizeGripRefH's own comment above) -
	// the sequencer drawer is Standalone/Android only there (2026-09-04, Alan's request: a DAW
	// host already comes with its own sequencer), which makes the keyboard the LAST drawer and
	// gives it the same "resize-only grip below it" need the sequencer used to cover for it via
	// the dual-role SEQUENCER handle band.
	static constexpr float kKeyboardResizeGripRefH = 10.0f;

private:
	// Shown once from the constructor when no ROMs were found, so a fresh install can point
	// at a folder right away instead of having to first find the Utility tab inside the
	// extended editor drawer - see its own .cpp comment.
	void showRomSetupDialog();

	float totalRefHeight() const;
	void applySize();
	juce::Rectangle<float> handleBand() const;
	juce::Rectangle<float> keyboardHandleBand() const;
	juce::Rectangle<float> sequencerHandleBand() const;
	juce::Rectangle<float> sequencerResizeBand() const;
	// VST3/AU only - see kKeyboardResizeGripRefH.
	juce::Rectangle<float> keyboardResizeBand() const;

	// Needed directly (not just by the child components below, which each keep their own
	// reference) for setEditorPaneRefH() - see mouseUp()'s use of it.
	D110AudioProcessor &processor;

	D110Panel panel;
	D110EditorPane editorPane;
	// The card is added AFTER the drawer and so lies on top of it - otherwise an ejected card
	// would hide behind the editor's fields.
	D110MemoryCard card;
	// Stacked below the extended editor's own drawer, with its own independent fold state -
	// see D110Keyboard's header comment for why.
	D110Keyboard keyboard;
	// Stacked below the keyboard, third drawer down, same independent-fold treatment.
	D110SequencerPanel sequencerPanel;
	// D-20-style alternate view of the same drawer - see processor.getSequencerRetroMode()
	// and D110Panel::onSequencerModeChanged below. Both are always constructed (cheap,
	// stateless views over the same host/engine); resized() shows exactly one of the views.
	D110SequencerRetroPanel sequencerRetroPanel;
	// Piano-roll grid editor, the third view of the same drawer - see processor.getSequencerGridMode().
	D110SequencerGridPanel sequencerGridPanel;
	juce::ComponentBoundsConstrainer constrainer;

	float expansion = 0.0f;        // smoothed 0..1
	float expansionTarget = 0.0f;  // what the click on the handle asked for
	bool handleHover = false;

	// The editor pane's own live height (see kPaneRefH's comment) - adjustable by dragging
	// the KEYBOARD handle band, which is the boundary directly below it. Persisted through
	// D110AudioProcessor::get/setEditorPaneRefH the same way the WINDOW SIZE/THEME choices
	// are, so a resized drawer stays resized across sessions.
	float editorPaneRefH = kPaneRefH;
	// Set on mouseDown over the KEYBOARD handle band, before it's known whether this turns
	// into a resize-drag or stays a plain click; resolved in mouseUp (toggle) or mouseDrag
	// (resize, once the pointer has moved past a small threshold - see mouseDrag()). Same
	// resizeDragStartY/resizeDragStartRefH pair is reused for all three resize gestures below -
	// only one can ever be in progress at a time, so which resizingXxx flag is set says which.
	bool keyboardHandlePressed = false;
	bool resizingEditorPane = false;
	float resizeDragStartY = 0.0f;
	float resizeDragStartRefH = 0.0f;

	float keyboardExpansion = 1.0f;       // eased 0..1, open by default
	float keyboardExpansionTarget = 1.0f;
	bool keyboardHandleHover = false;

	// The keyboard pane's own live height - same idea as editorPaneRefH, adjustable by
	// dragging the SEQUENCER handle band (the boundary directly below it), the same dual-role
	// trick keyboardHandleBand already uses for the editor pane. Persisted through
	// D110AudioProcessor::get/setKeyboardPaneRefH.
	float keyboardPaneRefH = D110Keyboard::kRefH;
	// Mirrors keyboardHandlePressed/resizingEditorPane above, for this second dual-role band.
	bool sequencerHandlePressed = false;
	bool resizingKeyboardPane = false;

	// Closed by default, unlike the keyboard: a bigger, more specialised drawer, better as
	// an opt-in reveal than something that greets every session already open.
	float sequencerExpansion = 0.0f;
	float sequencerExpansionTarget = 0.0f;
	bool sequencerHandleHover = false;

	// The sequencer pane's own live height - same idea again, adjustable by dragging its own
	// resize grip (sequencerResizeBand(), below the drawer - there's no further drawer there to
	// double up on, unlike the two above). Persisted through
	// D110AudioProcessor::get/setSequencerPaneRefH.
	float sequencerPaneRefH = D110SequencerPanel::kRefH;
	bool sequencerResizeHandlePressed = false;
	bool sequencerResizeHover = false;

	// VST3/AU only (see kKeyboardResizeGripRefH) - mirrors sequencerResizeHandlePressed/Hover,
	// but resizes keyboardPaneRefH instead: the keyboard is the last drawer there.
	bool keyboardResizeHandlePressed = false;
	bool keyboardResizeHover = false;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessorEditor)
};
