#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "D110SequencerEngine.h"
#include "D110SequencerHost.h"

// Piano-roll / step-grid view of the sequencer, the third one alongside D110SequencerPanel
// (mouse-driven strip) and D110SequencerRetroPanel (D-20 style LCD). One track and one bar at
// a time: pitches down the side (with a piano-key column that auditions), steps across
// (16 per bar in 4/4 at the default 1/16 grid, LMMS style), a velocity lane underneath. Same
// host contract as the other two, every edit goes through the engine's own note primitives
// (D110SequencerEngine::addNote()/updateNoteEvent()/deleteNoteEvent()) with one undo
// checkpoint per gesture, so UNDO/REDO behave like everywhere else. The view bar IS the
// engine's current bar (gotoBar()), so LOOP: BAR loops what's on screen and playback pages
// the view along by itself.
//
// Mouse model: click an empty cell to add a note (the last used length, dragging right while
// still held stretches it); click a note without moving to remove it; drag its body to move
// it (step + pitch), its right edge to resize; right-click deletes; in the velocity lane
// drag a note's stick up/down. The wheel and the scrollbar scroll the pitch range.
class D110SequencerGridPanel : public juce::Component, private juce::Timer, private juce::ScrollBar::Listener {
public:
	explicit D110SequencerGridPanel(D110SequencerHost &);
	~D110SequencerGridPanel() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

	// Same reference footprint as the other two views, so the owning editor/window can size
	// the drawer the same way whichever one is showing.
	static constexpr float kRefH = 347.0f;

	// A host with nowhere else to put its own menu (the Android app hides its hamburger while
	// a sequencer view shows) sets this: a bar-menu button then appears in the transport row's
	// last column, exactly where D110SequencerPanel has its own, holding "first/last bar" and
	// whatever this callback appends. Unset (the desktop) there is no such button.
	std::function<void(juce::PopupMenu &)> onBarMenuButtonExtra;

	// Android app only - see D110SequencerPanel::onModeButton.
	std::function<void()> onModeButton;
	std::function<int()> modeButtonMode; // d110ui::AppMode as int, for the icon

private:
	void timerCallback() override;
	void scrollBarMoved(juce::ScrollBar *, double newRangeStart) override;

	d110seq::D110SequencerEngine &engine() const;

	struct Button {
		juce::Rectangle<float> bounds;
		std::function<juce::String()> label;
		std::function<bool()> active;  // null = never highlighted
		std::function<bool()> enabled; // null = always enabled
		std::function<void()> onClick;
		std::function<void()> onRightClick; // null = none
		std::function<void(juce::Graphics &, juce::Rectangle<float>)> customPaint; // null = text label
	};

	void buildLayout();
	// Pixel height of one pitch row (the ROW button, Ctrl+wheel, and the host's persisted
	// choice all end up here). Touch hosts want it much taller than the mouse default; the
	// piano-key/scrollbar/velocity-lane sizes and the hit tolerances follow it.
	void setRowHeight(float newRowH, bool persist);
	void cycleRowHeight();
	void syncRowHeightFromHost();
	void showBarMenu();
	// Touchscreens have no right button: holding a transport button ~500ms runs its right-click
	// action too, same idea as D110SequencerPanel's long press.
	void armLongPress(std::function<void()> action, juce::Point<float> at);
	void addButton(juce::Rectangle<float> bounds, std::function<juce::String()> label, std::function<void()> onClick,
	               std::function<bool()> active = {}, std::function<bool()> enabled = {},
	               std::function<void()> onRightClick = {});

	// ---- grid geometry, all derived from the engine's live bar length + the chosen step size
	struct Geometry {
		int bar = 1;
		double barStart = 0.0;    // absolute beat of this bar's first step
		double barLen = 4.0;      // beats
		double stepBeats = 0.25;  // one step, in beats
		int columns = 16;         // drawn columns (rounded up)
		int validSteps = 16;      // steps that actually fall inside the bar (rounded down)
		int stepsPerBeat = 4;     // for the heavier beat lines
		float colW = 1.0f;
		// GRID OFF: notes are placed/moved/resized at any position (rounded only to the 960
		// ticks per beat the MIDI export uses), the step columns stay as a drawn guide.
		bool free = false;
	};
	Geometry geometry() const;
	// Absolute beat of a step boundary. Every timestamp the editor writes is built with this
	// one expression, so a note ending on step k and another starting on step k get
	// bit-identical times (see D110SequencerEngine::addNote()).
	static double stepTime(const Geometry &g, int step) { return g.barStart + double(step) * g.stepBeats; }

	int stepAtX(float x, const Geometry &) const; // clamped to [0, columns - 1]
	double beatAtX(float x, const Geometry &) const; // unsnapped, absolute beats
	static double roundToTick(double beat) { return std::round(beat * 960.0) / 960.0; }
	static constexpr double kMinFreeLen = 1.0 / 32.0; // shortest note GRID OFF will make, in beats
	float xForBeat(double beat, const Geometry &) const;
	int noteAtY(float y) const;  // MIDI note under a y in the grid area, may be out of 0-127
	float yForNote(int note) const;
	int visibleRows() const;
	void setTopNote(int topNote);
	void ensureNotesVisible(bool force);

