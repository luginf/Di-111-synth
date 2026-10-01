#include "SoundbankBrowser.h"

#include "PluginProcessor.h"
#include "UiTheme.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace {
constexpr float kRowHeight = 22.0f;
constexpr float kGroupStripWidth = 260.0f; // wide enough for 3 legible columns
constexpr int kGroupColumns = 3;
// Minimum width for one tone-list column before another one is added - wide enough for a
// long real name plus a " (N)" disambiguation suffix without truncating.
constexpr float kListColumnMinWidth = 200.0f;
// Wide enough to comfortably drag with a fingertip on Android, not just a mouse pointer.
constexpr float kScrollbarWidth = 18.0f;

// Internal Tone Memory slots are labelled plainly "SLOT 1".."SLOT 64" - the exact string the
// TONES tab's own slot list already uses (D110EditorPane::layoutTones(), PluginEditor.cpp,
// "SLOT " + juce::String(toneSlot + 1)) - no decade-skip scheme the way Patch numbering has.
juce::String toneSlotLabel(int slot) { return "SLOT " + juce::String(slot + 1); }
} // namespace

// Runs Database::rescan() (a recursive folder walk + per-file SysEx decode - can genuinely
// take a while for a big personal library) off the message thread so the browser stays
// responsive. Polled by PollTimer rather than driven by a completion callback since
// juce::Thread has no message-thread-safe callback of its own.
//
// Crash fix (Alan's report, 2026-08-28): this used to rescan() the PROCESSOR'S OWN shared
// Database instance directly from this background thread, while paint()/rebuildFilteredList()
// kept reading that same instance's `entries` vector from the message thread the whole time a
// scan was running - a plain unsynchronised concurrent read/write on a std::vector (rescan()
// push_back()-ing, which can reallocate, while paint() iterates it), undefined behaviour that
// crashed reliably on a real-sized library (scan slow enough for at least one repaint to land
// mid-scan). Fixed by giving this thread its OWN private Database instance (same on-disk root,
// but a separate in-memory object) - the shared instance is now only ever touched from the
// message thread, in checkRescanProgress()'s post-scan load().
struct SoundbankBrowser::RescanThread : public juce::Thread {
	// `folders` - Alan's request, 2026-08-28: RESCAN always walks TWO roots now, not one - the
	// user's own configured source folder (getSoundbankSourceFolder(), optional, may be empty)
	// AND the fixed staging folder individually-picked files/zips land in
	// (D110AudioProcessor::getSoundbankImportsFolder(), always scanned regardless) - see
	// startRescan()'s own comment for why they're kept separate rather than one replacing the
	// other. Database::rescan() itself stays single-folder (unchanged, still well-tested) -
	// this just calls it once per folder, summing the total added.
	RescanThread(const juce::File &dbRoot, juce::Array<juce::File> folders)
		: juce::Thread("D-110 Soundbank Rescan"), database(dbRoot), sourceFolders(std::move(folders)) {}

	void run() override {
		int total = 0;
		for (const auto &folder : sourceFolders)
			if (folder.isDirectory()) total += database.rescan(folder);
		added.store(total, std::memory_order_release);
		finished.store(true, std::memory_order_release);
	}

	d110bank::Database database; // this thread's own - never touched by the message thread
	juce::Array<juce::File> sourceFolders;
	std::atomic<bool> finished{ false };
	std::atomic<int> added{ 0 };
};

void SoundbankBrowser::PollTimer::timerCallback() { owner.checkRescanProgress(); }

SoundbankBrowser::SoundbankBrowser(D110AudioProcessor &processorToUse) : processor(processorToUse) {
	// So mouseDown()'s grabKeyboardFocus() (see its own comment) actually has somewhere neutral
	// to park focus, instead of it silently failing/going to the nearest focusable ancestor.
	setWantsKeyboardFocus(true);

	groupKeys.add("ALL");
	groupKeys.add("FAVORITES");
	for (char c = 'A'; c <= 'Z'; ++c) groupKeys.add(juce::String::charToString((juce::juce_wchar) c));
	groupKeys.add("0-9");
	groupKeys.add("_");

	searchBox.setTextToShowWhenEmpty("Click to search all sounds...", juce::Colours::grey);
	searchBox.onTextChange = [this] { rebuildFilteredList(); repaint(); };
	addAndMakeVisible(searchBox);

	// Alan's request, 2026-08-28: a way to get back out of a search - see clearSearch().
	clearSearchButton.onClick = [this] { clearSearch(); };
	addAndMakeVisible(clearSearchButton);

	rescanButton.onClick = [this] { startRescan(); };
	addAndMakeVisible(rescanButton);

	injectButton.onClick = [this] { showInjectMenu(); };
	injectButton.setEnabled(false);
	addAndMakeVisible(injectButton);

	exportFavoritesButton.onClick = [this] { exportFavorites(); };
	exportFavoritesButton.setEnabled(false);
	addAndMakeVisible(exportFavoritesButton);

	hideDuplicatesButton.setClickingTogglesState(true);
	hideDuplicatesButton.onClick = [this] { toggleHideDuplicates(); };
	addAndMakeVisible(hideDuplicatesButton);

	backupButton.onClick = [this] { backupDatabase(); };
	addAndMakeVisible(backupButton);

	restoreButton.onClick = [this] { restoreDatabase(); };
	addAndMakeVisible(restoreButton);

#if !JUCE_ANDROID
	chooseSourceButton.onClick = [this] { chooseSource(); };
	addAndMakeVisible(chooseSourceButton);
#endif

	statusLabel.setJustificationType(juce::Justification::centredLeft);
	addAndMakeVisible(statusLabel);

	refresh();
}

SoundbankBrowser::~SoundbankBrowser() {
	pollTimer.stopTimer();
	if (rescanThread != nullptr) rescanThread->waitForThreadToExit(2000);
}

void SoundbankBrowser::refresh() {
	processor.getSoundbankDatabase().load();
	selectedEntry = nullptr;
	injectButton.setEnabled(false);
	exportFavoritesButton.setEnabled(processor.getSoundbankFavorites().size() > 0);
	recomputeDuplicateHidden();
	rebuildFilteredList();

	const int total = processor.getSoundbankDatabase().size();
	// "Utility tab" doesn't exist on Android (no extended editor drawer there at all - see
	// CLAUDE.md) - this message used to say that unconditionally and was simply wrong there,
	// where the source folder/files are actually picked from the hamburger menu instead. Kept
	// platform-agnostic now rather than naming either location.
	statusLabel.setText(total == 0
	                         ? juce::String("No soundbank scanned yet - choose your SysEx files, "
	                                        "then RESCAN.")
	                         : juce::String(total) + (total == 1 ? " sound" : " sounds")
	                               + " in the database.",
	                     juce::dontSendNotification);
	repaint();
}

