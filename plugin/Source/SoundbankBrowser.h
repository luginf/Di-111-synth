#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <set>

#include "SoundbankDatabase.h"

class D110AudioProcessor;

// The Soundbanks feature's one UI component - shared verbatim between the desktop extended
// editor's own SOUNDBANKS tab and Android's swapped-in Soundbanks view (same reuse pattern as
// D110Keyboard/D110SequencerPanel: constructed straight from a D110AudioProcessor&, no
// separate Host interface, since both apps already talk to the same processor API directly).
// Alan's request, 2026-08-28: browse a personal SysEx tone library by name, grouped
// alphabetically with a count per letter and an overall total (both shown once a scan has
// run); a chosen tone can either be dropped straight into a part for instant, non-destructive
// audition (double-click - see mouseDoubleClick()'s own comment), or written permanently into
// one chosen internal Tone Memory slot ("Inject to slot...", with a confirm step naming what
// it would overwrite - Alan's request, 2026-08-28: never silently clobber a slot he's using).
class SoundbankBrowser : public juce::Component {
public:
	explicit SoundbankBrowser(D110AudioProcessor &processorToUse);
	~SoundbankBrowser() override;

	// Reloads the database's index (cheap - see d110bank::Database::load()) and refreshes the
	// list/counts. Call whenever this becomes visible, in case a rescan ran from elsewhere
	// (e.g. Android and the desktop plugin never share a live process, but the database on
	// disk is the same folder if Alan points both at it).
	void refresh();

	// Which Part (0-7) a double-click/double-tap auditions into right now - the PART row's own
	// selection. Android's own test-note/HOLD buttons (Main.cpp, Alan's request 2026-08-30)
	// target whichever Part this is, matching what auditioning a tone here already does.
	int getSelectedPart() const { return selectedPart; }

	// Fires after a tone is auditioned into a Part's live Tone Temporary (double-click, or any
	// of the right-click/long-press "Send to Part N" paths) - `part` is which one. Null by
	// default; only Android's Main.cpp wires this up (Alan's request, 2026-08-30: HOLD's own
	// note should retrigger - kill the old one, strike the new - whenever browsing lands on a
	// different sound while HOLD is on, so a continuously-held test note actually previews each
	// newly-picked tone's attack instead of just quietly swapping underneath a note already
	// mid-decay). The desktop editor's own SOUNDBANKS tab has no such feature and leaves this
	// unset.
	std::function<void(int part)> onToneAuditioned;

	void paint(juce::Graphics &g) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &e) override;
	void mouseDrag(const juce::MouseEvent &e) override;
	void mouseUp(const juce::MouseEvent &e) override;
	void mouseDoubleClick(const juce::MouseEvent &e) override;
	void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override;
	bool keyPressed(const juce::KeyPress &key) override;

