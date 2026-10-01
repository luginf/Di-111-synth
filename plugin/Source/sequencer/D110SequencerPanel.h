#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "D110SequencerEngine.h"
#include "D110SequencerHost.h"

// D-20-style sequencer drawer: a transport strip plus one row per track (D-110 parts
// 1-8, then rhythm). Inside the plugin it's a third independently-foldable drawer,
// alongside D110EditorPane and D110Keyboard - see D110AudioProcessorEditor, which owns
// and positions this the same way it does those two. In the independent sequencer app
// (see NonetSeqHost, the independent Nonet Sequencer app) it's the whole window instead. Talks to its host only
// through D110SequencerHost, so this UI - like the engine itself - doesn't know anything
// about firmware RAM, and doesn't require one to exist.
class D110SequencerPanel : public juce::Component, private juce::Timer {
public:
	explicit D110SequencerPanel(D110SequencerHost &);
	~D110SequencerPanel() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

	// Reference height, in the same units as D110Panel::kRefH - what the owning editor
	// adds to its own layout, exactly as it already does for D110Keyboard::kRefH. Raised
	// ~30% from the original 250 (Alan: tracks were too thin to read comfortably), then by
	// another 22 for the step-recording strip.
	static constexpr float kRefH = 347.0f;

	// Same toggle as right-clicking the extra-tracks zone (see showExtraTracksMenu()) - public
	// so NonetSeqMain's Options dialog can offer the same switch without duplicating the
	// trackPage-reset side effect. Only meaningful when processor.supportsExtraTracks();
	// harmless no-op call site is on Nonet-Seq only anyway (the plugin has no Options dialog).
	void toggleExtraTracks();

	// The bar-navigation menu button (☰, next to the > arrow - Alan's request, 2026-08-22,
	// occupying the column TAP used to have before it moved into promptForTempo()'s own
	// dialog) already opens showBarMenu(). This lets a host that has nowhere else to put its
	// own menu append extra items to that SAME popup instead of needing a second button -
	// the Android app uses it for exactly that (2026-08-22: switching to the sequencer view
	// there hides the app's own Play/Stop/hamburger row entirely to give the sequencer the
	// full height, so this becomes the only way back to it). Empty by default - zero effect
	// on the desktop plugin or Nonet Sequencer, neither of which sets it.
	std::function<void(juce::PopupMenu &)> onBarMenuButtonExtra;

	// Android app only: when set, a mode-switch button (cycles front panel / sequencer /
	// soundbanks, see AppModeIcons.h) takes a column just left of the bar-menu button, and the
	// other transport columns squeeze a little to make room. Unset on the desktop.
	std::function<void()> onModeButton;
	std::function<int()> modeButtonMode; // d110ui::AppMode as int, for the icon

private:
	void timerCallback() override; // repaints the bar readout while the transport rolls