void SoundbankBrowser::rebuildFilteredList() {
	filtered.clear();
	const auto &db = processor.getSoundbankDatabase();
	const auto &favorites = processor.getSoundbankFavorites();
	const auto query = searchBox.getText().trim();
	// Alan's request, 2026-08-28: while actively searching, search EVERY letter, not just
	// whichever one happens to be selected - a query overrides the group filter entirely
	// rather than narrowing it further.
	const bool searching = query.isNotEmpty();

	std::vector<const d110bank::Entry *> pool;
	if (searching || selectedGroup == "ALL") {
		pool.reserve(size_t(db.size()));
		for (const auto &e : db.all()) pool.push_back(&e);
		std::sort(pool.begin(), pool.end(), [](const d110bank::Entry *a, const d110bank::Entry *b) {
			return a->displayName.compareIgnoreCase(b->displayName) < 0;
		});
	} else if (selectedGroup == "FAVORITES") {
		for (const auto &e : db.all())
			if (favorites.contains(e)) pool.push_back(&e);
		std::sort(pool.begin(), pool.end(), [](const d110bank::Entry *a, const d110bank::Entry *b) {
			return a->displayName.compareIgnoreCase(b->displayName) < 0;
		});
	} else {
		pool = db.byLetter(selectedGroup);
	}

	for (const auto *e : pool) {
		if (hideDuplicates && duplicateHidden.count(e) > 0) continue;
		if (query.isEmpty() || e->displayName.containsIgnoreCase(query)) filtered.push_back(e);
	}

	listScrollRow = 0;
	if (selectedEntry != nullptr
	    && std::find(filtered.begin(), filtered.end(), selectedEntry) == filtered.end()) {
		selectedEntry = nullptr;
		injectButton.setEnabled(false);
	}
}

void SoundbankBrowser::pickGroup(const juce::String &group) {
	selectedGroup = group;
	rebuildFilteredList();
	repaint();
}

void SoundbankBrowser::pickPart(int part) {
	selectedPart = juce::jlimit(0, 7, part);
	repaint();
}

void SoundbankBrowser::clearSearch() {
	// sendTextChangeMessage=true so onTextChange fires immediately (rebuildFilteredList()),
	// same as if Alan had selected the text and pressed Delete.
	searchBox.setText({}, true);
	repaint();
}

// "EXPORT FAVORITES..." - Alan's request, 2026-08-28.
void SoundbankBrowser::exportFavorites() {
	auto &favorites = processor.getSoundbankFavorites();
	if (favorites.size() == 0) return;

	// Alphabetical, per Alan's own request - the order exportTonesAsSysex() then writes/splits
	// into 64-tone chunks in.
	std::vector<d110bank::Entry> sorted;
	for (const auto &e : processor.getSoundbankDatabase().all())
		if (favorites.contains(e)) sorted.push_back(e);
	std::sort(sorted.begin(), sorted.end(), [](const d110bank::Entry &a, const d110bank::Entry &b) {
		return a.displayName.compareIgnoreCase(b.displayName) < 0;
	});

	// Same raw-pointer-captured-in-its-own-callback FileChooser idiom PluginEditor.cpp already
	// uses for its own SysEx/snapshot export dialogs (e.g. "Export SysEx bank as").
	auto *chooser = new juce::FileChooser("Export Favorites as SysEx...",
	                                       processor.getLastDialogDir().getChildFile("favorites.syx"),
	                                       "*.syx");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser, sorted](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("syx")) file = file.withFileExtension("syx");
				processor.setLastDialogDir(file.getParentDirectory());
				const int written = d110bank::exportTonesAsSysex(sorted, file);
				statusLabel.setText(written <= 1
				                         ? "Exported " + juce::String(sorted.size()) + " favorite(s) to "
				                               + file.getFileName() + "."
				                         : "Exported " + juce::String(sorted.size())
				                               + " favorite(s) across " + juce::String(written)
				                               + " files (" + file.getFileNameWithoutExtension() + "_01.."
				                               + juce::String(written).paddedLeft('0', 2) + ").",
				                     juce::dontSendNotification);
				repaint();
			}
			delete chooser;
		});
}

// "HIDE DUPLICATES" - Alan's request, 2026-08-30.
void SoundbankBrowser::toggleHideDuplicates() {
	hideDuplicates = hideDuplicatesButton.getToggleState();

	// setClickingTogglesState()'s own colouring relies on a LookAndFeel this app's own
	// Standalone/plugin never installs (only Nonet Sequencer's main() does, via
	// d110ui::sharedLookAndFeel() - a real gap, discovered testing this very button: with no
	// LookAndFeel installed, JUCE's stock default buttonOnColourId reads as visually identical
	// to buttonColourId here, so a toggled button gave no visible feedback at all). Set the
	// colours directly instead, same pal.seqActiveFill/pal.value pair paint()'s own hand-painted
	// PART/group "buttons" already use for their own on/off state.
	// Both buttonColourId AND buttonOnColourId set to the SAME pair - TextButton::paintButton()
	// itself already branches on getToggleState() to pick which of the two it reads
	// (buttonOnColourId when on), so setting only one left the other (whichever JUCE's own
	// toggle-state check picked) still on an unset default. Setting both to the current desired
	// pair sidesteps having to also track which ID that branch will choose.
	const auto &pal = d110ui::palette();
	const auto fill = hideDuplicates ? pal.seqActiveFill : pal.box;
	const auto text = hideDuplicates ? pal.seqActiveText : pal.value;
	hideDuplicatesButton.setColour(juce::TextButton::buttonColourId, fill);
	hideDuplicatesButton.setColour(juce::TextButton::buttonOnColourId, fill);
	hideDuplicatesButton.setColour(juce::TextButton::textColourOffId, text);
	hideDuplicatesButton.setColour(juce::TextButton::textColourOnId, text);

	if (hideDuplicates) {
		recomputeDuplicateHidden();
		statusLabel.setText(duplicateHidden.empty()
		                         ? juce::String("No duplicate sounds found.")
		                         : "Hiding " + juce::String(duplicateHidden.size())
		                               + " duplicate sound(s) (same sound, different name/file).",
		                     juce::dontSendNotification);
	} else {
		statusLabel.setText("Showing duplicates again.", juce::dontSendNotification);
	}
	rebuildFilteredList();
	repaint();
}

void SoundbankBrowser::recomputeDuplicateHidden() {
	duplicateHidden.clear();
	if (!hideDuplicates) return; // nothing reads it while the toggle is off - no point computing it
	for (const auto &group : processor.getSoundbankDatabase().findDuplicateGroups())
		for (size_t i = 1; i < group.size(); ++i) duplicateHidden.insert(group[i]);
}

// "BACKUP..." - Alan's request, 2026-08-30.
void SoundbankBrowser::backupDatabase() {
	auto *chooser = new juce::FileChooser(
		"Backup Soundbanks Database as...",
		processor.getLastDialogDir().getChildFile("d110-soundbanks-backup.zip"), "*.zip");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("zip")) file = file.withFileExtension("zip");
				processor.setLastDialogDir(file.getParentDirectory());
				const bool ok = processor.getSoundbankDatabase().backupToZip(file);
				statusLabel.setText(ok ? "Database backed up to " + file.getFileName() + "."
				                        : "Backup failed - nothing to back up, or the file couldn't be written.",
				                     juce::dontSendNotification);
				repaint();
			}
			delete chooser;
		});
}