private:
	void rebuildFilteredList();
	void startRescan();
	// Both "Inject to slot..." paths (the button, for `selectedEntry`, and the right-click
	// menu, for whichever row was clicked) end up here.
	void showInjectMenu();
	void showInjectMenuFor(const d110bank::Entry &entry);
	// Writes `entry`'s 256 bytes permanently into internal Tone Memory slot `slot` (via
	// D110AudioProcessor::injectSoundbankTone()) and updates the status line - what "Inject to
	// slot..." does once its confirmation step (showInjectMenu()'s own comment) is accepted.
	void injectInto(const d110bank::Entry &entry, int slot);
	// Shared by double-click (the currently PART-selector part) and the right-click "Send to"
	// submenu (any part, explicitly chosen) - see mouseDoubleClick()'s own comment for why this
	// writes straight into Tone Temporary rather than a stored slot.
	void auditionEntryToPart(const d110bank::Entry &entry, int part);
	// Right-click on a row - Alan's request, 2026-08-28: Favorites toggle, "Send to Part N"
	// (1-8, quick-audition without changing the PART selector), "Inject to slot..." (same as
	// the button), "Send to real D-110..." (2026-08-28 follow-up - see
	// showExternalInjectMenuFor()'s own comment). On Android, the same menu opens from a
	// long-press instead (no right mouse button to hold there) - see handleLongPress().
	void showContextMenuFor(const d110bank::Entry &entry);
	// "Rename..." on the row context menu - Alan's request, 2026-08-30: rewrites the tone's own
	// embedded 10-char name (via d110bank::Database::rename() - see its own comment for why,
	// unlike everything else this component writes, that's a direct file write rather than a
	// firmware SysEx write, since a soundbank entry isn't loaded into any running instrument).
	void promptRenameEntry(const d110bank::Entry &entry);
	// "Delete" on the row context menu - Alan's request, 2026-08-30: permanently removes a tone
	// from the database (confirmed first - this can't be undone the way RESCAN/Favorites can).
	// Also drops it from Favorites if it was one, since Database::remove() itself doesn't know
	// about that separate file.
	void confirmDeleteEntry(const d110bank::Entry &entry);
	// Re-reads the database (same reasoning as checkRescanProgress()'s own post-scan reload -
	// rename()/remove() can shift/replace pointers into Database's own `entries` vector) and
	// refreshes the filtered list/duplicate-hidden set. Shared tail end of promptRenameEntry()/
	// confirmDeleteEntry() once their edit actually lands.
	void reloadAfterEdit(const juce::String &statusMessage);
	// Long-press-to-context-menu, Android's equivalent of a desktop right-click (touch has no
	// second button). Scheduled from mouseDown() (touch sources only - see its own comment) via
	// juce::Timer::callAfterDelay(); `gestureId` is compared against `longPressGestureId` so a
	// callback from an earlier, already-finished gesture can't fire late and act on stale state.
	void handleLongPress(int gestureId, d110bank::Entry entry);
	// "Send to real D-110..." on the row context menu - Alan's request, 2026-08-28: write a
	// browsed tone into a slot on an ACTUAL connected D-110 over a real MIDI Out port
	// (D110AudioProcessor::sendSoundbankToneToExternalMidi()), not just this emulator's own
	// Tone Memory. Deliberately a separate menu from showInjectMenuFor() rather than a shared
	// one: that one can show each slot's CURRENT name (read from this emulator's own RAM) next
	// to its number, which is impossible for a real remote unit this app has no read access
	// to - the two menus look almost the same but that's the reason they're not one function.
	void showExternalInjectMenuFor(const d110bank::Entry &entry);
	// "Send to real D-110" > "Part N" - Alan's request, 2026-08-28 follow-up: also reach a
	// Part on the real unit directly, the external-hardware equivalent of auditionEntryToPart()
	// ("comme le font la plupart des éditeurs" - his own words), not just a Tone Memory slot.
	void sendToExternalPart(const d110bank::Entry &entry, int part);
	void pickGroup(const juce::String &group);
	void pickPart(int part);
	void clearSearch();
	// "EXPORT FAVORITES..." button - Alan's request, 2026-08-28. Sorts the current favourites
	// alphabetically (his own request) and hands them to d110bank::exportTonesAsSysex() - see
	// that function's own comment for the file-splitting/naming scheme (its own "mes
	// favoris.syx" -> "mes favoris_01.syx"/"_02.syx"/... example is implemented there, not
	// here, since Database/Favorites is where the on-disk tone bytes actually live).
	void exportFavorites();
	// "HIDE DUPLICATES" toggle - Alan's request, 2026-08-30: find tones that are the same SOUND
	// (identical 246-byte tone body - see d110bank::Database::findDuplicateGroups()'s own
	// comment) even when captured under different names, and filter the browse list down to one
	// representative per group, search included - nothing is deleted, a hidden entry is still
	// reachable by switching the toggle back off. `duplicateHidden` is only ever populated while
	// the toggle is on - recomputed here and after anything that can change the database's
	// contents (rescan, delete).
	void toggleHideDuplicates();
	void recomputeDuplicateHidden();
	// "BACKUP..." button - Alan's request, 2026-08-30: zips the whole database (index.json +
	// every tone file - d110bank::Database::backupToZip()) to a file Alan picks. Independent of
	// Favorites, same reasoning as EXPORT FAVORITES being its own separate action.
	void backupDatabase();
	// "RESTORE..." button - Alan's request, 2026-08-30: REPLACES the current database wholesale
	// from a zip written by backupDatabase() (his own explicit choice - a restore should put
	// things back exactly as backed up, not merge) - confirmed first, since it's destructive to
	// whatever's in the database right now.
	void restoreDatabase();