	d110seq::D110SequencerEngine &engine();
	void layout();
	void cycleTimeSignature();
	void showTimeSignatureMenu();
	void cycleRecordMode();
	void showRecordModeMenu();
	void showQuantizeMenu(int track);
	void promptForRenameTrack(int track);
	// Only ever called when processor.supportsTrackChannelEdit() - see D110SequencerHost.h.
	// Also offers a "Program Change..." entry when processor.supportsProgramChange().
	void showTrackChannelMenu(int track);
	// Only ever called when processor.supportsProgramChange() - see D110SequencerHost.h.
	void promptForTrackProgram(int track);
	void showMetronomeModeMenu();
	// Right-click UNDO/REDO: a non-interactive popup naming what it would actually revert/
	// redo (e.g. "Clear track PART 2") - see D110SequencerEngine::getUndoDescription()/
	// getRedoDescription().
	void showUndoRedoInfo(bool isUndo);
	void showResyncInfo();
	void confirmCaptureLivePatch();
	void confirmClearTrack(int track);
	// Right-click any song-slot button: copy the CURRENT song into one of the other 3 slots
	// (see D110SequencerEngine::copyCurrentSongTo()), plus - only when
	// processor.supportsSoundSnapshots() - store/load the CLICKED slot's own sound
	// snapshot (see D110SequencerHost.h).
	void showCopySongMenu(int clickedSlot);
	void confirmCopySongTo(int destSlot);
	void confirmLoadSoundSnapshot(int slot);
	// Right-click LOAD/SAVE: all 4 song slots at once (.midiseq), as opposed to the plain
	// click's single current song (.mid) - see D110AudioProcessor::exportSequencerSongs().
	void showLoadMenu();
	void showSaveMenu();
	void cycleLoopMode();
	// Right-click the extra-tracks zone (above the track rows, right of the step-recording
	// strip) - only reachable when processor.supportsExtraTracks(). Toggles
	// D110SequencerEngine::setExtraTracksEnabled(); switching it off also snaps trackPage
	// back to 0, since page 1 would otherwise show tracks that no longer play/export/undo.
	void showExtraTracksMenu();
	// Which physical track index a given on-screen row (0-8) currently represents - row +
	// (trackPage == 0 ? 0 : D110SequencerEngine::kNumTracks). Page 1 only has 7 rows worth of
	// real tracks (9-15); rows 7-8 on that page have no backing track and are skipped by
	// every loop that calls this, never dereferenced.
	int trackForRow(int row) const;
	// How many of the 9 on-screen rows are backed by a real track on the current page - 9 on
	// page 0 always, 7 (kMaxTracks - kNumTracks) on page 1.
	int rowsOnCurrentPage() const;
	void showBarMenu();
	void promptForBar();
	// Bridges a FileChooser URL result to plain juce::File-based load/save actions
	// (D110SequencerEngine::loadMidiFile/saveMidiFile, D110SequencerHost::importSequencerSongs/
	// exportSequencerSongs - all four take a juce::File, none a stream). On desktop the URL is
	// always already a local file, so this is a same-cost passthrough there. On Android it may
	// be a content:// SAF result with no real filesystem path at all (see juce::AndroidDocument's
	// own class comment - the documented answer to exactly this), so the action instead runs
	// against a temp file, copied from (load) or to (save) the real destination through an
	// AndroidDocument stream. Alan's request, 2026-08-22, after the exact same bug already hit
	// (and got fixed) the Android app's own MIDI-file player, one level up.
	void withLocalFileForLoad(const juce::URL &url, std::function<void(const juce::File &)> action);
	void withLocalFileForSave(const juce::URL &url, const juce::String &extension,
	                           std::function<void(const juce::File &)> action);
	// Right-click on the TEMPO readout - a text-entry alternative to the click-drag/wheel
	// adjustments, for setting an exact BPM directly.
	void promptForTempo();
	void promptForPunchRange();
	void promptForTimeSignature();
	void confirmNewSong();
	// STEP-strip controls - see D110SequencerEngine::setStepDuration()/startStepRecording().
	void cycleStepDuration();
	void showStepDurationMenu();
	// track == -1 means every track at once (from the BAR readout's own menu); track >= 0
	// scopes the operation to just that one track (from a track row's right-click menu). See
	// D110SequencerEngine::deleteBars()/copyBars() for what "every track" vs. "just this one"
	// actually does to bar alignment.
	void promptForDeleteBars(int track);
	void promptForCopyBars(int track);
	// Same track==-1-means-every-track convention as the two above - see
	// D110SequencerEngine::transposeBars().
	void promptForTransposeBars(int track);
	// A graphical, scrollable list of every note in the CURRENTLY NAVIGATED bar on this one
	// track, each with its own delete button - "pas un piano roll", Alan's own framing, for
	// removing a single wrong note without a bar-range operation or re-recording. Re-opens
	// fresh each time (not kept in sync with gotoBar() while open) - see
	// D110SequencerEngine::eventsInBarRange()/deleteNoteEvent().
	void promptForEventList(int track);

	D110SequencerHost &processor;