// "RESTORE..." - Alan's request, 2026-08-30: REPLACES the whole database - confirmed first,
// since whatever's currently in it (and not also in the backup) is gone afterwards.
void SoundbankBrowser::restoreDatabase() {
	auto *chooser =
		new juce::FileChooser("Restore Soundbanks Database from...", processor.getLastDialogDir(), "*.zip");
	chooser->launchAsync(
		juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file == juce::File()) {
				delete chooser;
				return;
			}
			processor.setLastDialogDir(file.getParentDirectory());

			auto *aw = new juce::AlertWindow(
				"Restore database?",
				"This REPLACES the entire Soundbanks database with the contents of \""
					+ file.getFileName()
					+ "\" - any tone added or deleted since that backup was made is lost. "
					  "Favorites are a separate list and are not affected.",
				juce::AlertWindow::WarningIcon);
			aw->addButton("Restore", 1, juce::KeyPress(juce::KeyPress::returnKey));
			aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
			aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, file](int result) {
				if (result == 1) {
					const bool ok = processor.getSoundbankDatabase().restoreFromZip(file);
					if (ok) reloadAfterEdit("Database restored from " + file.getFileName() + ".");
					else
						statusLabel.setText("Restore failed - that file doesn't look like a Soundbanks "
						                     "database backup.",
						                     juce::dontSendNotification);
					repaint();
				}
				delete aw;
			}));
			delete chooser;
		});
}

// "Rename..." on the row context menu - Alan's request, 2026-08-30.
void SoundbankBrowser::promptRenameEntry(const d110bank::Entry &entryRef) {
	const d110bank::Entry entry = entryRef; // async dialog - see showContextMenuFor()'s own comment

	auto *aw = new juce::AlertWindow(
		"Rename tone", "Up to 10 characters, printable ASCII only - the D-110's own name field limit.",
		juce::AlertWindow::NoIcon);
	aw->addTextEditor("name", entry.rawName, "Name:");
	if (auto *nameEditor = aw->getTextEditor("name"))
		nameEditor->setInputRestrictions(10, " !\"#$%&'()*+,-./0123456789:;<=>?@"
		                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
		                                      "abcdefghijklmnopqrstuvwxyz{|}~");
	aw->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, entry](int result) {
		if (result == 1) {
			const auto newName = aw->getTextEditorContents("name").trim();
			if (newName.isNotEmpty() && processor.getSoundbankDatabase().rename(entry, newName))
				reloadAfterEdit("Renamed to \"" + newName + "\".");
		}
		delete aw;
	}));
}

// "Delete" on the row context menu - Alan's request, 2026-08-30.
void SoundbankBrowser::confirmDeleteEntry(const d110bank::Entry &entryRef) {
	const d110bank::Entry entry = entryRef; // async dialog - see showContextMenuFor()'s own comment

	auto *aw = new juce::AlertWindow(
		"Delete this tone?",
		"This permanently removes \"" + entry.displayName
			+ "\" from the database. This can't be undone.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, entry](int result) {
		if (result == 1) {
			// Database::remove() has no knowledge of Favorites (its own separate file) - drop it
			// from there too if it was one, same as removing it any other way would need to.
			auto &favorites = processor.getSoundbankFavorites();
			if (favorites.contains(entry)) favorites.toggle(entry);
			if (processor.getSoundbankDatabase().remove(entry))
				reloadAfterEdit("Deleted \"" + entry.displayName + "\".");
		}
		delete aw;
	}));
}

void SoundbankBrowser::reloadAfterEdit(const juce::String &statusMessage) {
	// Same reasoning as checkRescanProgress()'s own post-scan reload: Database::remove() can
	// shift/reallocate its `entries` vector, invalidating pointers `filtered`/`selectedEntry`
	// hold into it - reloading from the just-saved index.json and rebuilding everything from
	// scratch is the same known-safe pattern already used there.
	processor.getSoundbankDatabase().load();
	selectedEntry = nullptr;
	injectButton.setEnabled(false);
	exportFavoritesButton.setEnabled(processor.getSoundbankFavorites().size() > 0);
	recomputeDuplicateHidden();
	rebuildFilteredList();
	statusLabel.setText(statusMessage, juce::dontSendNotification);
	repaint();
}

#if !JUCE_ANDROID
// "CHOOSE FILES/FOLDER..." - Alan's request, 2026-08-28 (see this method's own header comment
// for the full context/why).
void SoundbankBrowser::chooseSource() {
	auto *chooser = new juce::FileChooser(
		"Choose a folder of SysEx/MIDI files to scan, or pick individual files/.zip to add",
		processor.getLastDialogDir(), "*.syx;*.mid;*.midi;*.smf;*.zip");
	chooser->launchAsync(
		juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::canSelectDirectories
			| juce::FileBrowserComponent::canSelectMultipleItems,
		[this, chooser](const juce::FileChooser &fc) {
			const auto results = fc.getResults();
			if (results.isEmpty()) {
				delete chooser;
				return;
			}
			processor.setLastDialogDir(results[0].getParentDirectory());

			// A single folder, picked alone - same as the Utility tab's own long-standing
			// picker: point the configured source straight at it, no copying. (If a folder
			// happens to be mixed in among several files in one multi-select, it's simplest and
			// least surprising to just treat the whole result as the files case below and skip
			// the folder entry there, rather than silently also repointing the source folder as
			// a side effect of an ostensibly file-focused pick.)
			if (results.size() == 1 && results[0].isDirectory()) {
				D110AudioProcessor::setSoundbankSourceFolder(results[0].getFullPathName());
				statusLabel.setText("Source folder set to \"" + results[0].getFullPathName()
				                         + "\" - hit RESCAN.",
				                     juce::dontSendNotification);
				repaint();
				delete chooser;
				return;
			}

			// One or more individual files (a loose .syx, or Alan's own reported gap - a bare
			// .zip - either alone or several at once) - copied into the same fixed staging
			// folder Android's own picker already uses (Main.cpp's copySoundbankFiles(),
			// D110AudioProcessor::getSoundbankImportsFolder()), which startRescan() now always
			// scans in addition to the configured source folder - see its own comment. Never
			// touches getSoundbankSourceFolder() itself, so this can't silently make RESCAN
			// forget an existing big library folder that was already configured.
			const auto dest = D110AudioProcessor::getSoundbankImportsFolder();
			dest.createDirectory();
			int copied = 0;
			for (const auto &f : results) {
				if (f.isDirectory() || !f.hasFileExtension("syx;mid;midi;smf;zip")) continue;
				if (f.copyFileTo(dest.getChildFile(f.getFileName()))) ++copied;
			}
			statusLabel.setText(copied > 0
			                         ? juce::String(copied) + " file(s) added - hit RESCAN to add "
			                               "them to the database."
			                         : juce::String("No SysEx/MIDI/.zip files in that selection."),
			                     juce::dontSendNotification);
			repaint();
			delete chooser;
		});
}
#endif

void SoundbankBrowser::startRescan() {
	if (rescanThread != nullptr) return; // already running

	// Two roots, always - Alan's request, 2026-08-28: the user's own configured source folder
	// (an existing personal library, typically) AND the fixed staging folder individually-picked
	// files/zips land in (chooseSource() below on desktop, Main.cpp's copySoundbankFiles() on
	// Android) - never one replacing the other, so picking a loose file/zip can't silently make
	// RESCAN forget a big folder-based library that was already configured, and vice versa.
	juce::Array<juce::File> folders;
	const auto configured = D110AudioProcessor::getSoundbankSourceFolder();
	if (configured.isNotEmpty() && juce::File(configured).isDirectory())
		folders.add(juce::File(configured));
	const auto imports = D110AudioProcessor::getSoundbankImportsFolder();
	if (imports.isDirectory()) folders.add(imports);

	if (folders.isEmpty()) {
		statusLabel.setText("Choose your SysEx files/folder first.",
		                     juce::dontSendNotification);
		return;
	}

	rescanButton.setEnabled(false);
	statusLabel.setText("Scanning...", juce::dontSendNotification);
	rescanThread = std::make_unique<RescanThread>(d110bank::Database::defaultRoot(), std::move(folders));
	rescanThread->startThread();
	pollTimer.startTimer(150);
}