#if !JUCE_ANDROID
	// "CHOOSE FILES/FOLDER..." button - Alan's request, 2026-08-28: picking the SysEx source
	// used to only be reachable from the desktop extended editor's own Utility tab, which he
	// found unintuitive ("pas besoin de mettre ça dans l'onglet utility") - and that picker
	// could only select a whole FOLDER, not a loose file or a .zip directly, which was his
	// separate "je n'ai pas trouvé comment importer un zip" report. This one picker handles
	// both: a folder picked alone sets it as getSoundbankSourceFolder() (unchanged behaviour,
	// same as the Utility tab's own button, still there too); one or more individual files
	// (including a bare .zip) are copied into getSoundbankImportsFolder() instead, which
	// startRescan() now always scans in ADDITION to the configured source folder - see its own
	// comment for why that's kept separate rather than overwriting it. Android-only excluded:
	// it already has its own dedicated, SAF-appropriate flow (the hamburger menu's "Choose
	// soundbank files..."), and a desktop-style combined file-or-folder juce::FileChooser isn't
	// something Android's picker reliably supports the way native desktop dialogs do.
	void chooseSource();
#endif

	D110AudioProcessor &processor;

	juce::TextEditor searchBox;
	juce::TextButton clearSearchButton{ "X" };
	juce::TextButton rescanButton{ "RESCAN" };
	juce::TextButton injectButton{ "INJECT TO SLOT..." };
	juce::TextButton exportFavoritesButton{ "EXPORT FAVORITES..." };
	juce::TextButton hideDuplicatesButton{ "HIDE DUPLICATES" };
	juce::TextButton backupButton{ "BACKUP..." };
	juce::TextButton restoreButton{ "RESTORE..." };
#if !JUCE_ANDROID
	juce::TextButton chooseSourceButton{ "CHOOSE FILES/FOLDER..." };
