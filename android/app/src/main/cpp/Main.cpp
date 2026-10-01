// Second Android milestone: the real engine (D110AudioProcessor/D110CoreNative), the real
// photographed front panel (D110Panel) with its original hit-tested buttons, the real
// on-screen keyboard (D110Keyboard), and (2026-08-22) the real sequencer - both its normal
// grid view (D110SequencerPanel, the default here, matching the desktop plugin's own default)
// and its retro D-pad+LCD view (D110SequencerRetroPanel, one Options tap away) - all reused
// unchanged from the desktop plugin, since all of them are already touch-compatible as-is
// (see the Android port investigation this follows up on). The grid view's right-click menus
// (quantize, tempo, load/save, ...) have no right mouse button to fire them on a touchscreen,
// so D110SequencerPanel itself grew a long-press equivalent (see its own mouseDown() comment)
// reaching the exact same menus - the retro view never needed this, having none at all (its
// own D-pad/button cluster covers everything a real D-20 does, only in a smaller frame). A
// long PRESS still doesn't belong on the piano keys themselves (see D110Keyboard.h's own note
// on why holding a key can't double as one - it's an ordinary sustained note), which is why
// that menu stays reachable from the hamburger instead. Deliberately NOT included: the
// extended editor drawer (Utility/Tone/Patches/... tabs), the memory card - out of scope for
// "as simple as possible" per the original brief, and nothing added since has needed them.
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Source/PluginProcessor.h"
#include "Source/PluginEditor.h"
#include "Source/AppModeIcons.h"
#include "Source/D110Keyboard.h"
#include "Source/SoundbankBrowser.h"
#include "Source/UiTheme.h"
#include "Source/sequencer/D110SequencerGridPanel.h"
#include "Source/sequencer/D110SequencerPanel.h"
#include "Source/sequencer/D110SequencerRetroPanel.h"
#include "Source/sequencer/SequencerViewMenu.h"

// Soundbanks view's own test-note button (Alan's request, 2026-08-30: no on-screen keyboard is
// visible there - see resized()'s own comment on showingSoundbanks - so there was no way to
// actually hear a browsed tone without leaving the screen). Plays while held, like a real piano
// key: mouseDown -> note on, mouseUp -> note off, nothing more - the pitch it plays is set
// separately by the PITCH button next to it (wireUpSoundbankTestNote()'s own comment). This
// used to also fire a long-press-to-open-a-dialog gesture, dropped 2026-09-03 (Alan: this
// button is just "play continuously while held" now, no long press at all) once a dedicated
// PITCH button took over changing the note.
class HeldNoteButton : public juce::TextButton {
public:
	std::function<void()> onPress;
	std::function<void()> onRelease;

	void mouseDown(const juce::MouseEvent &e) override {
		juce::TextButton::mouseDown(e);
		if (onPress) onPress();
	}
	void mouseUp(const juce::MouseEvent &e) override {
		juce::TextButton::mouseUp(e);
		if (onRelease) onRelease();
	}
};

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiInputCallback {
public:
	// The app's own external files dir - the one location the native core can always read
	// with a plain filesystem path on Android (see chooseRomFolder()'s own comment for why
	// that rules out pointing setCustomRomFolder() at an arbitrary SAF folder directly).
	static juce::File romBringUpDir() {
		return juce::File("/storage/emulated/0/Android/data/com.d110emulator.android/files/roms");
	}

	MainComponent()
		: panel(processor), keyboard(processor), gridSeq(processor), pianoRollSeq(processor), retroSeq(processor),
		  soundbankBrowser(processor) {
		// Bring-up shortcut, still the path chooseRomFolder() (hamburger menu) now also copies
		// into - see its own comment. Originally ROMs only got there via `adb push`; the
		// picker below is the "proper SAF file-picker" this comment used to say was still
		// missing.
		if (romBringUpDir().isDirectory())
			D110AudioProcessor::setCustomRomFolder(romBringUpDir().getFullPathName());
		processor.reloadRomsAndPowerOn();
		loadPersistedState();
		// The piano roll's pitch rows are 14px on a desktop - far too thin for a finger. First
		// run (nothing persisted yet) starts at a touch-sized 28; the piano roll's own ROW
		// button changes it from there and the choice is saved with the rest of the state.
		if (processor.getGridRowHeight() == 0) processor.setGridRowHeight(28);

			// Theme (Options -> Theme), added 2026-08-28: the app never applied its own saved
			// uiThemeLight/uiThemeFollowSystem to d110ui at all before this (grep confirmed zero
			// prior d110ui:: use anywhere in this file) - D110Keyboard already reads
			// d110ui::palette() for its own colours, so it was silently stuck on the Dark default
			// this whole time. juce::LookAndFeel::setDefaultLookAndFeel is the same call
			// NonetSeqMain.cpp makes to theme stock JUCE chrome (the Options popup menu itself
			// among it) consistently with the hand-painted keyboard/sequencer.
			juce::LookAndFeel::setDefaultLookAndFeel(&d110ui::sharedLookAndFeel());
			applyThemeMode();

		// Full panel (kRefW=2124) squeezes to illegibility on a phone-portrait width - the LCD
		// and part indicators end up a few pixels tall. Compact mode (see D110Panel::kCompactRefW,
		// PluginEditor.h) splices out the purely decorative Roland wordmark/PHONES jack and the
		// MEMORY CARD slot - nothing this app needs (no memory card UI here at all) - leaving
		// VOLUME/LCD/buttons/POWER/MIDI lamp at a noticeably larger effective zoom for the same
		// screen width. resized() below reads currentRefW(true) to match.
		processor.setCompactPanelMode(true);

		addAndMakeVisible(panel);
		// Both hidden until the hamburger menu's "Sequencer" is picked; which of the two then
		// shows follows processor.getSequencerRetroMode() (default false - normal/grid),
		// the exact same flag and default the desktop editor's own Options menu uses.
		addChildComponent(gridSeq);
		addChildComponent(pianoRollSeq); // third view: piano roll, see processor.getSequencerGridMode()
		addChildComponent(retroSeq);
		// Same shared component the desktop plugin's own SOUNDBANKS tab hosts (SoundbankBrowser.h)
		// - reached from the hamburger menu instead of a tab, this app having no tab strip at all.
		addChildComponent(soundbankBrowser);
		wireUpSoundbankTestNote();
		// See buildAppMenu()'s own comment: the grid sequencer hides the app's whole
		// Play/Stop/hamburger row to get its full height, so its own bar-navigation menu
		// button becomes the only way back to it.
		gridSeq.onBarMenuButtonExtra = [this](juce::PopupMenu &m) { buildAppMenu(m); };
		// Same reason for the piano roll: it replaces the app's own hamburger row too.
		pianoRollSeq.onBarMenuButtonExtra = [this](juce::PopupMenu &m) { buildAppMenu(m); };
		addAndMakeVisible(keyboard);

		// The hamburger and the mode switch are drawn in the sequencer's own button style
		// (AppModeIcons.h) so they look the same in every view, including the two sequencer
		// panels that carry their own copies in their transport rows (onModeButton below).
		// Play/Stop for "Load MIDI file..." playback used to be real buttons in the front panel's
		// top row; they live in the hamburger menu now (Alan's request, 2026-10-01), right under
		// "Load MIDI file...".
		menuButton.onClick = [this] { showMainMenu(); };
		addAndMakeVisible(menuButton);
		modeButton.onClick = [this] { cycleMode(); };
		addAndMakeVisible(modeButton);
		gridSeq.onModeButton = [this] { cycleMode(); };
		gridSeq.modeButtonMode = [this] { return int(currentMode()); };
		pianoRollSeq.onModeButton = [this] { cycleMode(); };
		pianoRollSeq.modeButtonMode = [this] { return int(currentMode()); };

		statusLabel.setJustificationType(juce::Justification::centredLeft);
		statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
		statusLabel.setText(processor.isSynthReady() ? "Ready" : processor.getLastError(),
		                     juce::dontSendNotification);
		addAndMakeVisible(statusLabel);

		// AudioProcessorPlayer is what a JUCE Standalone build's own StandaloneFilterWindow
		// uses internally to bridge a real AudioProcessor to a live device - same idea here,
		// minus the desktop window chrome (title bar, audio-settings dialog, resize
		// constrainer) that doesn't correspond to anything on a fixed, OS-managed Activity
		// surface. Its audioDeviceAboutToStart() is what actually calls prepareToPlay(),
		// which calls setPoweredOn(true) - idempotent, so doing it twice (once explicitly
		// above via reloadRomsAndPowerOn(), once here once the device starts) is harmless.
		deviceManager.initialiseWithDefaultDevices(0, 2);
		player.setProcessor(&processor);
		deviceManager.addAudioCallback(&player);

		// No setSize() call needed here: MainWindow's setContentOwned()/setFullScreen(true)
		// (ResizableWindow::resized() -> setBoundsInset()) keep this synced to the actual
		// window bounds on every resize/rotation, so a hardcoded guess here would only ever
		// be a one-frame placeholder immediately overwritten.
		scanForMidiInputs(); // catches a keyboard already plugged in before launch immediately,
		                     // rather than waiting for the first periodic scan
		startTimerHz(30);
	}

	~MainComponent() override {
		saveState();
		deviceManager.removeAudioCallback(&player);
		player.setProcessor(nullptr);
		for (const auto &id : knownMidiInputs) {
			deviceManager.removeMidiInputDeviceCallback(id, this);
			deviceManager.setMidiInputDeviceEnabled(id, false);
		}
	}

	// Unlike the desktop Standalone target (a real juce::StandaloneFilterApp, which
	// autosaves/reloads processor.getStateInformation()/setStateInformation() through its own
	// settings file), this Android app is a bare JUCEApplication with nothing wired up to
	// either call at all - confirmed as the root cause of Alan's 2026-08-23 report that
	// quitting the Android app loses every song ("comme si on avait réinitialisé la
	// mémoire"). Fixed the same way NonetSeqHost persists its own settings: one flat file in
	// the app's private data dir holding the exact same binary blob getStateInformation()/
	// setStateInformation() already produce/consume everywhere else (sequencer songs/tracks,
	// tempo, retro key bindings, LCD mode, keyboard config, theme - literally everything that
	// blob covers). Loaded once here, right after reloadRomsAndPowerOn() has already booted
	// the firmware fresh - restoring firmware NVRAM bytes into files on disk after boot only
	// takes visible effect on the NEXT power-on, but the D-110 core already flushes its own
	// NVRAM to disk continuously during normal operation independent of this (see
	// project_standalone_nvram_persistence_fix in project memory), so the only thing this
	// call actually needs to restore here is the higher-level state - which it does
	// unconditionally, regardless of that NVRAM-timing nuance.
	//
	// Loading on construction isn't enough by itself: Android can (and does) kill this whole
	// process without warning once it's backgrounded, well before any orderly C++ destructor
	// chain would run - see D110AndroidApp::suspended(), the actual save point that matters,
	// which calls this too. The destructor above only covers the rarer case of a clean
	// in-app quit.
	void saveState() {
		juce::MemoryBlock data;
		processor.getStateInformation(data);
		stateFile().getParentDirectory().createDirectory();
		stateFile().replaceWithData(data.getData(), data.getSize());
	}

private:
	static juce::File stateFile() {
		return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
			.getChildFile("d110-state.bin");
	}

	void loadPersistedState() {
		auto f = stateFile();
		if (!f.existsAsFile()) return;
		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && data.getSize() > 0)
			processor.setStateInformation(data.getData(), (int) data.getSize());
	}