void SoundbankBrowser::checkRescanProgress() {
	if (rescanThread == nullptr || !rescanThread->finished.load(std::memory_order_acquire)) return;

	const int added = rescanThread->added.load(std::memory_order_acquire);
	rescanThread->waitForThreadToExit(2000);
	rescanThread.reset();
	pollTimer.stopTimer();
	rescanButton.setEnabled(true);

	// The background instance already wrote a fresh index.json - this just re-reads it, on the
	// message thread, the only thread ever allowed to touch the SHARED Database (see
	// RescanThread's own comment above for why).
	processor.getSoundbankDatabase().load();
	recomputeDuplicateHidden();
	rebuildFilteredList();
	// Alan's request, 2026-08-28: report how many sounds a scan found, both new-this-time and
	// the running total.
	const int total = processor.getSoundbankDatabase().size();
	statusLabel.setText("Scan complete: " + juce::String(added) + " new sound"
	                         + (added == 1 ? juce::String() : juce::String("s")) + " added ("
	                         + juce::String(total) + " total).",
	                     juce::dontSendNotification);
	repaint();
}

void SoundbankBrowser::showInjectMenu() {
	if (selectedEntry == nullptr) return;
	showInjectMenuFor(*selectedEntry);
}

void SoundbankBrowser::showInjectMenuFor(const d110bank::Entry &entryRef) {
	// Copied, not referenced/pointed-to: this menu and its confirm dialog are both async and
	// can easily outlive `entryRef`'s own backing storage - a background RESCAN finishing
	// while this menu is still open calls Database::load(), which clears and rebuilds the
	// shared `entries` vector `entryRef` (and anything pointing into it) lives in. Cheap copy,
	// avoids a dangling-pointer crash of exactly the kind already found and fixed once this
	// session (see RescanThread's own comment above).
	const d110bank::Entry entry = entryRef;

	// Live internal Tone Memory names, for a "which slot am I overwriting" menu - same RAM
	// path the TONES tab itself reads (D110EditorPane::toneName(), PluginEditor.cpp), just
	// without that class's own cached snapshot since this component isn't part of it.
	std::vector<juce::uint8> ram(size_t(D110CoreType::kRamSize), 0);
	const bool haveRam = processor.getCore().getRam(ram.data());

	// Captures `ram`/`haveRam` BY VALUE, not by reference: this lambda is itself captured into
	// the async menu callback below and must stay valid after this function returns, long
	// after `ram`'s own stack storage would otherwise be gone.
	auto nameOfSlot = [ram, haveRam](int slot) -> juce::String {
		if (!haveRam) return {};
		const size_t base =
			size_t(D110CoreType::kRamTones) + size_t(slot) * size_t(D110CoreType::kToneMemRecord);
		juce::String name;
		for (int c = 0; c < D110CoreType::kNameChars; ++c) {
			const char ch = char(ram[base + size_t(c)]);
			if (ch < 32 || ch > 126) break;
			name << ch;
		}
		return name.trim();
	};

	juce::PopupMenu menu;
	for (int slot = 0; slot < D110CoreType::kNumTones; ++slot) {
		juce::String label = toneSlotLabel(slot);
		const auto name = nameOfSlot(slot);
		if (name.isNotEmpty()) label << "  " << name;
		menu.addItem(slot + 1, label);
	}

	menu.showMenuAsync(
		juce::PopupMenu::Options().withTargetComponent(injectButton), [this, entry, nameOfSlot](int result) {
			if (result <= 0) return;
			const int slot = result - 1;
			// Alan's request, 2026-08-28: never silently overwrite a slot he's using - confirm
			// first, naming exactly what would be lost.
			const auto currentName = nameOfSlot(slot);
			juce::AlertWindow::showAsync(
				juce::MessageBoxOptions()
					.withIconType(juce::MessageBoxIconType::WarningIcon)
					.withTitle("Overwrite tone?")
					.withMessage("This will overwrite " + toneSlotLabel(slot)
					             + (currentName.isNotEmpty() ? " (currently \"" + currentName + "\")"
					                                          : juce::String())
					             + " with \"" + entry.displayName + "\".")
					.withButton("Inject")
					.withButton("Cancel"),
				[this, entry, slot](int confirmResult) {
					if (confirmResult == 1) injectInto(entry, slot); // see AlertWindow::show()'s
					                                                 // own doc: 2 buttons, first
					                                                 // added returns 1
				});
		});
}

void SoundbankBrowser::injectInto(const d110bank::Entry &entry, int slot) {
	juce::uint8 data[256];
	if (!d110bank::Database::readToneBytes(entry, data)) {
		statusLabel.setText("Could not read \"" + entry.displayName + "\" - its file may be missing.",
		                     juce::dontSendNotification);
		return;
	}
	processor.injectSoundbankTone(slot, data);
	statusLabel.setText("Injected \"" + entry.displayName + "\" into " + toneSlotLabel(slot) + ".",
	                     juce::dontSendNotification);
}