	using NoteInfo = d110seq::D110SequencerEngine::NoteEventInfo;
	// Notes of the selected track touching this bar or the one after it: the ones that start
	// in the bar, the ones that started earlier and are still held into it (ghost notes, drawn
	// but not editable from this bar - see isEditable()), and next-bar notes, which only matter
	// for same-pitch overlap checks. isInBar() picks what is actually drawn/hit-tested.
	std::vector<NoteInfo> notesNear(const Geometry &) const;
	static double noteEnd(const NoteInfo &n, const Geometry &g) {
		return n.startBeat + (n.durationBeats > 0.0 ? n.durationBeats : g.stepBeats);
	}
	static bool isInBar(const NoteInfo &n, const Geometry &g) {
		return n.startBeat < g.barStart + g.barLen - 1.0e-9 && noteEnd(n, g) > g.barStart + 1.0e-9;
	}
	static bool isEditable(const NoteInfo &n, const Geometry &g) {
		return isInBar(n, g) && n.startBeat >= g.barStart - 1.0e-9;
	}
	juce::Rectangle<float> noteRect(const NoteInfo &, const Geometry &) const;
	// End time of a note of `lenSteps` steps starting at `start`: built from step boundaries
	// when `start` sits on one (see stepTime()), plain start + length otherwise.
	double endForLength(const Geometry &, double start, int lenSteps) const;
	// The gesture's note, looked up again in a fresh notesNear() - false (and the gesture
	// dropped) if something else edited the track meanwhile and the index no longer points at it.
	bool gestureNoteStillValid(const std::vector<NoteInfo> &);
	void resizeGestureNoteTo(float x);

	struct Hit {
		int listIndex = -1; // into the vector notesForBar() returned
		bool onRightEdge = false;
	};
	Hit hitNote(const std::vector<NoteInfo> &, const Geometry &, juce::Point<float>) const;
	int hitVelocityStick(const std::vector<NoteInfo> &, const Geometry &, juce::Point<float>) const;
	// True if [start, end) at this pitch would overlap another note on the track (ignoring the
	// one whose engine index is `ignoreIndex`) - same-pitch overlaps would cut each other short
	// on playback, so the editor never creates one.
	bool overlapsSamePitch(const std::vector<NoteInfo> &, int ignoreIndex, int note, double start, double end) const;
	// Start of the next same-pitch note strictly after `start`, or `limit` if none/later.
	double nextSamePitchStart(const std::vector<NoteInfo> &, int ignoreIndex, int note, double start,
	                          double limit) const;

	void startAudition(int note, int velocity);
	void stopAudition();

	void showGridMenu();
	void showLengthMenu();
	void showVelocityMenu();
	void promptForTempo();
	void cycleTimeSignature();
	void showTimeSignatureMenu();
	void selectTrack(int track);

	D110SequencerHost &processor;

	// ---- view state (UI only, not persisted)
	int selTrack = 0;
	d110seq::QuantizeGrid gridChoice = d110seq::QuantizeGrid::sixteenth;
	int noteLenSteps = 1; // length of the next note a click creates - follows the last edit
	int defaultVelocity = 100;
	int topNote = 79;     // highest pitch row currently visible
	bool viewInitialised = false;
	int lastBar = -1;
	float rowH = 14.0f;           // see setRowHeight()
	bool stackedTransport = false; // Android portrait two-row transport, see buildLayout()
	bool builtWithModeButton = false;
	bool builtWithBarMenu = false; // whether the bar-menu button existed when the layout was last built
	int longPressToken = 0;
	juce::Point<float> longPressStart;
	int builtTrackCount = 0; // activeTrackCount() the track buttons were last laid out for

	std::vector<Button> buttons;
	juce::Rectangle<float> tempoBounds, barReadoutBounds, infoBounds, hintBounds;
	juce::Rectangle<float> keysBounds, gridBounds, velBounds;
	std::array<juce::Rectangle<float>, d110seq::D110SequencerEngine::kNumSongSlots> slotBounds;
	juce::ScrollBar vBar{true};

	// ---- mouse gesture in progress
	enum class GestureKind { none, keyAudition, keyScroll, pendingNote, moveNote, resizeNote, newNote, velocity, tempoDrag };
	struct Gesture {
		GestureKind kind = GestureKind::none;
		int engineIndex = -1;      // the note's index in the engine's sequence, kept current after every update
		double curStart = 0.0;     // the note as it currently is in the engine
		double curEnd = 0.0;
		int curNote = 0;
		int curVelocity = 0;
		int grabStepOffset = 0;    // move: step under the mouse minus the note's own start step
		double grabOffsetBeats = 0.0; // move with GRID OFF: same thing in beats
		int startStep = 0;         // newNote/resizeNote: the note's first step
		bool onRightEdge = false;
		bool freeOverride = false; // middle button: this gesture ignores the grid, as GRID OFF would
		bool checkpointed = false; // an undo snapshot was already pushed for this gesture
		bool changed = false;      // the note was actually modified (vs a plain click)
		juce::Point<float> downPos;
		double tempoStart = 0.0;
		int scrollStartTop = 0;    // keyScroll: topNote when the drag began
	} gesture;
	int auditionNote = -1;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110SequencerGridPanel)
};