public:

	void paint(juce::Graphics &g) override { g.fillAll(juce::Colour(0xff1e1e22)); }

	void resized() override {
		// In landscape, the 3-button nav bar (back/home/recents) sits as a vertical strip
		// down one side of the screen rather than along the bottom, and since this window
		// draws edge-to-edge (see AndroidManifest's default window flags), that strip was
		// overlapping the right end of the panel/keyboard - specifically the keyboard's own
		// "+" octave button, unreachable underneath the system nav bar. safeAreaInsets is
		// JUCE's own cross-platform answer to exactly this (also covers notches/status bar,
		// though those don't collide with anything here) - insetting the whole layout by it
		// keeps every control clear of system UI on any device/orientation, not just this one.
		//
		// getDisplayForRect()'s live safeAreaInsets turned out too unreliable to build this on
		// (2026-08-22): occasionally null right after rotating or switching to/from the
		// sequencer, and a caching fix on top of that (remember the last successful reading,
		// reuse it whenever a fresh lookup failed) had its own gap - caching blindly across a
		// ROTATION could carry over a WRONG-SHAPED inset (landscape's right inset, reapplied
		// after rotating back to portrait, where the real nav bar has moved to the bottom) if
		// the live lookup happened to succeed-but-stale mid-rotation, or kept failing across a
		// full rotate-and-back cycle - worse than no inset, since it leaves controls sitting
		// under the real bar while looking like it should be clear of it. That's what Alan hit
		// ("apres un aller-retour ca deborde dans le taskbar, ca rend le sequenceur inutilisable").
		//
		// Replaced with something that has no state to go stale in the first place: a fixed
		// guess, recomputed fresh from isLandscape on every single resize, no live lookup at
		// all. Less precise than a real per-device measurement would be, but a comfortably
		// oversized guess costs a few dp of unused margin on devices that don't need it, where
		// a wrong live reading costs an unreachable button - the guess is the safer failure
		// mode. Alan also found this can't be fully auto-detected anyway: some Android tablets
		// keep the nav bar bottom-anchored even in landscape rather than moving it to a side
		// the way phones do, and there's no generic signal that distinguishes that from a
		// normal phone. navTop/Bottom/Left/Right below are the manual escape hatch - four
		// independent checkboxes (not a single "which side" choice) because a real device can
		// need two at once (status bar on top AND a bottom-anchored nav bar, even rotated).
		auto area = getLocalBounds();
		const bool isLandscape = getWidth() > getHeight();
		constexpr int kStatusBarInset = 32; // logical px - comfortably more than a status bar
		constexpr int kNavBarInset = 64;    // logical px - comfortably more than a 3-button/gesture bar
		juce::BorderSize<int> insets;
		if (navTop || navBottom || navLeft || navRight) {
			insets = { navTop ? kStatusBarInset : 0, navLeft ? kNavBarInset : 0,
			           navBottom ? kNavBarInset : 0, navRight ? kNavBarInset : 0 };
		} else {
			// Auto: the ordinary phone layout - status bar on top always, nav bar on the
			// "closing" edge for the current orientation (right in landscape, bottom in
			// portrait).
			insets = isLandscape ? juce::BorderSize<int>(kStatusBarInset, 0, 0, kNavBarInset)
			                     : juce::BorderSize<int>(kStatusBarInset, 0, kNavBarInset, 0);
		}
		area = insets.subtractedFrom(area);

		// Play/Stop/hamburger and the status line are hidden entirely while the GRID sequencer
		// is showing (Alan's request, 2026-08-22): it has its own STOP/PLAY/REC transport
		// already, so the app's copies were pure duplication, and landscape doesn't have height
		// to spare for duplication - every pixel they used to take goes to the sequencer
		// instead. The hamburger's own role (Load MIDI file/Panel switch/Keyboard channel/
		// Options) doesn't disappear with it: onBarMenuButtonExtra below feeds those same items
		// into the grid's own new bar-navigation menu button instead, so there's still exactly
		// one way to reach them, just relocated.
		//
		// Retro keeps the hamburger (it has no equivalent menu button of its own to relocate
		// Load MIDI file/Panel switch/etc onto - its whole design is a D-pad/button cluster
		// with no free-text menus at all, so hiding the app's own hamburger there would leave
		// no way back to Panel view). Play/Stop and the status line DO still hide in retro,
		// though (Alan's request, 2026-08-23): those are for the app's OWN "Load MIDI file..."
		// playback feature, not the sequencer transport - sitting right above retro's own
		// STOP/PLAY/REC, they read as duplicates of it even though they do something
		// completely unrelated, which is exactly the confusion Alan reported.
		// (Also true for the piano roll view: it has the same bar-menu button, fed by pianoRollSeq.onBarMenuButtonExtra.)
		const bool inGridSequencer = showingSequencer && !processor.getSequencerRetroMode();
		const bool inRetroSequencer = showingSequencer && processor.getSequencerRetroMode();
		// The two grid-style sequencer panels carry their own mode-switch and hamburger buttons
		// in their transport rows (same look, see AppModeIcons.h), so the app's top strip is
		// hidden for them to give the sequencer the whole height. Every other view - front panel,
		// soundbanks, retro sequencer - gets the strip with exactly those two buttons on its right.
		// Status line: front panel only (feedback for the MIDI-file player in the hamburger menu).
		menuButton.setVisible(!inGridSequencer);
		modeButton.setVisible(!inGridSequencer);
		modeButton.setMode(currentMode());
		statusLabel.setVisible(!inGridSequencer && !inRetroSequencer && !showingSoundbanks);
		// Visibility of the soundbank test-note/HOLD/PITCH buttons is decided here, outside the
		// strip block below: it used to live inside `if (!inGridSequencer)`, so leaving Soundbanks
		// for the grid/piano-roll sequencer never hid them and they covered its top rows.
		testNoteButton.setVisible(showingSoundbanks);
		holdButton.setVisible(showingSoundbanks);
		pitchButton.setVisible(showingSoundbanks);
		if (!inGridSequencer) {
			// Retro landscape keeps its compact 36px strip (Alan, 2026-08-24); everywhere else the
			// buttons match the grid sequencer's own bar-menu button: 40 tall, rowWidth * 0.042 - 4
			// wide, flush against the top-right corner.
			const bool compactStrip = inRetroSequencer && isLandscape;
			const int buttonH = compactStrip ? 36 : 40;
			auto transport = area.removeFromTop(buttonH);
			const int buttonW = compactStrip ? 36 : juce::jmax(28, juce::roundToInt(transport.getWidth() * 0.042f - 4.0f));
			menuButton.setBounds(transport.removeFromRight(buttonW));
			modeButton.setBounds(transport.removeFromRight(buttonW));
			if (showingSoundbanks) {
				const int btnW = juce::jmax(64, juce::roundToInt(transport.getWidth() * 0.16f));
				testNoteButton.setBounds(transport.removeFromLeft(btnW));
				transport.removeFromLeft(4);
				holdButton.setBounds(transport.removeFromLeft(btnW));
				transport.removeFromLeft(4);
				pitchButton.setBounds(transport.removeFromLeft(btnW));
			} else if (!inRetroSequencer) {
				statusLabel.setBounds(transport.reduced(10, 0));
			}
		}

		// D110Panel paints and hit-tests entirely in its own fixed reference-pixel space
		// (kCompactRefW x kRefH) and relies on the PARENT applying a Component::setTransform
		// scale to make that visually fit whatever window size there actually is - it does not
		// accept being resized to arbitrary pixel bounds itself (its paint() draws the panel
		// photo untransformed via drawImageAt(0,0), so a smaller setBounds just clips to a
		// corner instead of scaling down - this is what produced the giant single LCD segments
		// filling the whole screen before this fix). The desktop editor's own resized() gets
		// away with a bare scale(s) transform (no translation) because its panel always sits at
		// parent-space y=0 there - a pure scale is centred on the PARENT's origin, not the
		// component's own bounds position, so with any other y offset the scaled content lands
		// at y*s, not y, sliding it back up under whatever's above it (here, hidden entirely
		// behind the load/stop buttons added later - later-added children paint on top). Folding
		// the real pixel offset into the transform itself (scaled first, then translated) is
		// what actually places it where setBounds' own y argument suggests it should be.
		const float scale = float(area.getWidth()) / float(D110Panel::currentRefW(true));

		// The hamburger menu's "Sequencer" entry swaps this whole band between the front panel
		// and the sequencer (Alan's request, 2026-08-22) - only one at a time, since a phone
		// doesn't have room for both plus the keyboard the way the desktop editor stacks its
		// drawers. Grid (D110SequencerPanel) is the default, matching the desktop editor's own
		// default - retro is one Options tap away (processor.getSequencerRetroMode(), the exact
		// same flag). Both sequencer views self-scale from real pixel bounds exactly like
		// D110Keyboard, unlike D110Panel's fixed-reference-space+transform below.
		if (showingSoundbanks) {
			// Full remaining area, no keyboard underneath (unlike the sequencer views below) -
			// browsing/injecting a patch library doesn't need play-testing alongside it, and a
			// small phone screen has more use for the extra list height.
			panel.setVisible(false);
			gridSeq.setVisible(false);
			pianoRollSeq.setVisible(false);
			retroSeq.setVisible(false);
			keyboard.setVisible(false);
			// Bug fix (Alan's report, 2026-08-28): this was the one branch that never actually
			// called setVisible(true) - soundbankBrowser was added via addChildComponent()
			// (invisible by default, same as gridSeq/retroSeq), and every OTHER branch below
			// only ever turns it back off, never on. Bounds were being set correctly the whole
			// time, so the bug was invisible in code review - the view was just never shown,
			// which is exactly what "no Rescan button, empty screen" looks like from outside.
			soundbankBrowser.setVisible(true);
			soundbankBrowser.setBounds(area);
		} else if (showingSequencer) {
			panel.setVisible(false);
			soundbankBrowser.setVisible(false);
			const bool retro = processor.getSequencerRetroMode();
			const bool pianoRoll = !retro && processor.getSequencerGridMode();
			gridSeq.setVisible(!retro && !pianoRoll);
			pianoRollSeq.setVisible(pianoRoll);
			retroSeq.setVisible(retro);

			// Unlike Panel mode below, the sequencer here is NOT capped at its own reference
			// height (kRefH) - both D110SequencerPanel and D110SequencerRetroPanel lay
			// themselves out in PERCENTAGES of whatever real pixel bounds they're given
			// (self-scaling, exactly like D110Keyboard below, not fixed-reference-space like
			// D110Panel), so handing them extra height makes every row - the 9 track rows
			// especially - proportionally taller rather than just leaving it centred in unused
			// space. The keyboard also only gets ITS OWN small reference height here
			// (D110Keyboard::kRefH = 130) rather than borrowing D110Panel::kRefH's much taller
			// one (256, sized for sitting under the panel PHOTO) the way Panel mode's keyboard
			// does below - that mismatch was exactly the two complaints Alan raised 2026-08-22:
			// the keyboard sitting taller than it needed to, and the leftover gap under it that
			// created neither made it to the sequencer.
			//
			// `scale` collapses in portrait for the exact same reason it does in Panel mode
			// below (it's derived from width relative to the PANEL's reference width, which
			// portrait squeezes to a fraction of its natural size) - Alan's "le clavier est
			// toujours ridiculement petit" after the Panel-mode fix already went in, because
			// that fix never touched this second copy of the same formula. Landscape keeps the
			// scale-based height (already tuned and confirmed there); portrait gets the same
			// width-based cap Panel mode uses, just a bit tighter (width/5 not /4) to leave more
			// of the screen to the 9 track rows, which is the whole point of this view.
			//
			// Retro landscape specifically gets a further x1.6 on top of that (Alan's request,
			// 2026-08-24, "environ 60% plus haut") - Grid keeps the plain scale-based height,
			// this is retro only.
			const float retroLandscapeBoost = (retro && isLandscape) ? 1.6f : 1.0f;
			const int keyboardHeight =
				isLandscape
					? juce::jmin(area.getHeight(), juce::roundToInt(D110Keyboard::kRefH * scale * retroLandscapeBoost))
					: juce::jmin(area.getHeight(), juce::roundToInt(area.getWidth() / 5.0f));
			auto seqBounds = area;
			seqBounds.removeFromBottom(keyboardHeight);
			gridSeq.setBounds(seqBounds);
			pianoRollSeq.setBounds(seqBounds);
			retroSeq.setBounds(seqBounds);
			keyboard.setVisible(true);
			keyboard.setBounds(area.removeFromBottom(keyboardHeight));
		} else {
			gridSeq.setVisible(false);
			pianoRollSeq.setVisible(false);
			retroSeq.setVisible(false);
			soundbankBrowser.setVisible(false);
			keyboard.setVisible(true);
			panel.setVisible(true);
			const int panelY = area.getY();
			panel.setBounds(0, 0, D110Panel::currentRefW(true), D110Panel::kRefH);
			panel.setTransform(juce::AffineTransform::scale(scale).translated(0.0f, float(panelY)));
			panel.setDisplayScale(scale);
			area.removeFromTop(juce::roundToInt(D110Panel::kRefH * scale));

			// D110Keyboard, unlike D110Panel, scales itself internally from whatever real pixel
			// bounds it's given - same real-pixel setBounds() call the desktop editor uses. The
			// old cap here (D110Panel::kRefH*scale) tied keyboard height to the PANEL's own
			// reference height through `scale`, which is derived from width relative to the
			// panel's reference width (1497) - fine in landscape, where that width fills most of
			// the screen, but in portrait the panel is squeezed to a fraction of its natural
			// width, dragging `scale` (and therefore the keyboard height riding on it) down to
			// almost nothing: Alan's "trop petit" in portrait. Filling ALL remaining space
			// unconditionally (tried first) swung the other way - on a tall portrait screen the
			// keyboard has since sizes was the entire dead space, keys several times taller than
			// wide. This splits the difference: capped at a quarter of the keyboard's own WIDTH,
			// independent of the panel's `scale` entirely, so it can't inherit portrait's
			// squeeze - generous enough to fix "too small", nowhere near unbounded's stretch,
			// and if there's still leftover height below that (Alan's other complaint, the gap),
			// it's now bounded by width rather than open-ended, so any remaining gap should be
			// modest rather than "most of the screen" the way portrait's old cap made it.
			const int keyboardHeight = juce::jmin(area.getHeight(),
			                                       juce::roundToInt(area.getWidth() / 4.0f));
			keyboard.setBounds(area.removeFromTop(keyboardHeight));
		}
	}