// Right-click on a row - Alan's request, 2026-08-28.
void SoundbankBrowser::showContextMenuFor(const d110bank::Entry &entryRef) {
	selectedEntry = &entryRef;
	injectButton.setEnabled(true);
	repaint();

	auto &favorites = processor.getSoundbankFavorites();
	const bool isFav = favorites.contains(entryRef);
	// Copied, not pointed-to - this menu is async and can outlive entryRef's own backing
	// storage (a background rescan finishing while it's open reloads the shared Database's
	// `entries` vector) - see showInjectMenuFor()'s own comment for the same fix, same reason.
	const d110bank::Entry entry = entryRef;

	juce::PopupMenu menu;
	menu.addItem(isFav ? "Remove from Favorites" : "Add to Favorites", [this, entry] {
		processor.getSoundbankFavorites().toggle(entry);
		exportFavoritesButton.setEnabled(processor.getSoundbankFavorites().size() > 0);
		// FAVORITES may be the currently shown group, or a just-removed entry may need to
		// disappear from it immediately either way.
		rebuildFilteredList();
		repaint();
	});

	juce::PopupMenu sendMenu;
	for (int p = 0; p < 8; ++p)
		sendMenu.addItem("Part " + juce::String(p + 1),
		                  [this, entry, p] { auditionEntryToPart(entry, p); });
	menu.addSubMenu("Send to", sendMenu);

	menu.addItem("Inject to slot...", [this, entry] { showInjectMenuFor(entry); });

	// Alan's request, 2026-08-28, two rounds: write a browsed tone to a REAL connected D-110,
	// not just this emulator - mirrors the LOCAL "Send to"/"Inject to slot..." split just above
	// (a Part, for instant non-destructive audition "comme le font la plupart des éditeurs",
	// his own words for the follow-up ask; or a Tone Memory slot, permanent) - see
	// sendToExternalPart()/showExternalInjectMenuFor()'s own comments.
	juce::PopupMenu externalMenu;
	for (int p = 0; p < 8; ++p)
		externalMenu.addItem("Part " + juce::String(p + 1),
		                      [this, entry, p] { sendToExternalPart(entry, p); });
	externalMenu.addSeparator();
	externalMenu.addItem("Inject to slot...", [this, entry] { showExternalInjectMenuFor(entry); });
	menu.addSubMenu("Send to real D-110", externalMenu);

	menu.addSeparator();
	menu.addItem("Rename...", [this, entry] { promptRenameEntry(entry); });
	menu.addItem("Delete", [this, entry] { confirmDeleteEntry(entry); });

	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

// "Send to real D-110" > "Part N" - Alan's request, 2026-08-28: the external-hardware
// equivalent of auditionEntryToPart() (double-click, or the LOCAL "Send to" submenu) - writes
// straight into that Part's Tone Temporary on the real unit over MIDI Out, instantly audible,
// no Tone Memory slot spent, exactly like the local audition path. See
// D110AudioProcessor::sendSoundbankToneToExternalMidiPart()'s own comment for why this can't
// reuse sendSoundbankToneToExternalMidi() (that one targets Tone MEMORY, address/stride apart).
void SoundbankBrowser::sendToExternalPart(const d110bank::Entry &entry, int part) {
	juce::uint8 data[256];
	if (!d110bank::Database::readToneBytes(entry, data)) {
		statusLabel.setText("Could not read \"" + entry.displayName + "\" - its file may be missing.",
		                     juce::dontSendNotification);
		repaint();
		return;
	}
	const bool ok = processor.sendSoundbankToneToExternalMidiPart(part, data);
	statusLabel.setText(ok ? "Sent \"" + entry.displayName + "\" to Part " + juce::String(part + 1)
	                              + " on the real D-110."
	                        : "No MIDI Out device selected - pick one first, then try again.",
	                     juce::dontSendNotification);
	repaint();
}

void SoundbankBrowser::showExternalInjectMenuFor(const d110bank::Entry &entryRef) {
	const d110bank::Entry entry = entryRef; // see showContextMenuFor()'s own comment - same fix

	if (!processor.hasExternalMidiOutput()) {
		statusLabel.setText("No MIDI Out device selected - pick one first (MIDI Output menu), "
		                     "then try again.",
		                     juce::dontSendNotification);
		repaint();
		return;
	}

	// No name lookup here, unlike showInjectMenuFor()'s own slot menu - there's no RAM to read
	// a real remote unit's current slot contents from, only this emulator's own.
	juce::PopupMenu menu;
	for (int slot = 0; slot < D110CoreType::kNumTones; ++slot) menu.addItem(slot + 1, toneSlotLabel(slot));

	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this, entry](int result) {
		if (result <= 0) return;
		const int slot = result - 1;
		juce::uint8 data[256];
		if (!d110bank::Database::readToneBytes(entry, data)) {
			statusLabel.setText("Could not read \"" + entry.displayName + "\" - its file may be missing.",
			                     juce::dontSendNotification);
			repaint();
			return;
		}
		const bool ok = processor.sendSoundbankToneToExternalMidi(slot, data);
		statusLabel.setText(ok ? "Sent \"" + entry.displayName + "\" to " + toneSlotLabel(slot)
		                              + " on the real D-110."
		                        : "No MIDI Out device selected - pick one first, then try again.",
		                     juce::dontSendNotification);
		repaint();
	});
}

void SoundbankBrowser::resized() {
	auto area = getLocalBounds().toFloat().reduced(4.0f);

	auto top = area.removeFromTop(26.0f);
	clearSearchButton.setBounds(top.removeFromRight(28.0f).toNearestInt());
	top.removeFromRight(2.0f);
	searchBox.setBounds(top.toNearestInt());
	area.removeFromTop(4.0f);

	// Database-management row - Alan's request, 2026-08-30: hide-duplicates toggle plus
	// backup/restore, kept apart from the bottom action row (already crowded with per-tone
	// actions) since these three act on the database as a whole.
	auto toolsRow = area.removeFromTop(24.0f);
	area.removeFromTop(4.0f);
	hideDuplicatesButton.setBounds(toolsRow.removeFromLeft(150.0f).reduced(2.0f).toNearestInt());
	toolsRow.removeFromLeft(4.0f);
	backupButton.setBounds(toolsRow.removeFromLeft(90.0f).reduced(2.0f).toNearestInt());
	toolsRow.removeFromLeft(4.0f);
	restoreButton.setBounds(toolsRow.removeFromLeft(90.0f).reduced(2.0f).toNearestInt());

	// Which Part a double-click auditions into - see the `selectedPart` member's own comment.
	auto partRow = area.removeFromTop(24.0f);
	// Widened from 4 to 14, Alan's report, 2026-08-28 ("pourquoi Part 5 et pas 1?") - reproduced
	// live on his own build: a touch aimed at the FIRST row of the tone list, just a few px too
	// high, silently landed in this PART row instead (pickPart() acts immediately on
	// mouseDown, unlike a list row's own tap-vs-drag deferral) with no visible error - the
	// selection just quietly changed and nothing else looked wrong. A 4px gap is well inside
	// normal fingertip imprecision; this trades a little vertical space for a safety margin a
	// touch is unlikely to overshoot by accident.
	area.removeFromTop(14.0f);
	partStripArea = partRow;
	partRow.removeFromLeft(50.0f); // room for the "PART" label, painted separately
	partBounds.clear();
	const float partBtnW = partRow.getWidth() / 8.0f;
	for (int p = 0; p < 8; ++p)
		partBounds.push_back(
			{ partRow.getX() + float(p) * partBtnW, partRow.getY(), partBtnW, partRow.getHeight() });

	auto bottom = area.removeFromBottom(28.0f);
	injectButton.setBounds(bottom.removeFromRight(150.0f).reduced(2.0f).toNearestInt());
	exportFavoritesButton.setBounds(bottom.removeFromRight(150.0f).reduced(2.0f).toNearestInt());
	rescanButton.setBounds(bottom.removeFromRight(90.0f).reduced(2.0f).toNearestInt());
#if !JUCE_ANDROID
	chooseSourceButton.setBounds(bottom.removeFromRight(170.0f).reduced(2.0f).toNearestInt());
#endif
	statusLabel.setBounds(bottom.toNearestInt());
	area.removeFromBottom(4.0f);

	groupStripArea = area.removeFromLeft(kGroupStripWidth);
	area.removeFromLeft(4.0f);
	scrollbarArea = area.removeFromRight(kScrollbarWidth);
	area.removeFromRight(2.0f);
	listArea = area;

	// Alan's request, 2026-08-28: 29 entries (ALL/A-Z/0-9/_) in one narrow column read too
	// small. Column-major fill (A,B,... fills the first column top-to-bottom before starting
	// the second) reads the same order a single column would, just wrapped - the same layout a
	// phone contacts app uses for an alphabet strip. ALL and FAVORITES (both longer labels than
	// a single letter, and the two most likely to be reached for first) get their own full-width
	// row each, above the 3-column letter grid, rather than being squeezed into one narrow
	// column cell where "FAVORITES (n)" doesn't fit.
	layoutGroups();
}