	juce::Rectangle<float> stopBounds, playBounds, recBounds;
	juce::Rectangle<float> tempoBounds, timeSigBounds, metronomeBounds, precountBounds, loopBounds;
	bool stackedTransport = false; // Android portrait three-row transport, see layout()
	juce::Rectangle<float> barPrevBounds, barNextBounds, barReadoutBounds, barMenuBounds, modeBounds;
	juce::Rectangle<float> loadBounds, saveBounds, recModeBounds, newBounds, undoBounds, redoBounds;
	// Manual "resend Program Change/Bank/Volume/Pan now" - see D110SequencerHost.h's
	// resyncProgramChanges().
	juce::Rectangle<float> resyncBounds;
	// Step-recording strip, under the file strip - see D110SequencerEngine's step API.
	// stepInfoBounds is a plain readout (current bar/step while active), not clickable.
	juce::Rectangle<float> stepBounds, stepDurationBounds, stepDotBounds, restBounds, backBounds,
		stepInfoBounds;
	// "1-9" / "10-16" page-switch buttons, and the (wider, always-hit-testable-on-right-click)
	// zone above the track rows that toggles extra tracks on/off - see showExtraTracksMenu().
	// Only drawn/clickable when processor.supportsExtraTracks() && the engine's own
	// getExtraTracksEnabled() (page buttons) - the right-click zone itself is always
	// hit-testable when supportsExtraTracks(), even before extra tracks are turned on, since
	// that's how they GET turned on.
	juce::Rectangle<float> trackPage1Bounds, trackPage2Bounds, extraTracksZoneBounds;
	// One button per song slot (see D110SequencerEngine::kNumSongSlots) - click to switch,
	// highlighted on whichever is current, with a small dot for slots that have content.
	std::array<juce::Rectangle<float>, d110seq::D110SequencerEngine::kNumSongSlots> slotBounds;
	// Visual metronome: one LED per metronome click in the bar (see
	// D110SequencerEngine::clicksPerBar/currentClickInBar), shown under the transport strip
	// whenever METRO is on.
	juce::Rectangle<float> metroLedBounds;

	struct TrackRow {
		// rowBounds spans the whole row - used only to catch a right-click anywhere on the
		// row for the quantize menu, regardless of which column it lands on.
		juce::Rectangle<float> rowBounds;
		juce::Rectangle<float> label, channelReadout, muteBounds, soloBounds, armBounds, activityBounds,
			partNumberBounds;
	};
	// Sized to kMaxTracks so rows[] can be indexed directly by absolute track index on either
	// page - only rowsOnCurrentPage() of them are laid out/painted/hit-tested at a time.
	std::array<TrackRow, d110seq::D110SequencerEngine::kMaxTracks> rows;

	// 0 = tracks 1-9, 1 = tracks 10-16 - see trackForRow(). Not persisted (pure navigation
	// state, resets to page 0 on relaunch, like D-110 EditorPane's own PatchesSubTab).
	int trackPage = 0;

	// Precount's downbeat-LED flash (see paint()'s own comment) - edge-detected in
	// timerCallback() against D110SequencerEngine::precountBeatsElapsed(), since positionBeats
	// itself is frozen throughout precount and can't be used to time this the way the normal
	// scrolling LED strip is.
	int lastPrecountBeatsElapsed = -1;
	juce::int64 precountFlashUntilMs = 0;

	bool draggingTempo = false;
	float tempoDragStartY = 0.0f;
	double tempoDragStartValue = 0.0;

	bool draggingBar = false;
	float barDragStartY = 0.0f;
	int barDragStartValue = 1;

	// Touchscreens have no right mouse button - see mouseDown()'s own comment for how a long
	// press stands in for it. handleContextAction() is the exact body a real right-click has
	// always run (extracted, not duplicated, so both paths stay identical by construction).
	// longPressToken is bumped on release or on moving past the threshold, which is what lets
	// the deferred callAfterDelay() callback recognise a long press it should no longer act on
	// (SafePointer covers the component being destroyed outright in the meantime).
	void handleContextAction(juce::Point<float> p);
	juce::Point<float> longPressStartPos;
	int longPressToken = 0;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110SequencerPanel)
};