private:
	// Everything that isn't Play/Stop lives behind this one button (Alan's request,
	// 2026-08-22, replacing a dedicated "Load MIDI file..." button and a dedicated "Options"
	// button - one row of controls, not two, since landscape leaves the panel/keyboard little
	// enough vertical room already). "Sequencer" and "Keyboard channel..." added the same day,
	// once a USB MIDI keyboard became a real input source and not just the on-screen one -
	// MIDI Remap/channel selection actually matters now, and so does having a sequencer view
	// to record into.
	// Shared between the app's own hamburger button and (2026-08-22) the grid sequencer's own
	// bar-navigation menu button - see D110SequencerPanel::onBarMenuButtonExtra's own comment
	// for why the sequencer needs this too (its own transport replaces the app's hamburger
	// entirely while showing, so this is the only way back to Panel view or any of the rest).
	// Every item is a self-contained action callback rather than a numeric result ID precisely
	// so it can be dropped into either menu with no ID-space coordination between them.
	void buildAppMenu(juce::PopupMenu &m) {
		// Reachable up top and unconditionally (not gated behind processor.isSynthReady()) -
		// this is exactly the thing to reach for when the ROMs are missing and everything else
		// in the app is sitting there showing the "ROMs not found" error instead of the panel.
		m.addItem("Choose ROM files...", [this] { chooseRomFolder(); });
		m.addItem("Load MIDI file...", [this] { chooseMidiFile(); });
		m.addItem(playing ? "Pause MIDI file" : "Play MIDI file", [this] { togglePlayPause(); });
		m.addItem("Stop MIDI file", [this] { stopPlayback(); });
		m.addItem("Import SysEx/MIDI Bank...", [this] { chooseSysexFile(); });
		m.addSeparator();
		// The same three views the mode-switch button cycles through, picked directly.
		for (auto mode : { d110ui::AppMode::frontPanel, d110ui::AppMode::sequencer, d110ui::AppMode::soundbanks })
			m.addItem(d110ui::appModeName(mode), true, currentMode() == mode, [this, mode] { setMode(mode); });
		m.addSeparator();
		m.addItem("Choose soundbank files...", [this] { chooseSoundbankFiles(); });
		// D110Keyboard::showContextMenu() is the exact same channel/remap/PC-keyboard menu the
		// desktop keyboard's right-click shows - reached here directly instead of reimplementing
		// it, since there's no right mouse button (or safe long-press substitute - see
		// D110Keyboard.h) on a touchscreen.
		m.addItem("Keyboard channel...", [this] { keyboard.showContextMenu(); });
		m.addSeparator();

		// Drive an external hardware synth off whatever the D-110 itself is playing (Alan's
		// request, 2026-08-22) - setMidiOutputDevice() is the exact mechanism the desktop
		// Standalone build already uses for its own directly-opened MIDI Out port; every note
		// this app injects (on-screen keyboard, USB MIDI keyboard input, file/sequencer
		// playback) reaches handleIncomingMidiMessage() the same way regardless of source, and
		// that now also echoes to osMidiOut when one is set (PluginProcessor.cpp), so nothing
		// else has to change here beyond picking a device. Queried fresh every time this menu
		// opens rather than cached, so a USB device plugged in since the last time shows up
		// without needing its own periodic scan the way MIDI INPUT devices do (those need to be
		// enabled before they can deliver anything at all; output devices just need opening at
		// the moment of sending, which setMidiOutputDevice already does).
		{
			juce::PopupMenu midiOut;
			midiOut.addItem("None", true, processor.getMidiOutputId().isEmpty(),
			                 [this] { processor.setMidiOutputDevice({}); });
			for (const auto &device : juce::MidiOutput::getAvailableDevices())
				midiOut.addItem(device.name, true, processor.getMidiOutputId() == device.identifier,
				                 [this, id = device.identifier] { processor.setMidiOutputDevice(id); });
			m.addSubMenu("MIDI Output (external synth)", midiOut);
		}
		m.addSeparator();

		juce::PopupMenu options;
		options.addItem("1 Octave", true, keyboard.getNumOctaves() == 1, [this] { keyboard.setNumOctaves(1); });
		options.addItem("2 Octaves", true, keyboard.getNumOctaves() == 2, [this] { keyboard.setNumOctaves(2); });
		options.addSeparator();
		// "Sequencer > Classic / Retro / Grid" - the same submenu every front end of the family
		// offers on right-click (sequencer/SequencerViewMenu.h).
		seqview::addSubmenu(options, seqview::current(processor), [this](seqview::View v) {
			seqview::apply(processor, v);
			if (showingSequencer) resized();
		});
		options.addSeparator();
		// Alan's request, 2026-08-28. System resolves against the OS dark-mode setting right
		// away and keeps following it live (see UiTheme.h's setThemeMode() comment) - no restart
		// needed either way. The checkmark reflects the stored MODE (light/dark/system), not the
		// resolved theme, so "System" stays checked even while it happens to currently resolve
		// to whichever of Dark/Light the OS is in.
		{
			const bool followSystem = processor.getUiThemeFollowSystem();
			juce::PopupMenu theme;
			theme.addItem("Dark", true, !followSystem && !processor.getUiThemeLight(),
			              [this] { setThemeMode(d110ui::ThemeMode::Dark); });
			theme.addItem("Light", true, !followSystem && processor.getUiThemeLight(),
			              [this] { setThemeMode(d110ui::ThemeMode::Light); });
			theme.addItem("System (follow OS)", true, followSystem,
			              [this] { setThemeMode(d110ui::ThemeMode::System); });
			options.addSubMenu("Theme", theme);
		}
		options.addSeparator();
		// Manual escape hatch for resized()'s own nav-bar-avoidance guess (see its comment) -
		// Alan's request, 2026-08-22, after a tablet where the nav bar apparently stays
		// bottom-anchored even in landscape (rather than moving to a side the way phones do)
		// made the automatic side-guessing unreliable there. Four independent checkboxes, not
		// a single "which side" choice - a real device can need two margins at once (e.g. a
		// status bar on top AND a bottom-anchored nav bar). Flat items here rather than a
		// further-nested submenu on purpose: a 3-level-deep popup (Options -> this submenu ->
		// its items) was the one Alan reported rendering partly off-screen and needing a drag
		// to keep open - one fewer nesting level side-steps that.
		options.addSeparator();
		options.addItem("Auto margins (Recommended)", true, !navTop && !navBottom && !navLeft && !navRight,
		                 [this] { navTop = navBottom = navLeft = navRight = false; resized(); });
		options.addItem("Margin: top", true, navTop, [this] { navTop = !navTop; resized(); });
		options.addItem("Margin: bottom", true, navBottom, [this] { navBottom = !navBottom; resized(); });
		options.addItem("Margin: left", true, navLeft, [this] { navLeft = !navLeft; resized(); });
		options.addItem("Margin: right", true, navRight, [this] { navRight = !navRight; resized(); });
		m.addSubMenu("Options", options);
	}

	void showMainMenu() {
		juce::PopupMenu m;
		buildAppMenu(m);
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menuButton));
	}

	// Resolves the persisted uiThemeLight/uiThemeFollowSystem pair into an actual d110ui call -
	// shared by the constructor (applying whatever was saved last session) and setThemeMode()
	// below (applying a fresh menu pick and persisting it in the same step).
	void applyThemeMode() {
		d110ui::setThemeMode(processor.getUiThemeFollowSystem()  ? d110ui::ThemeMode::System
		                      : processor.getUiThemeLight()      ? d110ui::ThemeMode::Light
		                                                          : d110ui::ThemeMode::Dark);
	}

	void setThemeMode(d110ui::ThemeMode mode) {
		processor.setUiThemeFollowSystem(mode == d110ui::ThemeMode::System);
		if (mode != d110ui::ThemeMode::System) processor.setUiThemeLight(mode == d110ui::ThemeMode::Light);
		applyThemeMode();
	}

	// The three top-level views are mutually exclusive: front panel (neither flag), sequencer
	// (whose own Classic/Retro/Piano-roll flavour is the separate "Sequencer" submenu in
	// Options) and soundbanks.
	d110ui::AppMode currentMode() const {
		return showingSoundbanks ? d110ui::AppMode::soundbanks
		       : showingSequencer ? d110ui::AppMode::sequencer
		                          : d110ui::AppMode::frontPanel;
	}

	void setMode(d110ui::AppMode mode) {
		if (mode == currentMode()) return;
		const bool wasSoundbanks = showingSoundbanks;
		showingSequencer = (mode == d110ui::AppMode::sequencer);
		showingSoundbanks = (mode == d110ui::AppMode::soundbanks);
		if (showingSoundbanks) {
			soundbankBrowser.refresh(); // pick up anything scanned since this was last shown
		} else if (wasSoundbanks) {
			// Leaving the screen with HOLD still on would otherwise leave a note sustaining
			// with no visible way back to stop it short of MIDI Panic - stop it here instead.
			stopHeldTestNote();
		}
		resized();
		repaint();
	}

	// Front panel -> sequencer -> soundbanks -> front panel.
	void cycleMode() { setMode(d110ui::nextAppMode(currentMode())); }

	// Soundbanks view's test-note (left, plays testNotePitch on the currently selected Part -
	// see SoundbankBrowser::getSelectedPart()), HOLD (sustains it and follows whichever tone is
	// auditioned next) and PITCH (right, Alan's correction 2026-09-03: the trigger button itself
	// no longer has any long-press gesture at all - it's just "play continuously while held",
	// nothing more - and setting testNotePitch is this separate, dedicated button's whole job)
	// buttons - Alan's original request, 2026-08-30: that view has no on-screen keyboard (see
	// resized()'s own comment on showingSoundbanks), so there was otherwise no way to actually
	// hear a browsed tone without leaving the screen.
	void wireUpSoundbankTestNote() {
		testNoteButton.setButtonText(juce::MidiMessage::getMidiNoteName(testNotePitch, true, true, 4));
		testNoteButton.onPress = [this] {
			const int part = soundbankBrowser.getSelectedPart();
			const int channel = processor.liveChannelForPart(part);
			if (channel < 0) return; // that Part is off - nowhere to send a note
			testButtonChannel = channel;
			testButtonNote = testNotePitch;
			processor.injectTestNote(channel, testNotePitch, 0.9f, true);
		};
		testNoteButton.onRelease = [this] {
			if (testButtonChannel < 0) return;
			processor.injectTestNote(testButtonChannel, testButtonNote, 0.0f, false);
			testButtonChannel = -1;
		};
		addChildComponent(testNoteButton);

		holdButton.setButtonText("HOLD");
		holdButton.setClickingTogglesState(true);
		holdButton.onClick = [this] {
			if (holdButton.getToggleState()) startHeldTestNote();
			else stopHeldTestNote();
			updateHoldButtonColour();
		};
		addChildComponent(holdButton);
		updateHoldButtonColour();

		pitchButton.setButtonText("PITCH");
		pitchButton.onClick = [this] {
			auto *aw = new juce::AlertWindow("Test note pitch", {}, juce::AlertWindow::NoIcon);
			// A real slider, Alan's own follow-up request, 2026-08-30 (replacing this dialog's
			// first cut, a typed note name) - full MIDI range, its own text box shows the note
			// name rather than a raw number. addCustomComponent() takes no ownership (same as
			// D110SequencerPanel::promptForTempo()'s own TAP button) - deleted explicitly below.
			auto *slider = new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::TextBoxBelow);
			slider->setSize(280, 60);
			slider->setRange(0.0, 127.0, 1.0);
			slider->setValue(testNotePitch, juce::dontSendNotification);
			slider->textFromValueFunction = [](double v) {
				return juce::MidiMessage::getMidiNoteName(int(v), true, true, 4);
			};
			slider->updateText();
			// No live audition while dragging (Alan, 2026-09-03: sounded artefacted) - the
			// slider just picks the value, applied silently on "Set" below.
			aw->addCustomComponent(slider);
			aw->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
			aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
			aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, slider](int result) {
				if (result == 1) {
					testNotePitch = int(slider->getValue());
					testNoteButton.setButtonText(juce::MidiMessage::getMidiNoteName(testNotePitch, true, true, 4));
				}
				delete slider;
				delete aw;
			}));
		};
		addChildComponent(pitchButton);

		// Alan's request, 2026-08-30: while HOLD is on, browsing to a different sound (double-
		// tap, or any other way SoundbankBrowser auditions a tone) kills the currently-held note
		// and immediately strikes the newly-picked one on whichever Part it just landed on, so a
		// continuously-held note actually previews each new sound's attack rather than quietly
		// swapping underneath one already mid-decay.
		soundbankBrowser.onToneAuditioned = [this](int part) {
			if (!holdActive) return;
			if (heldChannel >= 0) processor.injectTestNote(heldChannel, heldNote, 0.0f, false);
			const int channel = processor.liveChannelForPart(part);
			if (channel < 0) {
				heldChannel = -1;
				return;
			}
			heldChannel = channel;
			heldNote = testNotePitch;
			processor.injectTestNote(heldChannel, heldNote, 0.9f, true);
		};
	}

	void startHeldTestNote() {
		const int part = soundbankBrowser.getSelectedPart();
		const int channel = processor.liveChannelForPart(part);
		if (channel < 0) {
			holdButton.setToggleState(false, juce::dontSendNotification); // nowhere to send it
			return;
		}
		holdActive = true;
		heldChannel = channel;
		heldNote = testNotePitch;
		processor.injectTestNote(heldChannel, heldNote, 0.9f, true);
	}

	void stopHeldTestNote() {
		if (heldChannel >= 0) processor.injectTestNote(heldChannel, heldNote, 0.0f, false);
		holdActive = false;
		heldChannel = -1;
		holdButton.setToggleState(false, juce::dontSendNotification);
	}

	// setClickingTogglesState()'s own colouring relies on the shared LookAndFeel this app DOES
	// install (unlike the desktop plugin/Standalone - see SoundbankBrowser::toggleHideDuplicates()'s
	// own comment for that gap) - explicit colours here anyway rather than trusting it, same
	// robust approach, so this doesn't depend on LookAndFeel install/construction order.
	void updateHoldButtonColour() {
		const auto &pal = d110ui::palette();
		const auto fill = holdActive ? pal.seqActiveFill : pal.box;
		const auto text = holdActive ? pal.seqActiveText : pal.value;
		holdButton.setColour(juce::TextButton::buttonColourId, fill);
		holdButton.setColour(juce::TextButton::buttonOnColourId, fill);
		holdButton.setColour(juce::TextButton::textColourOffId, text);
		holdButton.setColour(juce::TextButton::textColourOnId, text);
	}


	void chooseMidiFile() {
		// Async dialog, so the chooser has to outlive this call - same trick as every
		// other FileChooser in the desktop editor.
		fileChooser = std::make_unique<juce::FileChooser>("Choose a MIDI file to play",
		                                                  juce::File(), "*.mid;*.midi");
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
			[this](const juce::FileChooser &fc) {
				// getResult() (a juce::File) is what the desktop pickers hand back, but on
				// Android the system picker returns a content:// SAF URI with no real path on
				// disk. File::createInputStream() on it silently fails, AND (first attempt at
				// this fix) so does plain URL::createInputStream() - that path is for HTTP(S)
				// URLs; it only checks isLocalFile() (scheme=="file") before falling through to
				// a WebInputStream, which naturally can't open a "content://" address either.
				// juce::AndroidDocument is JUCE's actual answer to a SAF result (its own class
				// comment names this exact use case: "pass the FileChooser's URL result to
				// AndroidDocument::fromDocument... createInputStream to read from the file").
				auto url = fc.getURLResult();
				if (url != juce::URL()) loadAndPlayMidi(url);
			});
	}

	// Same "Import SysEx/MIDI Bank..." feature the desktop panel's right-click Options menu has
	// (PluginEditor.cpp, processor.importSysexBank()) - loads a real Roland librarian dump (.syx,
	// or a .mid with SysEx events embedded) and plays it down the emulated MIDI IN cable, which is
	// how a factory-empty internal Tone Memory (Bank I) actually gets populated. importSysexBank()
	// takes a real juce::File and reads it synchronously (loadFileAsData()/JUCE's own MidiFile
	// reader), neither of which work on a bare content:// SAF URI - same problem chooseMidiFile()
	// solves via AndroidDocument, but that only gets a stream, not a juce::File importSysexBank()
	// can hand to the firmware. So: copy the picked document into a real path first (the same copy
	// trick copyRomFilesAndReload() uses for ROM files), then import from that path - deleting the
	// copy is safe immediately afterwards since importSysexBank() has already fully parsed it by
	// the time it returns (no async re-read of the file later).
	void chooseSysexFile() {
		// No filter pattern, deliberately - same reasoning as chooseRomFolder() below. JUCE's
		// Android FileChooser (juce_FileChooser_android.cpp, convertFiltersToMimeTypes())
		// resolves each "*.ext" token through a small hardcoded extension->MIME table
		// (juce_common_MimeTypes.cpp); ".syx"/".smf" aren't in it, so they contribute nothing,
		// and Android's picker then greys out (silently un-tappable - no callback ever fires,
		// exactly what Alan hit) any file whose own sniffed MIME doesn't match the resulting
		// EXTRA_MIME_TYPES array. Unlike the desktop FileBrowserComponent's own glob matcher
		// (PluginEditor.cpp's "*.*" trick), a trailing "*.*" token does NOT mean "match
		// everything" here - "*" isn't a table entry either, so it's a no-op. Passing no filter
		// at all skips EXTRA_MIME_TYPES entirely (see the "if (mimeTypes.size() > 0)" guard in
		// that same constructor) - nothing is filtered, nothing greyed out.
		fileChooser = std::make_unique<juce::FileChooser>("Choose a SysEx or MIDI bank file", juce::File());
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
			[this](const juce::FileChooser &fc) {
				auto url = fc.getURLResult();
				if (url != juce::URL()) copySysexFileAndImport(url);
			});
	}

	void copySysexFileAndImport(const juce::URL &url) {
		auto doc = juce::AndroidDocument::fromDocument(url);
		auto in = doc.hasValue() ? doc.createInputStream() : nullptr;
		if (in == nullptr) {
			statusLabel.setText("Could not open: " + url.getFileName(), juce::dontSendNotification);
			return;
		}

		auto tempFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
			.getChildFile("sysex-import-" + url.getFileName());
		tempFile.deleteFile();
		auto out = tempFile.createOutputStream();
		if (out == nullptr || out->writeFromInputStream(*in, -1) <= 0) {
			out.reset();
			tempFile.deleteFile();
			statusLabel.setText("Could not read: " + url.getFileName(), juce::dontSendNotification);
			return;
		}
		out.reset();

		processor.importSysexBank(tempFile);
		tempFile.deleteFile();
		statusLabel.setText(processor.getLastImportMessage(), juce::dontSendNotification);
	}

	// Android's equivalent of the desktop Utility tab's "ROM FOLDER" picker (PluginEditor.cpp,
	// id 16) - but it can't work the same way desktop does (pointing setCustomRomFolder()
	// straight at whatever folder the user picks), and not only for the reason chooseMidiFile()'s
	// own comment already gives (content:// URIs aren't real filesystem paths, and
	// D110CoreNative::start() needs one). A canSelectDirectories FileChooser (tried first) hands
	// back a SAF *tree* URI, and this build's JUCE has no working way to list a tree's contents:
	// the public juce::AndroidDocumentIterator - despite its class comment - turns out to be
	// backed by plain juce::File/DirectoryIterator under the hood (see
	// AndroidDocumentIterator::makeRecursive()/makeNonRecursive() in
	// juce_AndroidDocument_android.cpp, both call dir.getUrl().getLocalFile() and iterate that),
	// which silently visits zero children for a real content:// tree (confirmed empirically on a
	// real device - a picked folder with 7 real ROM files inside came back "seen=0"). The
	// DocumentsContract-based recursive engine that WOULD walk a tree correctly
	// (AndroidDocumentDetail::RecursiveEngine) exists in that same file but is never actually
	// wired up to the public API - dead code in this JUCE version.
	//
	// So: ask for the ROM FILES themselves (multi-select), not a folder. That sidesteps tree
	// listing entirely and reuses AndroidDocument::fromDocument()+createInputStream() - the exact
	// per-file mechanism loadAndPlayMidi() already relies on successfully. Each selected file
	// gets copied into romBringUpDir() (a real path the native core can open), then
	// setCustomRomFolder() points there - same as before, and still the same folder the
	// constructor auto-detects on every future launch with no extra step.
	void chooseRomFolder() {
		fileChooser = std::make_unique<juce::FileChooser>(
			"Choose your D-110 ROM files (select all of them at once)", juce::File());
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
				| juce::FileBrowserComponent::canSelectMultipleItems,
			[this](const juce::FileChooser &fc) { copyRomFilesAndReload(fc.getURLResults()); });
	}

	void copyRomFilesAndReload(const juce::Array<juce::URL> &urls) {
		// Same whole-image/chip-dump size set PluginProcessor.cpp's materializeLooseRomsIfNeeded
		// filters loose ROM candidates by - lets the user select more than strictly needed (e.g.
		// their whole extracted romset) without copying in anything unrecognised. File CONTENT
		// (which chip a same-sized dump actually is) and Control-image splitting are sorted out
		// afterwards by tryAutoLoadRoms() itself, same as every other ROM location.
		auto isKnownRomSize = [](juce::int64 n) {
			return n == 4096 || n == 32768 || n == 131072 || n == 163840 || n == 524288 || n == 1048576;
		};

		const auto dest = romBringUpDir();
		dest.createDirectory();

		int copied = 0;
		for (const auto &url : urls) {
			auto doc = juce::AndroidDocument::fromDocument(url);
			auto in = doc.hasValue() ? doc.createInputStream() : nullptr;
			if (in == nullptr) continue;

			auto outFile = dest.getChildFile(url.getFileName());
			outFile.deleteFile();
			auto out = outFile.createOutputStream();
			if (out == nullptr || out->writeFromInputStream(*in, -1) <= 0) {
				outFile.deleteFile();
				continue;
			}
			out.reset();
			if (isKnownRomSize(outFile.getSize())) ++copied;
			else outFile.deleteFile();
		}

		if (copied == 0) {
			statusLabel.setText("None of those looked like ROM files", juce::dontSendNotification);
			return;
		}

		D110AudioProcessor::setCustomRomFolder(dest.getFullPathName());
		processor.reloadRomsAndPowerOn();
		statusLabel.setText(processor.isSynthReady() ? "Ready" : processor.getLastError(),
		                     juce::dontSendNotification);
	}

	// Same reasoning as chooseRomFolder() above (see its own comment): Android has no working
	// way to list a picked SAF folder's contents, so this asks for the SysEx/MIDI files
	// themselves (multi-select) and copies them into a real, plain-filesystem staging folder -
	// d110bank::Database::rescan() (SoundbankDatabase.h) then walks THAT, not whatever the user
	// originally picked from. Repeatable: picking again adds more files into the same staging
	// folder rather than replacing it, so a library can be built up over several picks. A
	// single .zip is also accepted and is the practical stand-in for "pick a whole folder" -
	// rescan() looks inside it - since a real folder pick still isn't reliable here (Alan's
	// request, 2026-08-28).
	void chooseSoundbankFiles() {
		fileChooser = std::make_unique<juce::FileChooser>(
			"Choose SysEx patch library files, or a single .zip of them", juce::File());
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
				| juce::FileBrowserComponent::canSelectMultipleItems,
			[this](const juce::FileChooser &fc) { copySoundbankFiles(fc.getURLResults()); });
	}

	void copySoundbankFiles(const juce::Array<juce::URL> &urls) {
		const auto dest = D110AudioProcessor::getSoundbankImportsFolder();
		dest.createDirectory();

		int copied = 0;
		for (const auto &url : urls) {
			auto doc = juce::AndroidDocument::fromDocument(url);
			auto in = doc.hasValue() ? doc.createInputStream() : nullptr;
			if (in == nullptr) continue;

			// Bug fix (Alan's report, 2026-08-28: picked files copied fine but RESCAN then found
			// nothing). url.getFileName() is unreliable for a content:// SAF result - it's just
			// `toString(false).fromLastOccurrenceOf("/", ...)` (juce_URL.cpp), i.e. whatever
			// follows the URI STRING's own last literal '/', which for a SAF document URI is an
			// opaque document-ID fragment, not the real file name - so the copy landed under a
			// garbage name with no ".syx"/".mid" extension, and Database::rescan()'s own
			// extension filter silently skipped every one of them. AndroidDocumentInfo::getName()
			// is backed by SAF's real DISPLAY_NAME column instead - see
			// .claude/dev-notes/android.md's own catalogue of these content:// gotchas for the
			// same lesson learned twice already elsewhere in this file.
			juce::String name = doc.getInfo().getName();
			if (name.isEmpty()) name = url.getFileName();
			// Belt and braces: some content providers' own display name can still omit the
			// extension for a MIME type they don't recognise (.syx isn't a registered MIME type
			// anywhere) - force one on rather than silently losing the file to the same filter
			// a second way. ".zip" is a real, recognised choice here too (see this function's
			// own comment) - never stomped, only ever added when nothing recognisable is there.
			if (!juce::File(name).hasFileExtension("syx;mid;midi;smf;zip")) name += ".syx";

			auto outFile = dest.getChildFile(name);
			outFile.deleteFile();
			auto out = outFile.createOutputStream();
			if (out == nullptr || out->writeFromInputStream(*in, -1) <= 0) {
				outFile.deleteFile();
				continue;
			}
			out.reset();
			++copied;
		}

		if (copied == 0) {
			statusLabel.setText("Could not read any of those files", juce::dontSendNotification);
			return;
		}

		// No setSoundbankSourceFolder() call needed (removed 2026-08-28) - RESCAN now always
		// scans getSoundbankImportsFolder() too, on top of whatever getSoundbankSourceFolder()
		// is separately set to (see SoundbankBrowser::startRescan()'s own comment), so this copy
		// alone is enough for the next RESCAN to pick these files up.
		statusLabel.setText(juce::String(copied) + " file(s) added - open Soundbanks and hit "
		                     "RESCAN to add them to the database",
		                     juce::dontSendNotification);
	}

	// Roland's own factory default is Part N -> MIDI channel N+1 (docs/factory_defaults.md,
	// "Part 1 listens on channel 2... up to part 8 on channel 9") - not a straightforward
	// per-track "channel N" convention. But every part-authoring tool that just numbers its
	// tracks/channels 1-8 sequentially (this app's own test file among them - Alan confirmed
	// he never remapped anything, and the file itself carries no SysEx touching the SYSTEM
	// area's channel table either) implicitly assumes a D-110 already reconfigured that way.
	// Doing that reconfiguration ourselves before playback - and only for as long as playback
	// needs it - means a plain "channel N for part N" MIDI file just works, without asking
	// the user to go press SYSTEM buttons on the panel first.
	//
	// SysEx field layout (D110CoreType::kRamSystem+13.. per native_editor_tone_repro_probe.cpp,
	// matching docs/sysex_address_map.md's "0x2D98 == SysEx 0x100004, partial reserve + MIDI
	// channel map, 18 bytes" - 9 bytes partial reserve at fields 4-12, 9 bytes channel map at
	// fields 13-21 for Parts 1-8 + Rhythm, stored 0-based (value = channel-1)): field 13+p is
	// Part (p+1)'s channel. Rhythm (field 21) already defaults to channel 10 either way, so
	// it's left untouched.
	void setSequentialPartChannelMap(bool sequential) {
		for (int part = 0; part < 8; ++part)
			processor.sendSystemParam(13 + part, juce::uint8(sequential ? part : part + 1));
	}

	void loadAndPlayMidi(const juce::URL &url) {
		auto document = juce::AndroidDocument::fromDocument(url);
		auto stream = document.hasValue() ? document.createInputStream() : nullptr;
		if (stream == nullptr) {
			statusLabel.setText("Could not open: " + url.getFileName(), juce::dontSendNotification);
			return;
		}

		juce::MidiFile midiFile;
		if (!midiFile.readFrom(*stream)) {
			statusLabel.setText("Could not read: " + url.getFileName(), juce::dontSendNotification);
			return;
		}
		midiFile.convertTimestampTicksToSeconds();

		sequence.clear();
		for (int i = 0; i < midiFile.getNumTracks(); ++i)
			sequence.addSequence(*midiFile.getTrack(i), 0.0);
		sequence.updateMatchedPairs();

		loadedFileName = url.getFileName();
		eventIndex = 0;
		pausedElapsedSeconds = 0.0;
		playing = sequence.getNumEvents() > 0;
		if (playing) setSequentialPartChannelMap(true);
		playStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
		statusLabel.setText(playing ? "Playing: " + loadedFileName : "Empty MIDI file",
		                     juce::dontSendNotification);
	}

	// Play doubles as Pause: pressed while playing, it stops advancing but remembers position
	// (pausedElapsedSeconds), same as Stop used to - the difference is Stop now always rewinds
	// to the top instead, so Play is the only way to leave off mid-file and pick back up later.
	// Either transition needs a MIDI panic - the file player only ever sends note-ONs/offs as
	// the sequence dictates, so stopping mid-note (or mid-sustain-pedal) would otherwise leave
	// it ringing forever with nothing left to send the matching note-off.
	void togglePlayPause() {
		if (playing) {
			pausedElapsedSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001 - playStartSeconds;
			playing = false;
			processor.midiPanic();
			statusLabel.setText("Paused: " + loadedFileName, juce::dontSendNotification);
			return;
		}
		if (sequence.getNumEvents() == 0) return;
		if (eventIndex >= sequence.getNumEvents()) {
			eventIndex = 0;
			pausedElapsedSeconds = 0.0;
		}
		setSequentialPartChannelMap(true);
		playStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001 - pausedElapsedSeconds;
		playing = true;
		statusLabel.setText("Playing: " + loadedFileName, juce::dontSendNotification);
	}

	// Always rewinds to the top, unlike Play-as-pause - Stop means "start over", not "leave off
	// here". The panic is what actually guarantees silence: without it, any note or sustain
	// pedal active at the moment of stopping would otherwise never get its matching note-off.
	void stopPlayback() {
		playing = false;
		eventIndex = 0;
		pausedElapsedSeconds = 0.0;
		processor.midiPanic();
		setSequentialPartChannelMap(false); // back to factory default (Part N -> channel N+1)
		statusLabel.setText("Stopped", juce::dontSendNotification);
	}

	// Deliberately not the sequencer engine (D110SequencerEngine) - that's a full transport/
	// undo/quantize model built for interactive editing, out of scope here. Just a plain
	// cursor over a merged MidiMessageSequence, timed against a wall-clock start - the
	// simplest thing that plays a file's notes/program changes/CCs in order.
	// The D-110 has no live-MIDI Channel Volume/Pan concept at all - confirmed on a real DAW
	// session during the desktop plugin's own MIDI-reimport work (see
	// project_midi_reimport_volume_fix memory): CC7/CC10 sent over the wire are silently
	// ignored by the firmware. That fix's answer was a DIRECT SysEx write to the part's own
	// TimbreTemp Level/Pan fields (sendTimbreTempParam, fields 8/9) instead of a live CC -
	// the exact same call the Parts tab's own Level/Pan columns use. This simple player needs
	// the same substitution: without it, a file's CC7 (e.g. track 7's Volume=43, meant to tame
	// "Bombastic" down from full scale) is a no-op, and the part keeps playing at whatever
	// level its patch already had - which is what Alan heard as "un peu trop fort".
	//
	// Only meaningful for Parts 1-8 - channel-1 here is a part index (0-7) because playback
	// keeps the SYSTEM channel map remapped sequentially (setSequentialPartChannelMap) for as
	// long as it's running, same assumption the rest of this player already makes. Channels
	// outside 1-8 (rhythm's channel 10, or anything unused) fall through as an ordinary,
	// harmless live CC.
	void injectPlaybackEvent(const juce::MidiMessage &message) {
		const int part = message.getChannel() - 1;
		if (message.isController() && part >= 0 && part < 8) {
			if (message.getControllerNumber() == 7) {
				const int level = juce::jlimit(0, 100,
					juce::roundToInt(message.getControllerValue() * 100.0f / 127.0f));
				processor.sendTimbreTempParam(part, 8, juce::uint8(level));
				return;
			}
			if (message.getControllerNumber() == 10) {
				const int pan = juce::jlimit(0, 14,
					juce::roundToInt(message.getControllerValue() * 14.0f / 127.0f));
				processor.sendTimbreTempParam(part, 9, juce::uint8(pan));
				return;
			}
		}
		processor.injectMidiMessage(message);
	}

	// A USB MIDI keyboard plugged in after launch needs to actually be noticed - Android's
	// android.media.midi API (what JUCE's MidiInput::getAvailableDevices() wraps here) has no
	// "device changed" callback exposed through JUCE, only a snapshot list, so this polls it.
	// Gated to once every ~2 seconds (60 ticks at this 30Hz timer) rather than every tick:
	// enumerating devices isn't free, and a keyboard being noticed a second late doesn't matter.
	// Every discovered device is enabled and given a callback straight into the same
	// injectMidiMessage() path the on-screen keyboard and MIDI-file playback already use -
	// class-compliant USB MIDI needs no pairing/permission dialog the way Bluetooth would.
	void scanForMidiInputs() {
		for (const auto &device : juce::MidiInput::getAvailableDevices()) {
			if (knownMidiInputs.contains(device.identifier)) continue;
			knownMidiInputs.add(device.identifier);
			deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
			deviceManager.addMidiInputDeviceCallback(device.identifier, this);
		}
	}

	void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &message) override {
		// Called on the MIDI thread, not the message thread - injectMidiMessage() only ever
		// queues onto osMidiCollector (MidiMessageCollector, built for exactly this: safe to
		// feed from any thread, drained later on the audio thread), so no locking of our own
		// is needed here.
		processor.injectMidiMessage(message);
	}

	void timerCallback() override {
		if (++midiScanCountdown >= 60) {
			midiScanCountdown = 0;
			scanForMidiInputs();
		}

		if (!playing) return;
		const double elapsed = juce::Time::getMillisecondCounterHiRes() * 0.001 - playStartSeconds;
		while (eventIndex < sequence.getNumEvents()
		       && sequence.getEventPointer(eventIndex)->message.getTimeStamp() <= elapsed) {
			injectPlaybackEvent(sequence.getEventPointer(eventIndex)->message);
			++eventIndex;
		}
		if (eventIndex >= sequence.getNumEvents()) {
			playing = false;
			statusLabel.setText("Finished", juce::dontSendNotification);
		}
	}

	D110AudioProcessor processor;
	D110Panel panel;
	D110Keyboard keyboard;
	D110SequencerPanel gridSeq;
	D110SequencerGridPanel pianoRollSeq; // the third view - the piano roll (gridSeq is the normal strip)
	D110SequencerRetroPanel retroSeq;
	SoundbankBrowser soundbankBrowser;
	bool showingSequencer = false;
	bool showingSoundbanks = false;
	// Manual per-side margin overrides - see resized()'s own comment and the "Options" menu's
	// four checkboxes above. All false = Auto.
	bool navTop = false, navBottom = false, navLeft = false, navRight = false;
	d110ui::HamburgerButton menuButton;
	d110ui::ModeButton modeButton;
	juce::Label statusLabel;
	std::unique_ptr<juce::FileChooser> fileChooser;

	// Soundbanks view's test-note/HOLD buttons - see resized()'s own comment on
	// where they're laid out and wireUpSoundbankTestNote()'s own comment for the full behaviour.
	HeldNoteButton testNoteButton;
	juce::TextButton holdButton;
	// PITCH - a third button beside HOLD, Alan's correction 2026-09-03: setting testNotePitch
	// used to be a long-press gesture on the trigger button itself, but that's gone now (the
	// trigger is just plain press-and-hold-to-play) - this dedicated button opens the same
	// pitch-picker dialog on a plain click instead. See wireUpSoundbankTestNote()'s own comment.
	juce::TextButton pitchButton;
	int testNotePitch = 48; // C3 in this app's own convention (getMidiNoteName(n, true, true, 4))
	// Which (channel, note) the test-note button itself last struck, if any (-1 = none) - its
	// own press/release, independent of HOLD's own tracking below even though they usually
	// target the same Part.
	int testButtonChannel = -1, testButtonNote = -1;
	// Which (channel, note) HOLD is currently sustaining, if any - retriggered (killed, then
	// restruck on the newly-auditioned Part) by onToneAuditioned below while HOLD is on.
	bool holdActive = false;
	int heldChannel = -1, heldNote = -1;

	juce::AudioDeviceManager deviceManager;
	juce::AudioProcessorPlayer player;
	juce::StringArray knownMidiInputs;
	int midiScanCountdown = 0;

	juce::MidiMessageSequence sequence;
	juce::String loadedFileName;
	int eventIndex = 0;
	double playStartSeconds = 0;
	double pausedElapsedSeconds = 0;
	bool playing = false;
};