void SoundbankBrowser::layoutGroups() {
	groupBounds.clear();
	// ALL and FAVORITES keep their full-width rows; the letters wrap column-major under them. Rows
	// never shrink below kMinGroupRowH - a short window scrolls instead of overlapping labels.
	constexpr float kMinGroupRowH = 22.0f;
	constexpr float kTrackW = 8.0f;
	const float wideRowH = 26.0f;
	const int letterCount = groupKeys.size() - 2; // everything after ALL/FAVORITES
	const int rows = juce::jmax(1, (letterCount + kGroupColumns - 1) / kGroupColumns);

	auto stripArea = groupStripArea;
	const float idealRowH = juce::jmin(28.0f, (stripArea.getHeight() - 2.0f * wideRowH) / float(rows));
	const float groupRowH = juce::jmax(kMinGroupRowH, idealRowH);
	groupContentH = 2.0f * wideRowH + float(rows) * groupRowH;
	const bool scrolls = groupContentH > stripArea.getHeight() + 0.5f;
	if (scrolls) stripArea.removeFromRight(kTrackW + 2.0f);
	const float maxScroll = scrolls ? groupContentH - groupStripArea.getHeight() : 0.0f;
	groupScroll = juce::jlimit(0.0f, maxScroll, groupScroll);

	const float x0 = stripArea.getX(), y0 = stripArea.getY() - groupScroll;
	groupBounds.push_back({ x0, y0, stripArea.getWidth(), wideRowH });                 // ALL
	groupBounds.push_back({ x0, y0 + wideRowH, stripArea.getWidth(), wideRowH });      // FAVORITES
	const float colW = stripArea.getWidth() / float(kGroupColumns);
	for (int i = 0; i < letterCount; ++i) {
		const int col = i / rows;
		const int row = i % rows;
		groupBounds.push_back({ x0 + float(col) * colW, y0 + 2.0f * wideRowH + float(row) * groupRowH, colW,
		                         groupRowH });
	}

	if (scrolls) {
		groupScrollTrack = { groupStripArea.getRight() - kTrackW, groupStripArea.getY(), kTrackW,
		                     groupStripArea.getHeight() };
		const float thumbH = juce::jmax(24.0f, groupStripArea.getHeight() * groupStripArea.getHeight() / groupContentH);
		const float thumbY = groupScrollTrack.getY()
		                   + (groupScrollTrack.getHeight() - thumbH) * (groupScroll / maxScroll);
		groupScrollThumb = { groupScrollTrack.getX(), thumbY, kTrackW, thumbH };
	} else {
		groupScrollTrack = groupScrollThumb = {};
	}
}

void SoundbankBrowser::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);

	// Alan's report, 2026-08-28: statusLabel (bottom-left - "No soundbank scanned yet...", scan
	// results, etc.) relied on the shared LookAndFeel's default Label colour (pal.value, the
	// accent green) like every other Label in the app - fine for a short bold VALUE next to its
	// own label, but a whole sentence of small body text in that same accent colour measured out
	// as genuinely low-contrast against panelBg on his real device/theme combination ("presque
	// invisible" in Light - confirmed by sampling actual on-device pixels: text ~(75,98,34) on a
	// background around (176,163,152), a ~1.8:1 contrast ratio, well under readable). Computed
	// fresh from the CURRENT panelBg every repaint (same live-theme-tracking pattern the rest of
	// this file already follows) via Colour::contrasting(), which picks near-black or near-white
	// based on panelBg's own brightness - guaranteed strong contrast by construction rather than
	// another hand-picked hex value that might not hold up on some other display/theme
	// combination either.
	statusLabel.setColour(juce::Label::textColourId, pal.panelBg.contrasting(0.9f));

	const auto &db = processor.getSoundbankDatabase();

	g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
	g.setColour(pal.label);
	g.drawText("PART", partStripArea.withWidth(46.0f), juce::Justification::centredLeft);
	g.setFont(juce::Font(juce::FontOptions(13.0f)));
	for (int p = 0; p < int(partBounds.size()); ++p) {
		const auto &b = partBounds[size_t(p)];
		const bool selected = p == selectedPart;
		g.setColour(selected ? pal.seqActiveFill : pal.box);
		g.fillRect(b.reduced(1.0f));
		g.setColour(pal.boxBorder);
		g.drawRect(b, 0.5f);
		g.setColour(selected ? pal.seqActiveText : pal.value);
		g.drawText(juce::String(p + 1), b, juce::Justification::centred);
	}

	g.setFont(juce::Font(juce::FontOptions(13.0f)));
	g.saveState();
	g.reduceClipRegion(groupStripArea.getSmallestIntegerContainer());
	for (int i = 0; i < groupKeys.size() && i < int(groupBounds.size()); ++i) {
		const auto &b = groupBounds[size_t(i)];
		const bool selected = groupKeys[i] == selectedGroup;
		g.setColour(selected ? pal.seqActiveFill : pal.box);
		g.fillRect(b.reduced(1.0f));
		g.setColour(pal.boxBorder);
		g.drawRect(b, 0.5f);
		g.setColour(selected ? pal.seqActiveText : pal.value);
		const int count = groupKeys[i] == "ALL"       ? db.size()
		                 : groupKeys[i] == "FAVORITES" ? processor.getSoundbankFavorites().size()
		                                                : db.countForLetter(groupKeys[i]);
		g.drawText(groupKeys[i] + " (" + juce::String(count) + ")", b.reduced(4.0f, 0.0f),
		           juce::Justification::centredLeft);
	}
	g.restoreState();
	if (!groupScrollTrack.isEmpty()) {
		g.setColour(pal.box);
		g.fillRoundedRectangle(groupScrollTrack, 3.0f);
		g.setColour(pal.boxBorder.brighter(0.4f));
		g.fillRoundedRectangle(groupScrollThumb.reduced(1.0f), 3.0f);
	}

	g.setColour(pal.box);
	g.fillRect(listArea);
	g.setColour(pal.boxBorder);
	g.drawRect(listArea, 0.5f);

	// Alan's request, 2026-08-28: a single column left most of a wide window's width empty
	// ("espace perdu"). Flows into as many columns as fit (row-major - left to right, then
	// wrap - so scrolling moves every column together as one grid, per his own question "ça
	// doit pouvoir scroller sur plusieurs colonnes en même temps" - yes, one scroll position
	// for the whole grid, not one per column).
	rowBounds.clear();
	rowEntries.clear();
	listColumns = juce::jmax(1, int(listArea.getWidth() / kListColumnMinWidth));
	const float colW = listArea.getWidth() / float(listColumns);
	visibleGridRows = juce::jmax(0, int(listArea.getHeight() / kRowHeight));
	const int totalGridRows = (int(filtered.size()) + listColumns - 1) / listColumns;
	listScrollRow = juce::jlimit(0, juce::jmax(0, totalGridRows - visibleGridRows), listScrollRow);

	g.setFont(juce::Font(juce::FontOptions(14.0f))); // grossi - Alan's request, 2026-08-28
	for (int gridRow = 0; gridRow < visibleGridRows; ++gridRow) {
		for (int col = 0; col < listColumns; ++col) {
			const int index = (listScrollRow + gridRow) * listColumns + col;
			if (index >= int(filtered.size())) continue;
			const auto *entry = filtered[size_t(index)];
			const juce::Rectangle<float> cellRect(listArea.getX() + float(col) * colW,
			                                       listArea.getY() + float(gridRow) * kRowHeight, colW,
			                                       kRowHeight);
			rowBounds.push_back(cellRect);
			rowEntries.push_back(entry);
			const bool selected = entry == selectedEntry;
			if (selected) {
				g.setColour(pal.seqActiveFill);
				g.fillRect(cellRect);
			}
			g.setColour(selected ? pal.seqActiveText : pal.value);
			const bool isFav = processor.getSoundbankFavorites().contains(*entry);
			g.drawText((isFav ? juce::String(juce::CharPointer_UTF8("\xe2\x98\x85 ")) : juce::String())
			               + entry->displayName,
			           cellRect.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
		}
	}

	if (filtered.empty() && db.size() > 0) {
		g.setColour(pal.dim);
		g.drawText("No match.", listArea.reduced(6.0f), juce::Justification::centred);
	}

	// Ascenseur (Alan's request, 2026-08-28: the mouse wheel was the only way to scroll, not
	// discoverable and not usable at all on Android, which has no wheel). Only drawn/interactive
	// when there's actually more content than fits - an empty/short list gets no thumb, nothing
	// to drag.
	g.setColour(pal.box);
	g.fillRect(scrollbarArea);
	g.setColour(pal.boxBorder);
	g.drawRect(scrollbarArea, 0.5f);
	scrollbarThumb = {};
	if (totalGridRows > visibleGridRows && visibleGridRows > 0) {
		const float trackH = scrollbarArea.getHeight();
		const float thumbH = juce::jmax(24.0f, trackH * float(visibleGridRows) / float(totalGridRows));
		const int maxScroll = totalGridRows - visibleGridRows;
		const float thumbY = scrollbarArea.getY()
		                    + (trackH - thumbH) * (maxScroll > 0 ? float(listScrollRow) / float(maxScroll) : 0.0f);
		scrollbarThumb = { scrollbarArea.getX() + 2.0f, thumbY, scrollbarArea.getWidth() - 4.0f, thumbH };
		g.setColour(draggingThumb ? pal.seqActiveFill : pal.value.withAlpha(0.6f));
		g.fillRoundedRectangle(scrollbarThumb, 3.0f);
	}
}