#endif
	juce::Label statusLabel;

	// See toggleHideDuplicates()'s own comment - membership only, no ordering/display
	// information (that lives in the group each entry came from, findDuplicateGroups()'s own
	// vector, not needed again once the hidden set is built).
	bool hideDuplicates = false;
	std::set<const d110bank::Entry *> duplicateHidden;

	// "ALL", "FAVORITES", "A".."Z", "0-9", "_" - fixed browse order. A non-empty search query
	// overrides `selectedGroup` entirely (searches every letter, not just the selected one -
	// Alan's request, 2026-08-28) - see rebuildFilteredList().
	juce::StringArray groupKeys;
	juce::String selectedGroup = "ALL";
	std::vector<const d110bank::Entry *> filtered;
	const d110bank::Entry *selectedEntry = nullptr;

	// Which Part a double-click auditions into (0-7) - explicit and remembered rather than
	// guessed from e.g. the keyboard's MIDI channel, since there's no reliable channel->Part
	// mapping available here (the real mapping lives in the firmware's own live SYSTEM area,
	// not any fixed formula - see D110AudioProcessor::selectTimbreForPart()'s own comment).
	// Mirrors exactly what picking a tone for a Part in the PARTS tab already asks for: which
	// part.
	int selectedPart = 0;
	std::vector<juce::Rectangle<float>> partBounds; // 8, one per Part button

	juce::Rectangle<float> listArea, groupStripArea, partStripArea;
	std::vector<juce::Rectangle<float>> groupBounds; // parallel to groupKeys
	// The ALL/FAVORITES/A-Z strip keeps a readable row height and scrolls (wheel, scrollbar,
	// drag) when the window is too short for all of it - rows used to shrink until the labels
	// overlapped. groupBounds already include the scroll offset.
	void layoutGroups();
	float groupScroll = 0.0f, groupContentH = 0.0f;
	juce::Rectangle<float> groupScrollTrack, groupScrollThumb;
	bool draggingGroupScroll = false;
	float groupDragStartY = 0.0f, groupDragStartScroll = 0.0f;

	// The tone list flows into as many columns as listArea's width allows (Alan's request,
	// 2026-08-28: a single column wasted most of a wide window's width) - row-major (left to
	// right, then wrap), so scrolling moves every column together as one grid, the same way
	// text wraps. `rowBounds`/`rowEntries` are parallel, one pair per visible CELL (not
	// necessarily one per `filtered` row anymore) - mouseDown()/mouseDoubleClick() index
	// `rowEntries` directly rather than re-deriving a position from `listScrollRow`.
	int listColumns = 1;
	int listScrollRow = 0; // scroll position in GRID ROWS, not raw `filtered` indices
	int visibleGridRows = 0; // set each paint(), reused by keyPressed()'s auto-scroll-to-selection
	std::vector<juce::Rectangle<float>> rowBounds;
	std::vector<const d110bank::Entry *> rowEntries;

	// A real scrollbar (Alan's request, 2026-08-28: mouse wheel only wasn't discoverable/usable,
	// and Android has no wheel at all) - narrow track to the right of the list, click-to-page or
	// drag-the-thumb, computed each paint() since it depends on the same totalGridRows/
	// visibleGridRows the grid layout itself does.
	juce::Rectangle<float> scrollbarArea, scrollbarThumb;
	bool draggingThumb = false;
	float thumbDragGrabOffsetY = 0.0f; // where inside the thumb the drag started, so it doesn't jump

	// Touch-drag-to-scroll the list itself (Alan's request, 2026-08-28: Android has no wheel and
	// dragging the list directly, not just a thin scrollbar, is the natural touch gesture). A
	// small movement threshold distinguishes a tap (selects a row, in mouseUp) from a drag
	// (scrolls, no selection) - selection is deferred to mouseUp for exactly this reason.
	bool trackingListDrag = false;
	bool listDragIsScroll = false;
	juce::Point<float> listDragStartPos;
	int listDragStartScrollRow = 0;

	// Long-press-to-context-menu (touch only - see mouseDown()'s own comment): scheduled via
	// juce::Timer::callAfterDelay() rather than a juce::Timer subclass since it only ever needs
	// one, cancellable-by-flag-check shot per gesture (see handleLongPress()'s own comment).
	// `longPressGestureId` increments on every new touch-down in the list, so a stale callback
	// from an already-finished (tapped, scrolled, or already-long-pressed) gesture is a no-op.
	int longPressGestureId = 0;

	// A rescan walks an arbitrary folder and can genuinely take a while for a big library -
	// runs off the message thread so the UI stays responsive; polled by a timer rather than a
	// direct callback since juce::Thread callbacks don't run on the message thread.
	struct RescanThread;
	std::unique_ptr<RescanThread> rescanThread;
	class PollTimer : public juce::Timer {
	public:
		explicit PollTimer(SoundbankBrowser &ownerToUse) : owner(ownerToUse) {}
		void timerCallback() override;

	private:
		SoundbankBrowser &owner;
	};
	PollTimer pollTimer{ *this };
	void checkRescanProgress();

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoundbankBrowser)
};