class MainWindow : public juce::DocumentWindow {
public:
	MainWindow(const juce::String &name)
		: DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::allButtons) {
		setUsingNativeTitleBar(true);
		auto *content = new MainComponent();
		mainComponent = content;
		setContentOwned(content, true);
		setFullScreen(true);
		setVisible(true);
	}

	void closeButtonPressed() override {
		juce::JUCEApplication::getInstance()->systemRequestedQuit();
	}

	// See MainComponent::saveState()'s own comment - D110AndroidApp::suspended() is the save
	// point that actually matters on Android.
	void saveState() { mainComponent->saveState(); }

private:
	MainComponent *mainComponent = nullptr;
};

class D110AndroidApp : public juce::JUCEApplication {
public:
	const juce::String getApplicationName() override { return "d110"; }
	const juce::String getApplicationVersion() override { return "0.1"; }

	void initialise(const juce::String &) override {
		mainWindow = std::make_unique<MainWindow>(getApplicationName());
	}

	void shutdown() override {
		if (mainWindow) mainWindow->saveState();
		mainWindow = nullptr;
	}

	// The actual save point that matters on Android (see MainComponent::saveState()'s own
	// comment): called when the OS backgrounds this app, and the process can be killed with
	// no further warning at any point afterwards, well before shutdown() above would ever
	// run - a plain in-app quit is the rare case, going to the home screen or switching apps
	// is the common one, and only this callback reliably fires for that.
	void suspended() override {
		if (mainWindow) mainWindow->saveState();
	}

private:
	std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(D110AndroidApp)