void SoundbankBrowser::mouseDown(const juce::MouseEvent &e) {
	// Alan's report, 2026-08-28: clicking a tone (or a group/part button) left keyboard focus
	// sitting in the search box from an earlier click into it, so typing on the on-screen
	// keyboard's PC-tracker input afterwards silently typed into the search field instead of
	// playing notes. Every click this handler ever sees is already OUTSIDE the search box (a
	// click landing ON it is handled by TextEditor's own mouseDown first, which grabs focus
	// INTO it - that path is untouched, still works normally), so parking focus on this
	// Component itself here is safe and unconditional - it does nothing with the keyboard
	// itself, so there's nowhere for a stray keystroke to go missing into.
	grabKeyboardFocus();

	const auto pos = e.position;

	if (scrollbarThumb.contains(pos)) {
		draggingThumb = true;
		thumbDragGrabOffsetY = pos.y - scrollbarThumb.getY();
		repaint();
		return;
	}
	if (scrollbarArea.contains(pos)) {
		// Clicked the track itself, outside the thumb - page toward the click, like any normal
		// scrollbar track click.
		const int page = juce::jmax(1, visibleGridRows);
		listScrollRow += pos.y < scrollbarThumb.getY() ? -page : page;
		listScrollRow = juce::jmax(0, listScrollRow);
		repaint();
		return;
	}

	if (!groupScrollTrack.isEmpty() && groupScrollTrack.contains(pos)) {
		if (groupScrollThumb.contains(pos)) {
			draggingGroupScroll = true;
			groupDragStartY = pos.y;
			groupDragStartScroll = groupScroll;
		} else {
			groupScroll += (pos.y < groupScrollThumb.getY() ? -1.0f : 1.0f) * groupStripArea.getHeight() * 0.8f;
			layoutGroups();
			repaint();
		}
		return;
	}

	for (int i = 0; i < int(groupBounds.size()); ++i)
		if (groupStripArea.contains(pos) && groupBounds[size_t(i)].contains(pos)) {
			pickGroup(groupKeys[i]);
			return;
		}

	for (int i = 0; i < int(partBounds.size()); ++i)
		if (partBounds[size_t(i)].contains(pos)) {
			pickPart(i);
			return;
		}

	if (e.mods.isPopupMenu()) {
		for (int i = 0; i < int(rowBounds.size()); ++i)
			if (rowBounds[size_t(i)].contains(pos)) {
				showContextMenuFor(*rowEntries[size_t(i)]);
				return;
			}
		return;
	}

	// Row selection itself is deferred to mouseUp (see trackingListDrag's own comment below) -
	// picking it up here unconditionally would select a row the instant a touch-drag-to-scroll
	// gesture merely started on top of one.
	if (listArea.contains(pos)) {
		trackingListDrag = true;
		listDragIsScroll = false;
		listDragStartPos = pos;
		listDragStartScrollRow = listScrollRow;

		// Long-press-to-context-menu - Alan's report, 2026-08-28: Android has no right mouse
		// button, so the row context menu (Favorites/Send to/Inject to slot/Send to real
		// D-110) was unreachable there entirely. Touch-only (`e.source.isTouch()`): a real
		// mouse already has right-click, and holding the left button for half a second there
		// would be a surprising way to also get a menu.
		if (e.source.isTouch()) {
			for (int i = 0; i < int(rowBounds.size()); ++i) {
				if (!rowBounds[size_t(i)].contains(pos)) continue;
				const int gestureId = ++longPressGestureId;
				const d110bank::Entry entry = *rowEntries[size_t(i)]; // copy - see handleLongPress()
				juce::Component::SafePointer<SoundbankBrowser> safeThis(this);
				juce::Timer::callAfterDelay(500, [safeThis, gestureId, entry] {
					if (safeThis != nullptr) safeThis->handleLongPress(gestureId, entry);
				});
				break;
			}
		}
	}
}

// Fires ~500ms after a touch-down inside the list, unless the gesture already resolved into
// something else first (a scroll, a release/tap, or a second gesture starting) - each of those
// either bumps `longPressGestureId` (invalidating this call) or clears `trackingListDrag`
// (mouseUp already handled it), both checked here. `entry` is a value copy taken at the moment
// the touch started, same dangling-pointer-avoidance reason showContextMenuFor()/
// showInjectMenuFor() already copy for their own async menus.
void SoundbankBrowser::handleLongPress(int gestureId, d110bank::Entry entry) {
	if (gestureId != longPressGestureId) return; // a newer gesture started since
	if (!trackingListDrag || listDragIsScroll) return; // already released, or turned into a scroll
	trackingListDrag = false; // consumed - mouseUp must not also treat this as a tap-select
	showContextMenuFor(entry);
}

void SoundbankBrowser::mouseDrag(const juce::MouseEvent &e) {
	const auto pos = e.position;

	if (draggingGroupScroll) {
		const float maxScroll = juce::jmax(1.0f, groupContentH - groupStripArea.getHeight());
		const float usable = juce::jmax(1.0f, groupScrollTrack.getHeight() - groupScrollThumb.getHeight());
		groupScroll = groupDragStartScroll + (pos.y - groupDragStartY) * (maxScroll / usable);
		layoutGroups();
		repaint();
		return;
	}

	if (draggingThumb) {
		const int totalGridRows = (int(filtered.size()) + listColumns - 1) / listColumns;
		const int maxScroll = juce::jmax(0, totalGridRows - visibleGridRows);
		const float usableTrack = juce::jmax(1.0f, scrollbarArea.getHeight() - scrollbarThumb.getHeight());
		const float thumbY = pos.y - thumbDragGrabOffsetY - scrollbarArea.getY();
		const float fraction = juce::jlimit(0.0f, 1.0f, thumbY / usableTrack);
		listScrollRow = juce::roundToInt(fraction * float(maxScroll));
		repaint();
		return;
	}

	if (!trackingListDrag) return;

	// A few pixels of wobble before a touch/click counts as a scroll drag rather than a tap -
	// touch input is never perfectly still (Alan's request, 2026-08-28: Android has no mouse
	// wheel, dragging the list is the natural touch gesture to scroll it).
	const float deltaY = pos.y - listDragStartPos.y;
	if (!listDragIsScroll && std::abs(deltaY) > 6.0f) listDragIsScroll = true;
	if (listDragIsScroll) {
		listScrollRow = juce::jmax(0, listDragStartScrollRow - juce::roundToInt(deltaY / kRowHeight));
		repaint();
	}
}

void SoundbankBrowser::mouseUp(const juce::MouseEvent &e) {
	if (draggingGroupScroll) {
		draggingGroupScroll = false;
		return;
	}
	if (draggingThumb) {
		draggingThumb = false;
		repaint();
	}

	if (!trackingListDrag) return;
	const bool wasScroll = listDragIsScroll;
	trackingListDrag = false;
	listDragIsScroll = false;
	if (wasScroll) {
		repaint();
		return;
	}

	// A genuine tap, not a drag - select the row under it (the behaviour mouseDown used to
	// provide directly, before it had to start deferring to tell a tap from a scroll gesture).
	const auto pos = e.position;
	for (int i = 0; i < int(rowBounds.size()); ++i) {
		if (!rowBounds[size_t(i)].contains(pos)) continue;
		selectedEntry = rowEntries[size_t(i)];
		injectButton.setEnabled(true);
		repaint();
		return;
	}
}

// Quick-audition path, Alan's request 2026-08-28: double-click a sound and it goes straight
// into the SELECTED PART's live/working tone (D110AudioProcessor::auditionToneBytes()) -
// immediately audible, exactly "comme quand on pick un tone dans Parts, ça devient
// l'instrument courant" (Alan's own comparison). No Tone Memory slot is spent doing this -
// unlike "Inject to slot...", this writes straight into Tone Temporary, so it's free to
// audition as many sounds as you like without touching any of the 64 stored slots.
void SoundbankBrowser::mouseDoubleClick(const juce::MouseEvent &e) {
	const auto pos = e.position;
	for (int i = 0; i < int(rowBounds.size()); ++i) {
		if (!rowBounds[size_t(i)].contains(pos)) continue;
		const auto *entry = rowEntries[size_t(i)];
		selectedEntry = entry;
		injectButton.setEnabled(true);
		auditionEntryToPart(*entry, selectedPart);
		return;
	}
}

// Corruption fix (Alan's report, 2026-08-28: garbled LCD, wrong sound on double-click).
// Tone Temporary is NOT "the record minus its leading 10-byte name" - it mirrors the record's
// own first 246 bytes UNCHANGED, name included, exactly what the already-proven
// auditionTone(part, slot) (PluginProcessor.cpp) copies (`ram.data() + kRamTones +
// slot*kToneMemRecord`, no offset - only the record's LAST 10 bytes, 246-255, are ever
// excluded). Skipping the first 10 bytes here instead shifted every real parameter after it by
// 10 bytes, corrupting the whole tone.
void SoundbankBrowser::auditionEntryToPart(const d110bank::Entry &entry, int part) {
	juce::uint8 data[256];
	if (!d110bank::Database::readToneBytes(entry, data)) {
		statusLabel.setText("Could not read \"" + entry.displayName + "\" - its file may be missing.",
		                     juce::dontSendNotification);
		repaint();
		return;
	}
	processor.auditionToneBytes(part, data);
	statusLabel.setText("Playing \"" + entry.displayName + "\" on Part " + juce::String(part + 1) + ".",
	                     juce::dontSendNotification);
	if (onToneAuditioned) onToneAuditioned(part);
	repaint();
}

void SoundbankBrowser::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
	if (groupStripArea.contains(e.position)) {
		if (!groupScrollTrack.isEmpty()) {
			groupScroll -= wheel.deltaY * 120.0f;
			layoutGroups();
			repaint();
		}
		return;
	}
	listScrollRow -= juce::roundToInt(wheel.deltaY * 4.0f);
	listScrollRow = juce::jmax(0, listScrollRow);
	repaint();
}

// Arrow-key navigation (Alan's request, 2026-08-28: the mouse wheel was the only way to move
// through the list at all - unusable on Android, and not discoverable even on desktop). Moves
// the selection through the same row-major grid paint() lays out (Up/Down by one grid row =
// `listColumns` entries, Left/Right by one entry, Page Up/Down by a full screen), auto-scrolling
// to keep the new selection visible exactly the way any normal list view does. Also auditions
// the newly-selected tone (2026-08-30 follow-up - see the `selectionChanged` block below).
bool SoundbankBrowser::keyPressed(const juce::KeyPress &key) {
	if (filtered.empty()) return false;

	int index = -1;
	if (selectedEntry != nullptr) {
		const auto it = std::find(filtered.begin(), filtered.end(), selectedEntry);
		if (it != filtered.end()) index = int(it - filtered.begin());
	}
	const int page = listColumns * juce::jmax(1, visibleGridRows);

	int newIndex;
	if (key == juce::KeyPress::downKey) newIndex = (index < 0 ? -listColumns : index) + listColumns;
	else if (key == juce::KeyPress::upKey) newIndex = (index < 0 ? listColumns : index) - listColumns;
	else if (key == juce::KeyPress::rightKey) newIndex = (index < 0 ? -1 : index) + 1;
	else if (key == juce::KeyPress::leftKey) newIndex = (index < 0 ? 1 : index) - 1;
	else if (key == juce::KeyPress::pageDownKey) newIndex = (index < 0 ? -page : index) + page;
	else if (key == juce::KeyPress::pageUpKey) newIndex = (index < 0 ? page : index) - page;
	else return false;

	newIndex = juce::jlimit(0, int(filtered.size()) - 1, newIndex);
	const bool selectionChanged = selectedEntry != filtered[size_t(newIndex)];
	selectedEntry = filtered[size_t(newIndex)];
	injectButton.setEnabled(true);

	// Alan's request, 2026-08-30: moving the selection with the keyboard plays it immediately,
	// the same instant, non-destructive audition double-click already does - "comme un
	// double-clic, mais au clavier". Only on an actual move (not e.g. Up already at the first
	// row, which clamps to the same entry) so holding the key at either end doesn't keep
	// retriggering the same note.
	if (selectionChanged) auditionEntryToPart(*selectedEntry, selectedPart);

	const int row = newIndex / listColumns;
	if (row < listScrollRow) listScrollRow = row;
	else if (row >= listScrollRow + juce::jmax(1, visibleGridRows)) listScrollRow = row - visibleGridRows + 1;
	listScrollRow = juce::jmax(0, listScrollRow);

	repaint();
	return true;
}
