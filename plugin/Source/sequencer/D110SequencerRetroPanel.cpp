#include "D110SequencerRetroPanel.h"
#include "SequencerTransportMenu.h"

#include <cmath>

#include "../DotMatrixFont.h"
#include "../UiTheme.h"

using d110seq::D110SequencerEngine;

namespace {

juce::String recordModeShortLabel(d110seq::RecordMode m) {
	switch (m) {
		case d110seq::RecordMode::overdub: return "OVERDUB";
		case d110seq::RecordMode::replaceRange: return "REPLACE";
		case d110seq::RecordMode::replaceToEnd: return "REPL+END";
	}
	return {};
}

juce::String loopModeShortLabel(d110seq::LoopMode m) {
	switch (m) {
		case d110seq::LoopMode::off: return "OFF";
		case d110seq::LoopMode::bar: return "BAR";
		case d110seq::LoopMode::punch: return "PUNCH";
	}
	return {};
}

// Shared by buildStepDurationMenu() (a full pick-list) and the STEP RECORDING overlay's
// own DUR row (UP/DOWN steps through the same presets in place, no submenu needed).
const std::array<d110seq::QuantizeGrid, 8> &stepDurationPresets() {
	using d110seq::QuantizeGrid;
	static const std::array<QuantizeGrid, 8> presets { { QuantizeGrid::whole, QuantizeGrid::half, QuantizeGrid::quarter,
	                                                       QuantizeGrid::eighth, QuantizeGrid::sixteenth,
	                                                       QuantizeGrid::eighthTriplet, QuantizeGrid::sixteenthTriplet,
	                                                       QuantizeGrid::thirtySecond } };
	return presets;
}

// Steps `current` forward/back by one entry in stepDurationPresets(), wrapping at either
// end - the STEP RECORDING overlay's DUR row UP/DOWN.
d110seq::QuantizeGrid cycleStepDuration(d110seq::QuantizeGrid current, int delta) {
	const auto &presets = stepDurationPresets();
	int idx = 0;
	for (int i = 0; i < (int) presets.size(); ++i)
		if (presets[(size_t) i] == current) { idx = i; break; }
	idx = (idx + delta + (int) presets.size()) % (int) presets.size();
	return presets[(size_t) idx];
}

juce::String stepDurationShortLabel(d110seq::QuantizeGrid g) {
	using d110seq::QuantizeGrid;
	switch (g) {
		case QuantizeGrid::whole: return "1/1";
		case QuantizeGrid::half: return "1/2";
		case QuantizeGrid::quarter: return "1/4";
		case QuantizeGrid::eighth: return "1/8";
		case QuantizeGrid::sixteenth: return "1/16";
		case QuantizeGrid::eighthTriplet: return "1/8T";
		case QuantizeGrid::sixteenthTriplet: return "1/16T";
		case QuantizeGrid::thirtySecond: return "1/32";
		case QuantizeGrid::off:
		default: return "1/4";
	}
}

// Character set the NameEdit screen's UP/DOWN cycles through at the caret position -
// space first (so trimming back to the default label is one press away), then A-Z, 0-9.
const juce::String kNameEditCharset(" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

// The real D-110's own LCD colours (see PluginEditor.cpp's kGlassOn/kInk, D110Panel's
// genuine hardware glass) - Alan's request, 2026-08-23: this LCD should look like the real
// instrument's, not follow the app's dark/light UI theme the way pal.value/pal.box do
// everywhere else in this file. A real two-colour alphanumeric LCD has no room for a
// "selected row" tint either, so selection here is shown only by the pre-existing ">"
// cursor marker, same as it would be on the genuine hardware's own menus.
const juce::Colour kLcdGlass(0xff6ab81f);
const juce::Colour kLcdInk(0xff05230a);

// ---------------------------------------------------------------------------
// Dot-matrix LCD font
// ---------------------------------------------------------------------------
// Extracted to ../DotMatrixFont.h/.cpp (2026-09-01) so the D-50 editor's own
// cosmetic LCD readout can share the same glyph table instead of a second
// copy of it - see that header's own comment for why this is a lookalike,
// not a real firmware-rendered display like the D-110's genuine one.
using namespace dotmatrix;

void paintRetroButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active) {
	const auto &pal = d110ui::palette();
	g.setColour(active ? pal.seqActiveFill : pal.seqInactiveFill);
	g.fillRoundedRectangle(b.reduced(2.0f), 3.0f);
	g.setColour(active ? pal.seqActiveText : pal.seqInactiveText);
	g.setFont(juce::FontOptions(juce::jlimit(8.0f, 13.0f, b.getHeight() * 0.42f)));
	g.drawText(label, b, juce::Justification::centred);
}

} // namespace

D110SequencerRetroPanel::D110SequencerRetroPanel(D110SequencerHost &p) : processor(p) {
	const juce::String saved = processor.getRetroKeyBindings();
	keyBindings = saved.isNotEmpty() ? decodeKeyBindings(saved) : defaultRetroKeyBindings();
	lcdCompactMode = processor.getRetroLcdCompactMode();
	homeScreen = buildHomeMenu();
	startTimerHz(15);
	setWantsKeyboardFocus(true);
}

D110SequencerRetroPanel::~D110SequencerRetroPanel() { stopTimer(); }

d110seq::D110SequencerEngine &D110SequencerRetroPanel::engine() { return processor.getSequencer(); }

juce::String D110SequencerRetroPanel::defaultTrackLabel(int t) {
	if (t == D110SequencerEngine::kRhythmTrack) return "RHYTHM";
	if (t < D110SequencerEngine::kNumTracks) return "PART " + juce::String(t + 1);
	return "TRACK " + juce::String(t + 1);
}

void D110SequencerRetroPanel::timerCallback() {
	auto &eng = engine();
	if (eng.isPlaying() || eng.isStepRecording() || eng.isPrecounting()) repaint();
}

// ---------------------------------------------------------------------------
// Navigation stack
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::pushScreen(Screen s) {
	stack.push_back(std::move(s));
	repaint();
}

void D110SequencerRetroPanel::popScreen() {
	if (!stack.empty()) stack.pop_back();
	// HOME's own transportRowLabel() quick-bar row (PLAY/STOP/REC/[MIDI]/OPTIONS) - whatever
	// it was last dialled to (OPTIONS, REC...) resets back to PLAY once we're actually at
	// HOME, rather than staying wherever it was left - Alan's request, 2026-08-23: landing
	// on a fixed, predictable action every time beats remembering whatever the row happened
	// to be showing before.
	if (stack.empty()) {
		const int idx = homeTransportRowIndex();
		if (idx < (int) homeScreen.quickIndex.size()) homeScreen.quickIndex[(size_t) idx] = 0;
	}
	repaint();
}

int D110SequencerRetroPanel::homeTransportRowIndex() {
	auto items = homeScreen.buildItems();
	const juce::String label = processor.transportRowLabel();
	for (int i = 0; i < (int) items.size(); ++i)
		if (items[(size_t) i].label == label) return i;
	return 0;
}

// ---------------------------------------------------------------------------
// Hardware buttons
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::pressStop() {
	// Same as D110SequencerPanel's plain-click STOP: halts the transport AND sends a MIDI
	// panic, so a note-off scheduled past the stop point never gets left stuck sounding -
	// see D110SequencerPanel::mouseDown()'s own comment on why.
	//
	// A STOP that lands while the transport is ALREADY stopped (isRecording/isPlaying/
	// isStepRecording all false going in - engine().stop() only ever flips playing false and
	// snaps to the CURRENT bar's start, it doesn't touch that) rewinds all the way to
	// getStartBar() on top of that - Alan's request, 2026-08-24, "deux fois sur stop": no
	// extra press-timing state needed, "stop pressed while already stopped" already means
	// this is at least the second press in a row.
	auto &eng = engine();
	const bool alreadyStopped = !eng.isRecording() && !eng.isPlaying() && !eng.isStepRecording();
	eng.stop();
	processor.midiPanic();
	if (alreadyStopped) eng.gotoBar(eng.getStartBar());
	repaint();
}

void D110SequencerRetroPanel::pressPlay() {
	engine().play();
	repaint();
}

void D110SequencerRetroPanel::pressRec() {
	auto &eng = engine();
	if (eng.isRecording()) eng.stopRecording();
	else if (eng.getArmedTrack() >= 0) eng.startRecording();
	repaint();
}

// The STEP RECORDING overlay (only ever shown on HOME, see paintLcd()) has its own 2-row
// BAR/DUR cursor (stepOverlayCursor) instead of a real Screen - UP/DOWN moves the cursor
// between the two rows (they're stacked vertically), LEFT/RIGHT (see pressLeft()/
// pressRight() below) acts on whichever row is focused (BAR: advance/rewind the step
// itself; DUR: cycle the duration preset). Alan's request, 2026-08-23: arrow keys used to
// always mean REST/undo-step with no way to reach DUR at all without leaving the overlay;
// initially wired UP/DOWN=act, LEFT/RIGHT=navigate, then flipped the same day once Alan
// pointed out UP/DOWN should move between the vertically-stacked rows, not act on them.
void D110SequencerRetroPanel::pressUp() {
	if (stack.empty() && engine().isStepRecording()) { stepOverlayCursor = 0; repaint(); return; }
	auto &s = top();
	if (s.kind == ScreenKind::list) {
		auto items = s.buildItems();
		if (!items.empty()) s.cursor = (s.cursor - 1 + (int) items.size()) % (int) items.size();
	} else if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) {
			if (s.verticalFields) {
				s.cursor = (s.cursor - 1 + (int) s.fields.size()) % (int) s.fields.size();
			} else {
				auto &f = s.fields[(size_t) juce::jlimit(0, (int) s.fields.size() - 1, s.cursor)];
				*f.value = juce::jlimit(f.minValue, f.maxValue, *f.value + f.upDownStep);
			}
		}
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditAdjust(+1);
	}
	repaint();
}

void D110SequencerRetroPanel::pressDown() {
	if (stack.empty() && engine().isStepRecording()) { stepOverlayCursor = 1; repaint(); return; }
	auto &s = top();
	if (s.kind == ScreenKind::list) {
		auto items = s.buildItems();
		if (!items.empty()) s.cursor = (s.cursor + 1) % (int) items.size();
	} else if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) {
			if (s.verticalFields) {
				s.cursor = (s.cursor + 1) % (int) s.fields.size();
			} else {
				auto &f = s.fields[(size_t) juce::jlimit(0, (int) s.fields.size() - 1, s.cursor)];
				*f.value = juce::jlimit(f.minValue, f.maxValue, *f.value - f.upDownStep);
			}
		}
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditAdjust(-1);
	}
	repaint();
}

// List rows: LEFT/RIGHT drives whichever of onAdjust/quickActions the row under the
// cursor defines (see ListItem's own comment) - a plain row (neither set, the vast
// majority of nested menus) does nothing here, same as before this mechanism existed.
void D110SequencerRetroPanel::pressLeft() {
	if (stack.empty() && engine().isStepRecording()) {
		auto &eng = engine();
		if (stepOverlayCursor == 0) eng.stepBack();
		else eng.setStepDuration(cycleStepDuration(eng.getStepDuration(), -1));
		repaint();
		return;
	}
	auto &s = top();
	if (s.kind == ScreenKind::list) {
		auto items = s.buildItems();
		if (!items.empty()) {
			const int idx = juce::jlimit(0, (int) items.size() - 1, s.cursor);
			auto &it = items[(size_t) idx];
			if (it.onAdjust) {
				it.onAdjust(-1);
			} else if (!it.quickActions.empty()) {
				if ((int) s.quickIndex.size() < (int) items.size()) s.quickIndex.resize(items.size(), 0);
				int &qi = s.quickIndex[(size_t) idx];
				qi = (qi - 1 + (int) it.quickActions.size()) % (int) it.quickActions.size();
			}
		}
	} else if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) {
			const int idx = juce::jlimit(0, (int) s.fields.size() - 1, s.cursor);
			auto &f = s.fields[(size_t) idx];
			if (s.verticalFields) *f.value = juce::jlimit(f.minValue, f.maxValue, *f.value - (f.leftRightStep != 0 ? f.leftRightStep : f.upDownStep));
			else if (f.leftRightStep != 0) *f.value = juce::jlimit(f.minValue, f.maxValue, *f.value - f.leftRightStep);
			else if (s.fields.size() == 1) *f.value = juce::jlimit(f.minValue, f.maxValue, *f.value - f.upDownStep);
			else s.cursor = (s.cursor - 1 + (int) s.fields.size()) % (int) s.fields.size();
		}
	} else if (s.kind == ScreenKind::confirm) {
		s.confirmYes = false;
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditMoveCaret(-1);
	}
	repaint();
}

void D110SequencerRetroPanel::pressRight() {
	if (stack.empty() && engine().isStepRecording()) {
		auto &eng = engine();
		if (stepOverlayCursor == 0) eng.stepRest();
		else eng.setStepDuration(cycleStepDuration(eng.getStepDuration(), +1));
		repaint();
		return;
	}
	auto &s = top();
	if (s.kind == ScreenKind::list) {
		auto items = s.buildItems();
		if (!items.empty()) {
			const int idx = juce::jlimit(0, (int) items.size() - 1, s.cursor);
			auto &it = items[(size_t) idx];
			if (it.onAdjust) {
				it.onAdjust(+1);
			} else if (!it.quickActions.empty()) {
				if ((int) s.quickIndex.size() < (int) items.size()) s.quickIndex.resize(items.size(), 0);
				int &qi = s.quickIndex[(size_t) idx];
				qi = (qi + 1) % (int) it.quickActions.size();
			}
		}
	} else if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) {
			const int idx = juce::jlimit(0, (int) s.fields.size() - 1, s.cursor);
			auto &f = s.fields[(size_t) idx];
			if (s.verticalFields) *f.value = juce::jlimit(f.minValue, f.maxValue, *f.value + (f.leftRightStep != 0 ? f.leftRightStep : f.upDownStep));
			else if (f.leftRightStep != 0) *f.value = juce::jlimit(f.minValue, f.maxValue, *f.value + f.leftRightStep);
			else if (s.fields.size() == 1) *f.value = juce::jlimit(f.minValue, f.maxValue, *f.value + f.upDownStep);
			else s.cursor = (s.cursor + 1) % (int) s.fields.size();
		}
	} else if (s.kind == ScreenKind::confirm) {
		s.confirmYes = true;
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditMoveCaret(+1);
	}
	repaint();
}

// See the header's callback convention comment: list items may push/pop freely (we never
// hold a reference into `stack` across their call, `items` is a local copy); form/confirm
// onConfirm may only touch engine()/processor state, since popScreen() right after is what
// actually closes the screen.
void D110SequencerRetroPanel::pressEnter() {
	// While the STEP RECORDING overlay owns the whole glass (stack empty), ENTER used to be
	// a dead no-op - the only way to reach DURATION/DOT while recording was to stop first,
	// which broke the whole point of changing them mid-take. Open the same STEP REC screen
	// REC -> STEP REC reaches, live-reflecting engine state, so recording carries on
	// underneath while it's open (Alan's request, 2026-08-23: needs to be reachable with
	// just the 6 D-pad buttons, no dead ends).
	if (stack.empty() && engine().isStepRecording()) { pushScreen(buildStepMenu()); return; }
	auto &s = top();
	const ScreenKind kind = s.kind;
	if (kind == ScreenKind::list) {
		auto items = s.buildItems();
		const int listCursor = s.cursor;
		if (listCursor < 0 || listCursor >= (int) items.size()) return;
		auto &item = items[(size_t) listCursor];
		if (!item.quickActions.empty()) {
			if ((int) s.quickIndex.size() < (int) items.size()) s.quickIndex.resize(items.size(), 0);
			const int qi = juce::jlimit(0, (int) item.quickActions.size() - 1, s.quickIndex[(size_t) listCursor]);
			auto &qa = item.quickActions[(size_t) qi];
			if (qa.enabled && qa.onEnter) qa.onEnter();
		} else if (item.enabled && item.onEnter) {
			item.onEnter();
		}
		repaint();
		return;
	}
	if (kind == ScreenKind::form) {
		auto onConfirm = s.onConfirm;
		if (onConfirm) onConfirm();
		popScreen();
		return;
	}
	if (kind == ScreenKind::confirm) {
		const bool yes = s.confirmYes;
		auto onConfirm = s.onConfirm;
		if (yes && onConfirm) onConfirm();
		popScreen();
		return;
	}
	if (kind == ScreenKind::nameEdit) {
		nameEditCommit();
		return;
	}
}

void D110SequencerRetroPanel::pressExit() {
	// A running transport takes priority over navigation - one BACK press is a full STOP
	// (same as the STOP button, not just stopRecording()), wherever in the menu tree you
	// happen to be, rather than just popping a screen (Alan's request, 2026-08-23). Step
	// recording rides along the same rule (Alan's request, 2026-08-23: "on quitte le mode
	// step avec STOP ou back") - pressStop() -> engine().stop() folds it in the same way it
	// already folds an in-progress real-time take.
	if (engine().isRecording() || engine().isPlaying() || engine().isStepRecording()) {
		pressStop();
		return;
	}
	if (!stack.empty()) {
		popScreen();
		return;
	}
	// Already on HOME (stack empty, nothing left to pop) - BACK jumps the cursor to the
	// transportRowLabel() row instead of doing nothing, so it's always a way back to a known
	// place regardless of how far down the list you scrolled (Alan's request, 2026-08-23;
	// re-targeted the same day from a hardcoded row 0 to this row specifically once it moved
	// off the top of HOME - see buildHomeMenu()'s own header comment) - and resets that row's
	// own dialled action to PLAY, same as popScreen() does when landing on HOME from a
	// submenu (see its own comment).
	const int idx = homeTransportRowIndex();
	const bool quickIndexNeedsReset = idx < (int) homeScreen.quickIndex.size() && homeScreen.quickIndex[(size_t) idx] != 0;
	if (homeScreen.cursor != idx || quickIndexNeedsReset) {
		homeScreen.cursor = idx;
		if (quickIndexNeedsReset) homeScreen.quickIndex[(size_t) idx] = 0;
	} else {
		// Cursor's already sitting right there with nothing left to reset - a further BACK
		// rewinds the bar to getStartBar(), same destination double-tapping STOP reaches
		// (Alan's request, 2026-08-24) - the entry point BACK keeps landing you on is exactly
		// where STOP itself lives, so a repeat press there reads the same way either input.
		engine().gotoBar(engine().getStartBar());
	}
	repaint();
}

void D110SequencerRetroPanel::pressTrackRec(int track) {
	auto &eng = engine();
	if (eng.isRecording() && eng.getArmedTrack() == track) {
		eng.stopRecording();
	} else {
		eng.armTrack(track);
		eng.startRecording();
	}
	repaint();
}

// Same one-press toggle as pressTrackRec(), for step recording instead of real-time -
// arms the track and starts step recording if it isn't already this track, stops it if it
// is (Alan's request, 2026-08-23: a STEP quick action per track, same as REC already has).
void D110SequencerRetroPanel::pressTrackStep(int track) {
	auto &eng = engine();
	if (eng.isStepRecording() && eng.getArmedTrack() == track) {
		eng.stopStepRecording();
	} else {
		eng.armTrack(track);
		eng.startStepRecording();
		stepOverlayCursor = 0; // always land on BAR, not wherever it was left last time
	}
	repaint();
}

void D110SequencerRetroPanel::visibilityChanged() {
	if (isVisible()) grabKeyboardFocus();
}

// Numeric D-pad, using the TOP-ROW digit keys (plain juce::KeyPress('8') etc.), not the
// physical numeric keypad: 8/2/4/6 for UP/DOWN/LEFT/RIGHT, 7/9 for EXIT/ENTER directly
// above LEFT/RIGHT - same digit layout as a numpad, just triggered off the row every
// keyboard has, keypad or not. This replaces an earlier version keyed to
// juce::KeyPress::numberPadN, which only ever fires with NumLock ON (Alan's report,
// 2026-08-23: it showed as "numpad 8" and needed NumLock unlocked to use); that version
// also had to swap DOWN from 2 to 5 to dodge numberPad2 colliding with the hardcoded Down
// arrow fallback below when NumLock is OFF (a physical numpad with NumLock off sends
// navigation keysyms instead of digits) - top-row '2' has no such collision (NumLock never
// touches the top row), so DOWN reverts to the natural 2, matching the pre-existing binding
// still saved from before that same-day change.
std::array<juce::KeyPress, D110SequencerRetroPanel::kBindingCount>
D110SequencerRetroPanel::defaultRetroKeyBindings() {
	std::array<juce::KeyPress, kBindingCount> b;
	b[bindUp] = juce::KeyPress((int) '8');
	b[bindDown] = juce::KeyPress((int) '2');
	b[bindLeft] = juce::KeyPress((int) '4');
	b[bindRight] = juce::KeyPress((int) '6');
	b[bindEnter] = juce::KeyPress((int) '9');
	b[bindExit] = juce::KeyPress((int) '7');
	return b;
}

juce::String D110SequencerRetroPanel::encodeKeyBindings(const std::array<juce::KeyPress, kBindingCount> &b) {
	juce::StringArray parts;
	for (auto &k : b) parts.add(k.getTextDescription());
	return parts.joinIntoString(";");
}

// Falls back to the factory default for the WHOLE set if the encoded string doesn't even
// have the right number of entries (an old settings/project file predating this feature, or
// anything else unparseable), and per-slot to that slot's own default if just one entry
// fails to decode - either way, a corrupt/outdated value never leaves a slot silently unbound.
std::array<juce::KeyPress, D110SequencerRetroPanel::kBindingCount>
D110SequencerRetroPanel::decodeKeyBindings(const juce::String &encoded) {
	auto result = defaultRetroKeyBindings();
	juce::StringArray parts;
	parts.addTokens(encoded, ";", "");
	if (parts.size() != kBindingCount) return result;
	for (int i = 0; i < kBindingCount; ++i) {
		const auto kp = juce::KeyPress::createFromDescription(parts[i]);
		if (kp.isValid()) result[(size_t) i] = kp;
	}
	return result;
}

bool D110SequencerRetroPanel::keyPressed(const juce::KeyPress &key) {
	// Rebinding capture (see buildKeyBindingsMenu()) - the very next key of ANY kind becomes
	// the new binding for whichever action is being rebound, consuming it rather than also
	// acting on it.
	if (capturingBinding >= 0) {
		keyBindings[(size_t) capturingBinding] = key;
		capturingBinding = -1;
		persistKeyBindings();
		repaint();
		return true;
	}

	// Plain arrow keys + Return/Backspace are a permanent fallback, unaffected by
	// customization - rebinding the numpad-style keys below can never lock anyone out.
	if (key.isKeyCode(juce::KeyPress::leftKey)) { pressLeft(); return true; }
	if (key.isKeyCode(juce::KeyPress::rightKey)) { pressRight(); return true; }
	if (key.isKeyCode(juce::KeyPress::upKey)) { pressUp(); return true; }
	if (key.isKeyCode(juce::KeyPress::downKey)) { pressDown(); return true; }
	if (key.isKeyCode(juce::KeyPress::returnKey)) { pressEnter(); return true; } // Enter and Return both map here
	if (key.isKeyCode(juce::KeyPress::backspaceKey)) { pressExit(); return true; }

	if (key.isKeyCode(keyBindings[bindUp].getKeyCode())) { pressUp(); return true; }
	if (key.isKeyCode(keyBindings[bindDown].getKeyCode())) { pressDown(); return true; }
	if (key.isKeyCode(keyBindings[bindLeft].getKeyCode())) { pressLeft(); return true; }
	if (key.isKeyCode(keyBindings[bindRight].getKeyCode())) { pressRight(); return true; }
	if (key.isKeyCode(keyBindings[bindEnter].getKeyCode())) { pressEnter(); return true; }
	if (key.isKeyCode(keyBindings[bindExit].getKeyCode())) { pressExit(); return true; }
	return false;
}

void D110SequencerRetroPanel::mouseDown(const juce::MouseEvent &e) {
	grabKeyboardFocus();
	const auto p = e.position;
	if (e.mods.isPopupMenu() && stopBounds.contains(p)) { d110seq::showTransportMenu(processor); return; }
	if (e.mods.isPopupMenu() && playBounds.contains(p)) { d110seq::showTransportMenu(processor); return; }
	if (stopBounds.contains(p)) { pressStop(); return; }
	if (playBounds.contains(p)) { pressPlay(); return; }
	if (recBounds.contains(p)) { pressRec(); return; }
	if (upBounds.contains(p)) { pressUp(); return; }
	if (downBounds.contains(p)) { pressDown(); return; }
	if (leftBounds.contains(p)) { pressLeft(); return; }
	if (rightBounds.contains(p)) { pressRight(); return; }
	if (enterBounds.contains(p)) { pressEnter(); return; }
	if (exitBounds.contains(p)) { pressExit(); return; }
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------

// HOME: the permanent base of the navigation stack (see top()). One row per rubric, in the
// order Alan settled on 2026-08-23: TEMPO/SIG/METRO and PRECOUNT/LOOP (plain rows, ENTER
// opens a thin list), SONG (a quick-bar row, LEFT/RIGHT cycles a fast action, ENTER fires
// it), BAR (a live scrub - LEFT/RIGHT moves the transport directly, ENTER still opens the
// full bar menu for exact jumps/punch/bar-range ops), then the transportRowLabel() row
// (PLAY/STOP/REC/[MIDI]/OPTIONS, also quick-bar) sitting right above the tracks it
// controls, then each TRACK (also quick-bar). It used to lead HOME, above even
// TEMPO/SIG/METRO; moved down here the same day so the rubrics that configure a song sit
// together above it, right before the transport that plays that song and the tracks that
// make it up - see homeTransportRowIndex(), which the BACK-to-known-place logic below
// depends on to still find this row wherever it ends up in this ordering.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildHomeMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "HOME";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		auto &eng = engine();

		items.push_back({ "TEMPO/SIG/METRO", "", true, [this] { pushScreen(buildTempoSigMetroMenu()); } });
		items.push_back({ "PRECOUNT/LOOP", "", true, [this] { pushScreen(buildPrecountLoopMenu()); } });

		{
			std::vector<QuickAction> qa;
			for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
				juce::String label = "SLOT" + juce::String(slot + 1);
				if (slot == eng.getCurrentSongSlot()) label += "<";
				qa.push_back({ label, [this, slot] { engine().selectSongSlot(slot); repaint(); }, true });
			}
			qa.push_back({ "NEW", [this] { pushScreen(buildNewSongConfirm()); }, true });
			qa.push_back({ "COPY", [this] { pushScreen(buildSongCopyMenu()); }, true });
			if (processor.supportsSoundSnapshots())
				qa.push_back({ "SNAPSHOT", [this] { pushScreen(buildSnapshotMenu()); }, true });
			items.push_back({ "SONG", "", true, nullptr, qa });
		}

		items.push_back({ "BAR", juce::String(eng.getCurrentBar()) + "/" + juce::String(eng.getBarCount()), true,
		                   [this] { pushScreen(buildBarMenu()); }, {},
		                   [this](int delta) {
			                   engine().gotoBar(juce::jmax(1, engine().getCurrentBar() + delta));
			                   repaint();
		                   } });

		// Just below BAR, just above PART 1 (Alan's request, 2026-08-23) - see this
		// function's own header comment for the reasoning.
		{
			std::vector<QuickAction> qa;
			qa.push_back({ "PLAY", [this] { pressPlay(); }, true });
			qa.push_back({ "STOP", [this] { pressStop(); }, true });
			qa.push_back({ "REC", [this] { pushScreen(buildRecordMenu()); }, true });
			if (processor.supportsTrackChannelEdit())
				qa.push_back({ "MIDI", [this] { pushScreen(buildMidiChannelsMenu()); }, true });
			qa.push_back({ "OPTIONS", [this] { pushScreen(buildOptionsMenu()); }, true });
			const juce::String status = eng.isRecording() ? "REC" : eng.isPlaying() ? "PLAY" : "STOP";
			items.push_back({ processor.transportRowLabel(), status, true, nullptr, qa });
		}

		for (int t = 0; t < eng.activeTrackCount(); ++t) {
			juce::String name = eng.getTrackName(t);
			if (name.isEmpty()) name = defaultTrackLabel(t);
			juce::String flags;
			if (eng.isTrackMuted(t)) flags += "M";
			if (eng.isTrackSoloed(t)) flags += "S";
			if (eng.getArmedTrack() == t) flags += "A";
			if (!flags.isEmpty()) name += " " + flags;

			std::vector<QuickAction> qa;
			qa.push_back({ "REC", [this, t] { pressTrackRec(t); }, true });
			qa.push_back({ "STEP", [this, t] { pressTrackStep(t); }, true });
			qa.push_back({ "PLAY", [this] { pressPlay(); }, true });
			qa.push_back({ "SOLO", [this, t] {
				               auto &e = engine();
				               e.setTrackSoloed(t, !e.isTrackSoloed(t));
				               repaint();
			               }, true });
			qa.push_back({ "MUTE", [this, t] {
				               auto &e = engine();
				               e.setTrackMuted(t, !e.isTrackMuted(t));
				               repaint();
			               }, true });
			qa.push_back({ "COPY", [this, t] { pushScreen(buildCopyBarsForm(t)); }, eng.trackHasEvents(t) });
			qa.push_back({ "CLEAR", [this, t] { pushScreen(buildClearConfirm(t)); }, eng.trackHasEvents(t) });
			qa.push_back({ "UNDO", [this] { engine().undo(); repaint(); }, eng.canUndo() });
			qa.push_back({ "REDO", [this] { engine().redo(); repaint(); }, eng.canRedo() });
			qa.push_back({ "MORE", [this, t] { pushScreen(buildTrackMenu(t)); }, true });
			items.push_back({ name, "", true, nullptr, qa });
		}

		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTempoSigMetroMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "TEMPO/SIG/METRO";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		// onAdjust mirrors buildTempoForm()'s own leftRightStep (1 BPM) so scrubbing here and
		// scrubbing once inside the form feel identical - entering the form still additionally
		// exposes its own faster UP/DOWN step (5.5 BPM) for quick swings (Alan's request,
		// 2026-08-23, same "skip the submenu" treatment HOME's BAR row already got).
		items.push_back({ "TEMPO", juce::String(eng.getTempo(), 0) + "BPM", true, [this] { pushScreen(buildTempoForm()); }, {},
		                   [this](int delta) {
			                   engine().setTempo(engine().getTempo() + delta);
			                   repaint();
		                   } });
		// Each ENTER press is one tap - the value column doubles as a live readout, so the
		// beats-per-minute figure updates right here as taps land (same idea as the mouse
		// view's TAP button, sharing D110SequencerEngine::registerTapTempo()).
		items.push_back({ "TAP TEMPO", juce::String(eng.getTempo(), 1) + "BPM", true, [this] {
			                 engine().registerTapTempo();
			                 repaint();
		                 } });
		items.push_back({ "TIME SIG", juce::String(eng.getTimeSigNumerator()) + "/" + juce::String(eng.getTimeSigDenominator()),
		                   true, [this] { pushScreen(buildTimeSigMenu()); } });
		items.push_back(
			{ "METRONOME", eng.getMetronomeEnabled() ? "ON" : "OFF", true, [this] { pushScreen(buildMetronomeMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildPrecountLoopMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "PRECOUNT/LOOP";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		// Both rows get an onAdjust for the same "skip the submenu for a simple change" reason
		// as HOME's BAR row and TEMPO above - ENTER still opens the full submenu (Alan's
		// request, 2026-08-23).
		items.push_back({ "PRECOUNT", juce::String(eng.getPrecountBars()), true, [this] { pushScreen(buildPrecountForm()); }, {},
		                   [this](int delta) {
			                   engine().setPrecountBars(engine().getPrecountBars() + delta);
			                   repaint();
		                   } });
		items.push_back({ "LOOP", loopModeShortLabel(eng.getLoopMode()), true, [this] { pushScreen(buildLoopMenu()); }, {},
		                   [this](int delta) {
			                   using d110seq::LoopMode;
			                   static constexpr LoopMode order[] = { LoopMode::off, LoopMode::bar, LoopMode::punch };
			                   int idx = 0;
			                   for (int i = 0; i < 3; ++i)
				                   if (order[i] == engine().getLoopMode()) idx = i;
			                   idx = (idx + delta + 3) % 3;
			                   engine().setLoopMode(order[idx]);
			                   repaint();
		                   } });
		// Where double-tap STOP / a further BACK on the transport row rewinds to (Alan's
		// request, 2026-08-24) - see D110SequencerEngine::setStartBar()'s own comment.
		items.push_back({ "STARTING POINT", juce::String(eng.getStartBar()), true,
		                   [this] { pushScreen(buildStartingPointForm()); }, {}, [this](int delta) {
			                   engine().setStartBar(engine().getStartBar() + delta);
			                   repaint();
		                   } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildMidiChannelsMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "MIDI CHANNELS";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		auto &eng = engine();
		for (int t = 0; t < eng.activeTrackCount(); ++t) {
			juce::String name = eng.getTrackName(t);
			if (name.isEmpty()) name = defaultTrackLabel(t);
			items.push_back(
				{ name, juce::String(eng.channelForTrack(t)), true, [this, t] { pushScreen(buildChannelForm(t)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildOptionsMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "OPTIONS";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back({ "FILE", "", true, [this] { pushScreen(buildFileMenu()); } });
		items.push_back({ "KEY BINDINGS", "", true, [this] { pushScreen(buildKeyBindingsMenu()); } });
		// Bigger text, fewer rows visible at once - Alan's request, 2026-08-23. See
		// bodyRows() and paintLcd()'s own comments for what actually changes per screen kind.
		items.push_back({ "LCD LINES", lcdCompactMode ? "2" : "4", true, [this] {
			                 lcdCompactMode = !lcdCompactMode;
			                 processor.setRetroLcdCompactMode(lcdCompactMode);
			                 repaint();
		                 } });
		// Same engine-wide setting as the outer app Options dialog's own "Quantize mode" row
		// (NonetSeqMain.cpp/PluginEditor.cpp) - Alan's request, 2026-08-23: reachable from
		// inside the sequencer itself too, retro or not, without having to back out to that
		// separate dialog. HARD moves a track's own recorded notes onto the grid for good;
		// SOFT leaves them exactly as played and only snaps them live during playback.
		{
			auto &eng = engine();
			items.push_back({ "QUANTIZE MODE", eng.getQuantizeMode() == d110seq::QuantizeMode::soft ? "SOFT" : "HARD",
			                   true, [this] {
				                   auto &e = engine();
				                   e.setQuantizeMode(e.getQuantizeMode() == d110seq::QuantizeMode::soft
				                                          ? d110seq::QuantizeMode::hard
				                                          : d110seq::QuantizeMode::soft);
				                   repaint();
			                   } });
		}
		// ENTER opens a list of every pending step (buildUndoListMenu()/buildRedoListMenu())
		// instead of firing a single undo()/redo() directly - Alan's request, 2026-08-23: a
		// value column showing what undo/redo would actually do (e.g. "CLEAR TRACK PART 2")
		// used to visually collide with the "UNDO"/"REDO" label for anything longer than a
		// few characters (the label/value split only ever gives the value ~38% of the row).
		// Listing every step by number sidesteps that AND lets you jump back/forward more
		// than one step in a single ENTER, which a single fixed row never could.
		{
			auto &eng = engine();
			items.push_back(
				{ "UNDO", "", eng.canUndo(), [this] { pushScreen(buildUndoListMenu()); } });
			items.push_back(
				{ "REDO", "", eng.canRedo(), [this] { pushScreen(buildRedoListMenu()); } });
		}
		if (processor.supportsExtraTracks()) {
			auto &eng = engine();
			const bool on = eng.getExtraTracksEnabled();
			items.push_back({ "EXTRA TRACKS", on ? "ON" : "OFF", true, [this] {
				                 auto &e = engine();
				                 e.setExtraTracksEnabled(!e.getExtraTracksEnabled());
				                 repaint();
			                 } });
		}
		// Both directions between the live patch and every track's stored Program Change/Bank/
		// Volume/Pan - see D110SequencerHost.h's own comments on resyncProgramChanges() (this
		// pushes stored -> live) and captureLivePatchIntoTracks() (the reverse, pulls live ->
		// stored, destructive so it confirms first via buildCaptureLivePatchConfirm()). Both are
		// D-110-only: Nonet Sequencer has no live patch (no synth to read back), so it has
		// nothing to sync with - hide both rather than expose a "live patch" that doesn't exist.
		if (processor.supportsCaptureLivePatch()) {
			items.push_back({ "SYNC: TO PATCH", "", true, [this] { processor.resyncProgramChanges(); } });
			items.push_back(
				{ "SYNC: FROM PATCH", "", true, [this] { pushScreen(buildCaptureLivePatchConfirm()); } });
		}
		return items;
	};
	return s;
}

// One row per pending step, most recent first (stepsBack 0 == what a single undo() would
// do), numbered so ENTER on row N undoes N steps at once - Alan's request, 2026-08-23, in
// place of a single fixed-width UNDO row whose value column used to visually collide with
// the label for anything but a short description. No value column here on purpose: the
// step count is folded into the label itself ("1. CLEAR TRACK...") so a long description
// has the whole row width to itself instead of being squeezed into ~38% of it.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildUndoListMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "UNDO";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		auto &eng = engine();
		for (int i = 0; i < eng.getUndoStackSize(); ++i) {
			const int steps = i + 1;
			items.push_back({ juce::String(steps) + ". " + eng.getUndoDescriptionAt(i).toUpperCase(), "", true,
			                   [this, steps] {
				                   for (int n = 0; n < steps; ++n) engine().undo();
				                   popScreen(); // this list
				                   popScreen(); // OPTIONS
			                   } });
		}
		return items;
	};
	return s;
}

// See buildUndoListMenu() - same idea, redoStack instead of undoStack.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildRedoListMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "REDO";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		auto &eng = engine();
		for (int i = 0; i < eng.getRedoStackSize(); ++i) {
			const int steps = i + 1;
			items.push_back({ juce::String(steps) + ". " + eng.getRedoDescriptionAt(i).toUpperCase(), "", true,
			                   [this, steps] {
				                   for (int n = 0; n < steps; ++n) engine().redo();
				                   popScreen(); // this list
				                   popScreen(); // OPTIONS
			                   } });
		}
		return items;
	};
	return s;
}

// Rebindable D-pad keys (Alan's request, 2026-08-23) - one row per action, ENTER on a row
// starts capture (capturingBinding), and the very next key press of any kind - handled at
// the top of keyPressed(), before the normal dispatch - becomes that action's new binding.
// "RESET TO DEFAULT" restores the numpad-based factory mapping in one step.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildKeyBindingsMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "KEY BINDINGS";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		auto row = [this](const char *label, BindingIndex idx) {
			const juce::String value =
				capturingBinding == (int) idx ? "PRESS A KEY" : keyBindings[(size_t) idx].getTextDescription().toUpperCase();
			return ListItem{ label, value, true, [this, idx] { capturingBinding = (int) idx; repaint(); } };
		};
		items.push_back(row("UP", bindUp));
		items.push_back(row("DOWN", bindDown));
		items.push_back(row("LEFT", bindLeft));
		items.push_back(row("RIGHT", bindRight));
		items.push_back(row("ENTER", bindEnter));
		items.push_back(row("BACK", bindExit));
		items.push_back({ "RESET TO DEFAULT", "", true, [this] {
			                 keyBindings = defaultRetroKeyBindings();
			                 persistKeyBindings();
			                 repaint();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTrackMenu(int track) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = defaultTrackLabel(track);
	s.buildItems = [this, track]() {
		std::vector<ListItem> items;
		auto &eng = engine();
		items.push_back({ "MUTE", eng.isTrackMuted(track) ? "ON" : "OFF", true, [this, track] {
			                 auto &e = engine();
			                 e.setTrackMuted(track, !e.isTrackMuted(track));
			                 repaint();
		                 } });
		items.push_back({ "SOLO", eng.isTrackSoloed(track) ? "ON" : "OFF", true, [this, track] {
			                 auto &e = engine();
			                 e.setTrackSoloed(track, !e.isTrackSoloed(track));
			                 repaint();
		                 } });
		items.push_back({ "ARM", eng.getArmedTrack() == track ? "ON" : "OFF", true, [this, track] {
			                 auto &e = engine();
			                 e.armTrack(e.getArmedTrack() == track ? -1 : track);
			                 repaint();
		                 } });
		items.push_back({ "RENAME", "", true, [this, track] { startNameEdit(track); } });
		if (processor.supportsTrackChannelEdit())
			items.push_back({ "CHANNEL", juce::String(eng.channelForTrack(track)), true,
			                   [this, track] { pushScreen(buildChannelForm(track)); } });
		items.push_back({ "QUANTIZE", "", true, [this, track] { pushScreen(buildQuantizeMenu(track)); } });
		// Rhythm has no Program Change equivalent but does have its own fixed Volume/Pan
		// (2026-08-21, Alan's request) - "CC CHANGE" there instead, since PRG/BANK don't apply.
		if (processor.supportsProgramChangeForTrack(track)) {
			const int program = processor.getTrackProgram(track);
			items.push_back({ "PROGRAM CHG", program < 0 ? "OFF" : juce::String(program + 1), true,
			                   [this, track] { pushScreen(buildProgramForm(track)); } });
		} else if (processor.supportsTrackVolumePanForTrack(track)) {
			const int volume = processor.getTrackVolume(track);
			const int pan = processor.getTrackPan(track);
			juce::StringArray parts;
			if (volume >= 0) parts.add("V" + juce::String(volume));
			if (pan >= 0) parts.add("P" + juce::String(pan));
			items.push_back({ "CC CHANGE", parts.isEmpty() ? "OFF" : parts.joinIntoString(" "), true,
			                   [this, track] { pushScreen(buildProgramForm(track)); } });
		}
		items.push_back(
			{ "CLEAR TRACK", "", eng.trackHasEvents(track), [this, track] { pushScreen(buildClearConfirm(track)); } });
		items.push_back(
			{ "DELETE BARS", "", eng.getBarCount() >= 1, [this, track] { pushScreen(buildDeleteBarsForm(track)); } });
		items.push_back(
			{ "COPY BARS", "", eng.trackHasEvents(track), [this, track] { pushScreen(buildCopyBarsForm(track)); } });
		items.push_back(
			{ "TRANSPOSE", "", eng.trackHasEvents(track), [this, track] { pushScreen(buildTransposeForm(track)); } });
		items.push_back({ "EDIT EVENTS", "", true, [this, track] { pushScreen(buildEventList(track)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildQuantizeMenu(int track) {
	using d110seq::QuantizeGrid;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "QUANTIZE";
	s.buildItems = [this, track]() {
		static const std::array<std::pair<QuantizeGrid, const char *>, 7> presets { {
			{ QuantizeGrid::off, "OFF" }, { QuantizeGrid::quarter, "1/4" }, { QuantizeGrid::eighth, "1/8" },
			{ QuantizeGrid::sixteenth, "1/16" }, { QuantizeGrid::eighthTriplet, "1/8 T" },
			{ QuantizeGrid::sixteenthTriplet, "1/16 T" }, { QuantizeGrid::thirtySecond, "1/32" } } };
		const auto current = engine().getTrackQuantize(track);
		std::vector<ListItem> items;
		for (const auto &preset : presets) {
			const QuantizeGrid grid = preset.first;
			items.push_back({ preset.second, grid == current ? "*" : "", true, [this, track, grid] {
				                 engine().pushUndoSnapshot("Quantize (" + defaultTrackLabel(track) + ")");
				                 engine().quantizeTrack(track, grid);
				                 popScreen();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildChannelForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "CHANNEL";
	auto value = std::make_shared<int>(engine().channelForTrack(track));
	s.fields.push_back({ "CH", value, 1, 16, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, track, value] { processor.setTrackChannel(track, *value); };
	return s;
}

// Rhythm (D-110 plugin only) has no Program Change equivalent but does have its own fixed
// Volume/Pan (2026-08-21, Alan's request) - hasProgram is false there, so PRG/BANK fields are
// skipped entirely and the screen title becomes "CC CHANGE".
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildProgramForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	// One field per line, UP/DOWN to move between them, LEFT/RIGHT to adjust - see
	// Screen::verticalFields's own comment. This form can hold up to 5 fields (Nonet
	// Sequencer, PRG+BANK(HIGH)+BANK(LOW)+VOL+PAN), which the default two-per-line grid
	// packed onto 3 rows with no room left for the value itself (Alan's report, 2026-08-26:
	// PRG's own value was invisible at PRG 11).
	s.verticalFields = true;
	const bool hasProgram = processor.supportsProgramChangeForTrack(track);
	s.title = hasProgram ? "PROGRAM CHG" : "CC CHANGE";
	const int currentProgram = hasProgram ? processor.getTrackProgram(track) : -1;
	const bool hasLsb = hasProgram && processor.supportsBankLsb();
	const bool hasVolPan = processor.supportsTrackVolumePanForTrack(track);
	auto program = std::make_shared<int>(currentProgram >= 0 ? currentProgram + 1 : 0);
	auto bank = std::make_shared<int>(hasProgram ? processor.getTrackBank(track) : 1);
	auto bankLsb = std::make_shared<int>(hasLsb ? processor.getTrackBankLsb(track) : 1);
	// Same "0 = OFF, else value+1" trick PRG uses above - 0 is otherwise a perfectly meaningful
	// LEVEL/PAN, so it can't double as "unset" the way it does for PRG (there, 0 is never a
	// real Program Change value, this dialog numbers those 1-128 like Bank).
	auto volume = std::make_shared<int>(hasVolPan ? processor.getTrackVolume(track) + 1 : 0);
	auto pan = std::make_shared<int>(hasVolPan ? processor.getTrackPan(track) + 1 : 0);
	if (hasProgram) {
		s.fields.push_back(
			{ "PRG(0=OFF)", program, 0, 128, [](int v) { return v == 0 ? juce::String("OFF") : juce::String(v); } });
		// MIDI has two Bank Select controllers (CC0/high/MSB, CC32/low/LSB) - see
		// D110SequencerPanel::promptForTrackProgram's own comment. Only NonetSeqHost sends either;
		// the D-110 has neither, so this second field only shows up there.
		s.fields.push_back({ hasLsb ? "BANK(HIGH)" : "BANK", bank, 1, 128, [](int v) { return juce::String(v); } });
		if (hasLsb) s.fields.push_back({ "BANK(LOW)", bankLsb, 1, 128, [](int v) { return juce::String(v); } });
	}
	if (hasVolPan) {
		s.fields.push_back({ "VOL(0=OFF)", volume, 0, 101,
		                      [](int v) { return v == 0 ? juce::String("OFF") : juce::String(v - 1); } });
		s.fields.push_back({ "PAN(0=OFF)", pan, 0, 15,
		                      [](int v) { return v == 0 ? juce::String("OFF") : juce::String(v - 1); } });
	}
	s.onConfirm = [this, track, program, bank, bankLsb, hasProgram, hasLsb, volume, pan, hasVolPan] {
		if (hasProgram) {
			processor.setTrackProgram(track, *program == 0 ? -1 : *program - 1);
			processor.setTrackBank(track, *bank);
			if (hasLsb) processor.setTrackBankLsb(track, *bankLsb);
		}
		if (hasVolPan) {
			processor.setTrackVolume(track, *volume == 0 ? -1 : *volume - 1);
			processor.setTrackPan(track, *pan == 0 ? -1 : *pan - 1);
		}
	};
	return s;
}

// Pull direction - see D110SequencerHost.h's captureLivePatchIntoTracks() comment. Destructive
// to every track's stored Program Change/Bank/Volume/Pan (not undoable - those live on the
// processor, outside the engine's own undo stack, same as every other setTrackProgram()/
// setTrackBank() call already is), hence the confirm.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildCaptureLivePatchConfirm() {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "CAPTURE PATCH?";
	s.message = "OVERWRITES EVERY TRACK'S PROGRAM/BANK/VOL/PAN WITH THE LIVE PATCH - CAN'T UNDO";
	s.onConfirm = [this] { processor.captureLivePatchIntoTracks(); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildClearConfirm(int track) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "CLEAR TRACK?";
	s.message = "ERASES ALL NOTES ON " + defaultTrackLabel(track);
	s.onConfirm = [this, track] {
		engine().pushUndoSnapshot("Clear track (" + defaultTrackLabel(track) + ")");
		engine().clearTrack(track);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildDeleteBarsForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = track < 0 ? "DEL BARS(ALL)" : "DELETE BARS";
	auto &eng = engine();
	auto from = std::make_shared<int>(eng.getCurrentBar());
	auto to = std::make_shared<int>(eng.getCurrentBar());
	s.fields.push_back({ "FROM", from, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "TO", to, 1, 9999, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, track, from, to] {
		if (*to >= *from) {
			const juce::String scope = track < 0 ? "all tracks" : defaultTrackLabel(track);
			engine().pushUndoSnapshot("Delete bars " + juce::String(*from) + "-" + juce::String(*to) + " (" + scope + ")");
			engine().deleteBars(track, *from, *to);
		}
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildCopyBarsForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = track < 0 ? "COPY BARS(ALL)" : "COPY BARS";
	auto &eng = engine();
	auto from = std::make_shared<int>(eng.getCurrentBar());
	auto to = std::make_shared<int>(eng.getCurrentBar());
	auto dest = std::make_shared<int>(eng.getBarCount() + 1);
	s.fields.push_back({ "FROM", from, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "TO", to, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "DEST BAR", dest, 1, 9999, [](int v) { return juce::String(v); } });
	std::shared_ptr<int> destTrack;
	if (track >= 0) {
		destTrack = std::make_shared<int>(track);
		const int maxTrack = juce::jmax(0, eng.activeTrackCount() - 1);
		s.fields.push_back(
			{ "DEST TRK", destTrack, 0, maxTrack, [](int v) { return D110SequencerRetroPanel::defaultTrackLabel(v); } });
	}
	s.onConfirm = [this, track, from, to, dest, destTrack] {
		if (*to >= *from && *dest >= 1) {
			const juce::String scope = track < 0 ? "all tracks" : defaultTrackLabel(track);
			engine().pushUndoSnapshot("Copy bars " + juce::String(*from) + "-" + juce::String(*to) + " (" + scope + ")");
			engine().copyBars(track, track >= 0 ? *destTrack : -1, *from, *to, *dest);
		}
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTransposeForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = track < 0 ? "TRANSPOSE(ALL)" : "TRANSPOSE";
	auto &eng = engine();
	auto from = std::make_shared<int>(eng.getCurrentBar());
	auto to = std::make_shared<int>(eng.getCurrentBar());
	auto semitones = std::make_shared<int>(0);
	s.fields.push_back({ "FROM", from, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "TO", to, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back(
		{ "SEMITONES", semitones, -127, 127, [](int v) { return (v > 0 ? juce::String("+") : juce::String()) + juce::String(v); } });
	s.onConfirm = [this, track, from, to, semitones] {
		if (*to >= *from && *semitones != 0) {
			const juce::String scope = track < 0 ? "all tracks" : defaultTrackLabel(track);
			engine().pushUndoSnapshot("Transpose bars " + juce::String(*from) + "-" + juce::String(*to) + " (" + scope + ")");
			engine().transposeBars(track, *from, *to, *semitones);
		}
	};
	return s;
}

// Not bar-navigable in place, unlike D110SequencerPanel::promptForEventList()'s own
// "< Bar N >" strip - operates on whatever bar HOME was on when TRACK > EDIT EVENTS was
// entered. A deliberate v1 simplification: EXIT back to TRACK MENU, change HOME's bar
// quick-field, re-enter EDIT EVENTS for a different bar.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventList(int track) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "EVT BAR " + juce::String(engine().getCurrentBar());
	const int bar = engine().getCurrentBar();
	s.buildItems = [this, track, bar]() {
		std::vector<ListItem> items;
		for (const auto &ev : engine().eventsInBarRange(track, bar, bar)) {
			const juce::String label = juce::MidiMessage::getMidiNoteName(ev.note, true, true, 4);
			const juce::String value = "v" + juce::String(ev.velocity);
			const int idx = ev.index;
			const int note = ev.note;
			items.push_back({ label, value, true, [this, track, idx, note] { pushScreen(buildEventItemMenu(track, idx, note)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventItemMenu(int track, int eventIndex, int note) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = juce::MidiMessage::getMidiNoteName(note, true, true, 4);
	s.buildItems = [this, track, eventIndex, note]() {
		std::vector<ListItem> items;
		items.push_back({ "EDIT PITCH", "", true,
		                   [this, track, eventIndex, note] { pushScreen(buildEventPitchForm(track, eventIndex, note)); } });
		items.push_back(
			{ "DELETE", "", true, [this, track, eventIndex] { pushScreen(buildEventDeleteConfirm(track, eventIndex)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventPitchForm(int track, int eventIndex, int note) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "EDIT PITCH";
	auto value = std::make_shared<int>(note);
	s.fields.push_back({ "NOTE", value, 0, 127, [](int v) { return juce::MidiMessage::getMidiNoteName(v, true, true, 4); } });
	s.onConfirm = [this, track, eventIndex, value] {
		engine().pushUndoSnapshot("Edit note pitch (" + defaultTrackLabel(track) + ")");
		engine().setNoteEventPitch(track, eventIndex, *value);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventDeleteConfirm(int track, int eventIndex) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "DELETE NOTE?";
	s.message = "REMOVES THIS NOTE EVENT";
	s.onConfirm = [this, track, eventIndex] {
		engine().pushUndoSnapshot("Delete note (" + defaultTrackLabel(track) + ")");
		engine().deleteNoteEvent(track, eventIndex);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTempoForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "TEMPO";
	// Held in half-BPM units so LEFT/RIGHT's 1 BPM step and UP/DOWN's 5.5 BPM step (Alan's
	// own numbers, 2026-08-18) both land on whole units - 2 and 11 half-units respectively.
	auto value = std::make_shared<int>((int) std::lround(engine().getTempo() * 2.0));
	s.fields.push_back({ "BPM", value, 40, 600, [](int v) { return juce::String(v / 2.0, 1) + "BPM"; },
	                      /* upDownStep */ 11, /* leftRightStep */ 2 });
	s.onConfirm = [this, value] { engine().setTempo((double) *value / 2.0); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTimeSigMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "TIME SIG";
	s.buildItems = [this]() {
		static const std::array<std::pair<int, int>, 6> presets { { { 4, 4 }, { 3, 4 }, { 6, 8 }, { 2, 4 }, { 5, 4 },
		                                                              { 7, 8 } } };
		auto &eng = engine();
		std::vector<ListItem> items;
		for (const auto &preset : presets) {
			const bool current = preset.first == eng.getTimeSigNumerator() && preset.second == eng.getTimeSigDenominator();
			const int num = preset.first, den = preset.second;
			items.push_back({ juce::String(num) + "/" + juce::String(den), current ? "*" : "", true, [this, num, den] {
				                 engine().setTimeSignature(num, den);
				                 popScreen();
			                 } });
		}
		items.push_back({ "CUSTOM...", "", true, [this] { pushScreen(buildTimeSigCustomForm()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTimeSigCustomForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "CUSTOM SIG";
	auto num = std::make_shared<int>(engine().getTimeSigNumerator());
	auto den = std::make_shared<int>(engine().getTimeSigDenominator());
	s.fields.push_back({ "NUM", num, 1, 32, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "DEN", den, 1, 32, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, num, den] { engine().setTimeSignature(*num, *den); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildMetronomeMenu() {
	using d110seq::MetronomeMode;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "METRONOME";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		items.push_back({ "METRO", eng.getMetronomeEnabled() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setMetronomeEnabled(!e.getMetronomeEnabled());
			                 repaint();
		                 } });
		const auto mode = eng.getMetronomeMode();
		items.push_back({ "MODE", mode == MetronomeMode::visualOnly ? "VISUAL" : mode == MetronomeMode::audioOnly ? "AUDIO" : "BOTH",
		                   true, [this] {
			                   auto &e = engine();
			                   switch (e.getMetronomeMode()) {
				                   case MetronomeMode::visualOnly: e.setMetronomeMode(MetronomeMode::audioOnly); break;
				                   case MetronomeMode::audioOnly: e.setMetronomeMode(MetronomeMode::both); break;
				                   case MetronomeMode::both: e.setMetronomeMode(MetronomeMode::visualOnly); break;
			                   }
			                   repaint();
		                   } });
		items.push_back({ "USE CH10", eng.getMetronomeUseChannel10() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setMetronomeUseChannel10(!e.getMetronomeUseChannel10());
			                 repaint();
		                 } });
		items.push_back({ "REC ONLY", eng.getMetronomeRecordOnly() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setMetronomeRecordOnly(!e.getMetronomeRecordOnly());
			                 repaint();
		                 } });
		items.push_back({ "VOLUME", juce::String((int) std::lround(eng.getMetronomeVolume() * 100.0f)) + "%", true,
		                   [this] { pushScreen(buildMetronomeVolumeMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildMetronomeVolumeMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "METRO VOL";
	s.buildItems = [this]() {
		static constexpr float presets[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f };
		const float current = engine().getMetronomeVolume();
		std::vector<ListItem> items;
		for (float v : presets) {
			const bool selected = std::abs(current - v) < 0.01f;
			items.push_back({ juce::String((int) std::lround(v * 100.0f)) + "%", selected ? "*" : "", true, [this, v] {
				                 engine().setMetronomeVolume(v);
				                 popScreen();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildPrecountForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "PRECOUNT";
	auto value = std::make_shared<int>(engine().getPrecountBars());
	s.fields.push_back({ "BARS", value, 0, 2, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, value] { engine().setPrecountBars(*value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildLoopMenu() {
	using d110seq::LoopMode;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "LOOP";
	s.buildItems = [this]() {
		const auto current = engine().getLoopMode();
		std::vector<ListItem> items;
		items.push_back({ "OFF", current == LoopMode::off ? "*" : "", true, [this] {
			                 engine().setLoopMode(LoopMode::off);
			                 popScreen();
		                 } });
		items.push_back({ "BAR", current == LoopMode::bar ? "*" : "", true, [this] {
			                 engine().setLoopMode(LoopMode::bar);
			                 popScreen();
		                 } });
		items.push_back({ "PUNCH", current == LoopMode::punch ? "*" : "", true, [this] {
			                 engine().setLoopMode(LoopMode::punch);
			                 popScreen();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildBarMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "BAR";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		items.push_back({ "GO TO BAR", juce::String(eng.getCurrentBar()), true, [this] { pushScreen(buildGotoBarForm()); } });
		items.push_back({ "PUNCH IN HERE", "bar " + juce::String(eng.getCurrentBar()), true, [this] {
			                 engine().setPunchIn(engine().getCurrentBar());
			                 repaint();
		                 } });
		items.push_back({ "PUNCH OUT HERE", "bar " + juce::String(eng.getCurrentBar()), true, [this] {
			                 engine().setPunchOut(engine().getCurrentBar());
			                 repaint();
		                 } });
		items.push_back({ "PUNCH RANGE", juce::String(eng.getPunchIn()) + "-" + juce::String(eng.getPunchOut()), true,
		                   [this] { pushScreen(buildPunchForm()); } });
		items.push_back({ "DELETE BARS", "(all tracks)", true, [this] { pushScreen(buildDeleteBarsForm(-1)); } });
		items.push_back({ "COPY BARS", "(all tracks)", true, [this] { pushScreen(buildCopyBarsForm(-1)); } });
		items.push_back({ "TRANSPOSE", "(all tracks)", true, [this] { pushScreen(buildTransposeForm(-1)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildGotoBarForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "GO TO BAR";
	auto value = std::make_shared<int>(engine().getCurrentBar());
	// LEFT/RIGHT = 1 bar, UP/DOWN = 10 bars - same asymmetric-step idiom as TEMPO's own form
	// (buildTempoForm()), Alan's own numbers (2026-08-18).
	s.fields.push_back({ "BAR", value, 1, 9999, [](int v) { return juce::String(v); }, /* upDownStep */ 10,
	                      /* leftRightStep */ 1 });
	s.onConfirm = [this, value] { engine().gotoBar(*value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildStartingPointForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "STARTING POINT";
	auto value = std::make_shared<int>(engine().getStartBar());
	// Same asymmetric LEFT/RIGHT-1-bar/UP-DOWN-10-bars idiom as GO TO BAR above.
	s.fields.push_back({ "BAR", value, 1, 9999, [](int v) { return juce::String(v); }, /* upDownStep */ 10,
	                      /* leftRightStep */ 1 });
	s.onConfirm = [this, value] { engine().setStartBar(*value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildPunchForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "PUNCH RANGE";
	auto in = std::make_shared<int>(engine().getPunchIn());
	auto out = std::make_shared<int>(engine().getPunchOut());
	s.fields.push_back({ "IN", in, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "OUT", out, 1, 9999, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, in, out] { engine().setPunchRange(*in, *out); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildRecordMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "RECORD";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back(
			{ "REC MODE", recordModeShortLabel(engine().getRecordMode()), true, [this] { pushScreen(buildRecordModeMenu()); } });
		items.push_back({ "STEP REC", "", true, [this] { pushScreen(buildStepMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildRecordModeMenu() {
	using d110seq::RecordMode;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "REC MODE";
	s.buildItems = [this]() {
		const auto current = engine().getRecordMode();
		std::vector<ListItem> items;
		items.push_back({ "OVERDUB", current == RecordMode::overdub ? "*" : "", true, [this] {
			                 engine().setRecordMode(RecordMode::overdub);
			                 popScreen();
		                 } });
		items.push_back({ "REPLACE", current == RecordMode::replaceRange ? "*" : "", true, [this] {
			                 engine().setRecordMode(RecordMode::replaceRange);
			                 popScreen();
		                 } });
		items.push_back({ "REPLACE+END", current == RecordMode::replaceToEnd ? "*" : "", true, [this] {
			                 engine().setRecordMode(RecordMode::replaceToEnd);
			                 popScreen();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildStepMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "STEP REC";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		// startStepRecording() needs an armed track; without one ENTER used to silently
		// do nothing. Grey the row out and say why, same as COPY/CLEAR/UNDO/REDO below do
		// for their own preconditions, instead of leaving it looking actionable but dead.
		const bool canToggle = eng.isStepRecording() || eng.getArmedTrack() >= 0;
		items.push_back({ "STEP REC", eng.isStepRecording() ? "ON" : canToggle ? "OFF" : "ARM A TRACK", canToggle,
			               [this] {
				               auto &e = engine();
				               if (e.isStepRecording()) e.stopStepRecording();
				               else if (e.getArmedTrack() >= 0) { e.startStepRecording(); stepOverlayCursor = 0; }
				               repaint();
			               } });
		items.push_back({ "DURATION", stepDurationShortLabel(eng.getStepDuration()), true,
		                   [this] { pushScreen(buildStepDurationMenu()); } });
		items.push_back({ "DOT", eng.getStepDotted() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setStepDotted(!e.getStepDotted());
			                 repaint();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildStepDurationMenu() {
	using d110seq::QuantizeGrid;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "STEP DUR";
	s.buildItems = [this]() {
		const auto &presets = stepDurationPresets();
		const auto current = engine().getStepDuration();
		std::vector<ListItem> items;
		for (auto grid : presets)
			items.push_back({ stepDurationShortLabel(grid), grid == current ? "*" : "", true, [this, grid] {
				                 engine().setStepDuration(grid);
				                 popScreen();
			                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SONG";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back(
			{ "SLOT", juce::String(engine().getCurrentSongSlot() + 1), true, [this] { pushScreen(buildSongSlotList()); } });
		items.push_back({ "COPY TO SLOT", "", true, [this] { pushScreen(buildSongCopyMenu()); } });
		items.push_back({ "NEW SONG", "", true, [this] { pushScreen(buildNewSongConfirm()); } });
		if (processor.supportsSoundSnapshots())
			items.push_back({ "SOUND SNAPSHOT", "", true, [this] { pushScreen(buildSnapshotMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongSlotList() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SELECT SLOT";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
			juce::String value = eng.songSlotHasContent(slot) ? "*" : "";
			if (slot == eng.getCurrentSongSlot()) value += "<";
			items.push_back({ "SLOT " + juce::String(slot + 1), value, true, [this, slot] {
				                 engine().selectSongSlot(slot);
				                 popScreen();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongCopyMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "COPY SONG TO";
	s.buildItems = [this]() {
		auto &eng = engine();
		const int current = eng.getCurrentSongSlot();
		std::vector<ListItem> items;
		for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
			if (slot == current) continue;
			const juce::String value = eng.songSlotHasContent(slot) ? "OVERWRITE" : "";
			items.push_back(
				{ "SLOT " + juce::String(slot + 1), value, true, [this, slot] { pushScreen(buildSongCopyConfirm(slot)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongCopyConfirm(int destSlot) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "COPY SONG?";
	s.message = "OVERWRITES SLOT " + juce::String(destSlot + 1);
	s.onConfirm = [this, destSlot] {
		engine().pushUndoSnapshot("Copy song to Slot " + juce::String(destSlot + 1));
		engine().copyCurrentSongTo(destSlot);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildNewSongConfirm() {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "NEW SONG?";
	s.message = "CLEARS EVERY TRACK IN SLOT " + juce::String(engine().getCurrentSongSlot() + 1);
	s.onConfirm = [this] {
		engine().pushUndoSnapshot("New song (Slot " + juce::String(engine().getCurrentSongSlot() + 1) + ")");
		// newSong() also resets tempo and the fixed per-track Program Change/Bank/Volume/Pan
		// override for this slot's own tracks - see its own comment.
		engine().newSong();
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSnapshotMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SOUND SNAP";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
			const bool has = processor.hasSoundSnapshot(slot);
			items.push_back(
				{ "SLOT " + juce::String(slot + 1), has ? "STORED" : "", true, [this, slot] { pushScreen(buildSnapshotSlotMenu(slot)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSnapshotSlotMenu(int slot) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SLOT " + juce::String(slot + 1);
	s.buildItems = [this, slot]() {
		std::vector<ListItem> items;
		items.push_back({ "STORE HERE", "", true, [this, slot] {
			                 processor.storeSoundSnapshotForSlot(slot);
			                 popScreen();
		                 } });
		items.push_back({ "LOAD HERE", "", processor.hasSoundSnapshot(slot),
		                   [this, slot] { pushScreen(buildSnapshotLoadConfirm(slot)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSnapshotLoadConfirm(int slot) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "LOAD SOUNDS?";
	s.message = "REPLACES ALL CURRENT SOUNDS, POWER-CYCLES";
	s.onConfirm = [this, slot] { processor.loadSoundSnapshotForSlot(slot); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildFileMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "FILE";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back({ "LOAD SONG", ".MID", true, [this] { doLoadSong(); popScreen(); } });
		items.push_back({ "SAVE SONG", ".MID", true, [this] { doSaveSong(); popScreen(); } });
		items.push_back({ "LOAD ALL", ".MIDISEQ", true, [this] { doLoadAllSongs(); popScreen(); } });
		items.push_back({ "SAVE ALL", ".MIDISEQ", true, [this] { doSaveAllSongs(); popScreen(); } });
		return items;
	};
	return s;
}

// ---------------------------------------------------------------------------
// Character-wheel rename
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::startNameEdit(int track) {
	nameEditTrack = track;
	juce::String current = engine().getTrackName(track);
	if (current.isEmpty()) current = defaultTrackLabel(track);
	nameEditBuffer = current.toUpperCase().substring(0, kNameEditLength);
	while (nameEditBuffer.length() < kNameEditLength) nameEditBuffer += " ";
	nameEditCaret = 0;
	Screen s;
	s.kind = ScreenKind::nameEdit;
	s.title = "RENAME";
	pushScreen(s);
}

void D110SequencerRetroPanel::nameEditAdjust(int delta) {
	if (nameEditCaret < 0 || nameEditCaret >= nameEditBuffer.length()) return;
	int idx = kNameEditCharset.indexOfChar(nameEditBuffer[nameEditCaret]);
	if (idx < 0) idx = 0;
	idx = (idx + delta + kNameEditCharset.length()) % kNameEditCharset.length();
	nameEditBuffer = nameEditBuffer.replaceSection(nameEditCaret, 1, juce::String::charToString(kNameEditCharset[idx]));
	repaint();
}

void D110SequencerRetroPanel::nameEditMoveCaret(int delta) {
	nameEditCaret = juce::jlimit(0, kNameEditLength - 1, nameEditCaret + delta);
	repaint();
}

void D110SequencerRetroPanel::nameEditCommit() {
	engine().pushUndoSnapshot("Rename (" + defaultTrackLabel(nameEditTrack) + ")");
	engine().setTrackName(nameEditTrack, nameEditBuffer.trim());
	popScreen();
}

// ---------------------------------------------------------------------------
// File dialogs - same juce::FileChooser calls as D110SequencerPanel's own LOAD/SAVE
// handlers (see D110SequencerPanel::mouseDown()/showLoadMenu()/showSaveMenu()).
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::doLoadSong() {
	// "*.MID" alongside "*.mid" because Linux's native picker (zenity/kdialog) matches filters
	// with a case-sensitive glob - see PluginEditor.cpp's SysEx-import chooser
	// (TieredNativeFileChooser) for the fuller explanation and its own "*.*" fallback,
	// deliberately not duplicated here to keep this dialog's default filter narrow.
	auto *chooser = new juce::FileChooser("Load a MIDI file into the sequencer", processor.getLastDialogDir(), "*.mid;*.MID");
	chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
	                      [this, chooser](const juce::FileChooser &fc) {
		                      const auto file = fc.getResult();
		                      if (file != juce::File()) {
			                      processor.setLastDialogDir(file.getParentDirectory());
			                      engine().loadMidiFile(file);
		                      }
		                      delete chooser;
		                      repaint();
	                      });
}

void D110SequencerRetroPanel::doSaveSong() {
	auto *chooser = new juce::FileChooser("Save the sequencer as a MIDI file", processor.getLastDialogDir(), "*.mid;*.MID");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("mid")) file = file.withFileExtension("mid");
				processor.setLastDialogDir(file.getParentDirectory());
				engine().saveMidiFile(file);
			}
			delete chooser;
		});
}

void D110SequencerRetroPanel::doLoadAllSongs() {
	auto *chooser =
		new juce::FileChooser("Load all 4 sequencer songs", processor.getLastDialogDir(), "*.midiseq;*.d110songs");
	chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
	                      [this, chooser](const juce::FileChooser &fc) {
		                      const auto file = fc.getResult();
		                      if (file != juce::File()) {
			                      processor.setLastDialogDir(file.getParentDirectory());
			                      processor.importSequencerSongs(file);
		                      }
		                      delete chooser;
		                      repaint();
	                      });
}

void D110SequencerRetroPanel::doSaveAllSongs() {
	// Default filename dated rather than a bare "song.midiseq" - see D110SequencerPanel's own
	// showSaveMenu() comment.
	const auto defaultFile =
		processor.getLastDialogDir().getChildFile(juce::Time::getCurrentTime().formatted("song-%Y-%m-%d.midiseq"));
	auto *chooser = new juce::FileChooser("Save all 4 sequencer songs", defaultFile, "*.midiseq");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("midiseq")) file = file.withFileExtension("midiseq");
				processor.setLastDialogDir(file.getParentDirectory());
				processor.exportSequencerSongs(file);
			}
			delete chooser;
		});
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

juce::Rectangle<float> D110SequencerRetroPanel::layoutTransportAndDpad(juce::Rectangle<float> row, float cell,
                                                                        float transportColW) {
	auto transportCol = row.removeFromLeft(transportColW);
	stopBounds = transportCol.removeFromTop(cell).reduced(3.0f);
	playBounds = transportCol.removeFromLeft(transportCol.getWidth() * 0.5f).reduced(3.0f);
	recBounds = transportCol.reduced(3.0f);

	// Square cells (never stretched into a non-square shape just because the drawer happens
	// to be unusually wide or tall), anchored to the RIGHT edge of `row` rather than centred -
	// Alan wanted the RIGHT arrow pushed all the way to the right, not floating in the middle
	// of whatever space is left once the LCD's own share is subtracted (the caller does that,
	// not this method - see the returned leftover rectangle).
	auto grid = juce::Rectangle<float>(cell * 3.0f, cell * 2.0f).withY(row.getY()).withRightX(row.getRight());
	auto gridRow = [&](int r) { return grid.withY(grid.getY() + cell * (float) r).withHeight(cell); };
	auto colOf = [cell](juce::Rectangle<float> r, int c) { return r.withX(r.getX() + cell * (float) c).withWidth(cell); };

	exitBounds = colOf(gridRow(0), 0).reduced(3.0f);  // BACK
	upBounds = colOf(gridRow(0), 1).reduced(3.0f);
	enterBounds = colOf(gridRow(0), 2).reduced(3.0f); // ENTER
	leftBounds = colOf(gridRow(1), 0).reduced(3.0f);
	downBounds = colOf(gridRow(1), 1).reduced(3.0f);
	rightBounds = colOf(gridRow(1), 2).reduced(3.0f);

	row.removeFromRight(cell * 3.0f);
	return row;
}

void D110SequencerRetroPanel::resized() {
	auto area = getLocalBounds().toFloat();
	if (area.getWidth() < 1.0f || area.getHeight() < 1.0f) return;
	area = area.reduced(8.0f, 5.0f);

	if (area.getWidth() >= area.getHeight()) {
		// Landscape-ish (wider than tall - most desktop/Nonet Sequencer windows, Android
		// landscape): three columns sharing one horizontal band - transport (STOP on top,
		// PLAY/REC side by side below it) on the left, the D-pad cluster on the right, LCD
		// filling whatever's left in the middle - Alan's request, 2026-08-23. The LCD used to
		// get only a short fixed-height strip across the top, with both button clusters
		// squeezed into the remainder and a large dead gap between them (very visible on a
		// wide/short window - Android landscape in particular, where the old transport column
		// claimed 30% of a much wider area than it needed just to hold 3 buttons).
		//
		// The whole band's height (cell * 2, two rows of whatever `cell` comes out to) is
		// derived from a single `cell` unit - the same one the D-pad sizes its own square
		// buttons from - and then CENTRED in whatever height this component actually has,
		// rather than stretching every button to fill it. Without that centring, a container
		// offering a lot more height than width blows STOP/PLAY/REC up into enormous, oddly
		// elongated buttons - the D-pad itself was already immune (its own cell size was
		// always derived this same way), only the transport column wasn't, since it used to
		// take its row height from this component's FULL height instead of from `cell`.
		//
		// cell is capped by a fixed FRACTION of the total width (not "whatever's left after
		// the transport column"), guaranteeing the LCD always keeps a real share of it
		// regardless of aspect ratio (an earlier version derived cell from the leftover width
		// instead, which let the D-pad eat 100% of what remained and left the LCD with
		// nothing at all). transportColW is just cell * 2 - a SQUARE block (as wide as the
		// two-row band is tall), not its own separate width-based fraction - Alan's request,
		// 2026-08-23: the transport column doesn't need to be as wide as the D-pad cluster
		// just because it's a 3-button block rather than a 3x2 grid, and every pixel it isn't
		// using goes to the LCD instead.
		const float cell = juce::jmin(area.getWidth() * 0.30f / 3.0f, area.getHeight() / 2.0f);
		const float transportColW = cell * 2.0f;
		auto controlRow = area.withSizeKeepingCentre(area.getWidth(), cell * 2.0f);
		lcdBounds = layoutTransportAndDpad(controlRow, cell, transportColW).reduced(10.0f, 0.0f);
	} else {
		// Portrait-ish (taller than wide - Android portrait, or a desktop/Nonet Sequencer
		// window resized narrow) - Alan's request, 2026-08-23: the shared-row layout above
		// would squeeze the LCD's own share of the width down to almost nothing here, so
		// instead the LCD gets its own full-width strip on top, with the transport/D-pad
		// clusters sharing whatever height is left below it - same buttons, just stacked
		// instead of flanking the LCD. cell isn't capped to a fixed fraction of the width
		// here the way the landscape branch's is, since there's no LCD in this same row
		// competing for it - only "whatever's left after the transport column" matters.
		lcdBounds = area.removeFromTop(area.getHeight() * 0.42f).reduced(4.0f, 0.0f);
		area.removeFromTop(8.0f);
		const float transportColW = juce::jmin(area.getWidth() * 0.30f, area.getHeight() * 1.2f);
		const float cell = juce::jmin((area.getWidth() - transportColW) / 3.0f, area.getHeight() / 2.0f);
		auto controlRow = area.withSizeKeepingCentre(area.getWidth(), cell * 2.0f);
		layoutTransportAndDpad(controlRow, cell, transportColW);
	}
}

void D110SequencerRetroPanel::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);
	paintLcd(g, lcdBounds);
	paintButtons(g);
}

void D110SequencerRetroPanel::paintButtons(juce::Graphics &g) {
	auto &eng = engine();
	paintRetroButton(g, stopBounds, "STOP", !eng.isPlaying());
	paintRetroButton(g, playBounds, "PLAY", eng.isPlaying() && !eng.isRecording());
	paintRetroButton(g, recBounds, "REC", eng.isRecording());
	paintRetroButton(g, upBounds, juce::String::fromUTF8("\xE2\x96\xB2"), false);   // ▲
	paintRetroButton(g, downBounds, juce::String::fromUTF8("\xE2\x96\xBC"), false); // ▼
	paintRetroButton(g, leftBounds, juce::String::fromUTF8("\xE2\x97\x80"), false); // ◀
	paintRetroButton(g, rightBounds, juce::String::fromUTF8("\xE2\x96\xB6"), false); // ▶
	// "ENTER" doesn't fit this button now that it's a single square D-pad cell - shortened to
	// match BACK's own 4-character width.
	paintRetroButton(g, enterBounds, "ENT", false);
	paintRetroButton(g, exitBounds, "BACK", false); // exitBounds is the BACK button - see the .h
}

void D110SequencerRetroPanel::paintLcd(juce::Graphics &g, juce::Rectangle<float> area) {
	// Fixed 20x4 character grid, like a real alphanumeric LCD. charPx (one glyph's dot-matrix
	// height) used to be picked independently per axis - a width-based candidate exactly
	// calibrated to fill `area`, and a height-based one with an arbitrary "0.85" fudge factor
	// - and whichever was smaller won, with the GLASS ITSELF staying at the full, larger
	// `area` regardless. Alan's complaint, 2026-08-23: whenever the height-based candidate
	// won (the usual case, since this drawer is wide and short), the actual 20x4 text ended
	// up confined to one corner of a much wider green rectangle - not a real 4x20 display's
	// proportions at all. Fixed by computing the exact pixel size the grid produces and
	// cropping `area` down to precisely that, centred in whatever space was allotted, before
	// any painting happens - the glass itself is now a true 4x20 shape, not a letterboxed
	// container with dead space down one side.
	constexpr float kCols = 20.0f;
	// 4 rows (title + 3 body) by default; OPTIONS > LCD LINES (lcdCompactMode) switches to 2
	// (title + 1 body) for roughly double the character size - see bodyRows().
	const float kRows = lcdCompactMode ? 2.0f : 4.0f;
	// One character's width in units of charPx: drawDotGlyphs() advances 6 dots (5 columns +
	// a 1-dot gap) per glyph over a 7-dot-tall cell, so 6/7 of charPx. kRowSpacing is a small
	// deliberate step up from 1.0 (one glyph-height per row) so adjacent rows don't touch.
	constexpr float kCharAdvance = 6.0f / 7.0f, kRowSpacing = 1.15f;
	auto innerGuess = area.reduced(5.0f, 3.0f);
	if (innerGuess.getWidth() <= 0.0f || innerGuess.getHeight() <= 0.0f) return;
	const float charPx = juce::jmin(innerGuess.getWidth() / (kCols * kCharAdvance),
	                                 innerGuess.getHeight() / (kRows * kRowSpacing));
	area = area.withSizeKeepingCentre(kCols * kCharAdvance * charPx + 10.0f, kRows * kRowSpacing * charPx + 6.0f);

	g.setColour(kLcdGlass);
	g.fillRoundedRectangle(area, 4.0f);
	g.setColour(kLcdInk);
	g.drawRoundedRectangle(area, 4.0f, 1.5f);

	auto inner = area.reduced(5.0f, 3.0f);
	if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f) return;
	const float rowH = inner.getHeight() / kRows;

	// STEP RECORDING overlay takes over the whole glass (no title/menu underneath makes
	// sense while it's active) - only ever shown on HOME, since that's the only screen
	// still visible once a track is armed for step recording.
	if (stack.empty() && engine().isStepRecording()) {
		auto &eng = engine();
		const int stepIndex = eng.getStepIndexInBar();
		const int stepsPerBar = eng.getStepsPerBar();
		g.setColour(kLcdInk);
		// BAR/DUR each get a ">"/" " cursor marker, same convention as every List/Form
		// row elsewhere - see stepOverlayCursor's own comment.
		const juce::String barText = (stepOverlayCursor == 0 ? juce::String(">") : juce::String(" ")) + "BAR "
		    + juce::String(eng.getStepBar()) + " STEP " + juce::String(stepIndex) + "/" + juce::String(stepsPerBar);
		const juce::String durText = (stepOverlayCursor == 1 ? juce::String(">") : juce::String(" ")) + "DUR "
		    + stepDurationShortLabel(eng.getStepDuration()) + (eng.getStepDotted() ? " DOT" : "");
		if (lcdCompactMode) {
			// Same two rows as the 4-line version below, just without the static
			// header/footer - no room for those once LCD LINES halves the row count (see
			// bodyRows()'s own comment).
			const float lineH = inner.getHeight() / 2.0f;
			auto l0 = inner.removeFromTop(lineH);
			auto l1 = inner.removeFromTop(lineH);
			drawDotText(g, barText, l0, juce::Justification::centredLeft, charPx);
			drawDotText(g, durText, l1, juce::Justification::centredLeft, charPx);
		} else {
			const float lineH = inner.getHeight() / 4.0f;
			auto l0 = inner.removeFromTop(lineH);
			auto l1 = inner.removeFromTop(lineH);
			auto l2 = inner.removeFromTop(lineH);
			auto l3 = inner.removeFromTop(lineH);
			drawDotText(g, "STEP RECORDING", l0, juce::Justification::centredLeft, charPx);
			drawDotText(g, barText, l1, juce::Justification::centredLeft, charPx);
			drawDotText(g, durText, l2, juce::Justification::centredLeft, charPx);
			drawDotText(g, juce::String(juce::jmax(0, stepsPerBar - stepIndex)) + " STEPS LEFT", l3,
			            juce::Justification::centredLeft, charPx);
		}
		return;
	}

	auto titleArea = inner.removeFromTop(rowH);
	g.setColour(kLcdInk);
	drawDotText(g, stack.empty() ? homeStatusText() : top().title, titleArea, juce::Justification::centredLeft, charPx);

	switch (top().kind) {
		case ScreenKind::list: paintListScreen(g, inner, charPx); break;
		case ScreenKind::form: paintFormScreen(g, inner, charPx); break;
		case ScreenKind::confirm: paintConfirmScreen(g, inner, charPx); break;
		case ScreenKind::nameEdit: paintNameEditScreen(g, inner, charPx); break;
	}
}

juce::String D110SequencerRetroPanel::homeStatusText() {
	auto &eng = engine();
	const juce::String transport = eng.isRecording() ? "REC" : eng.isPlaying() ? "PLAY" : "STOP";
	return transport + " BAR " + juce::String(eng.getCurrentBar()) + "/" + juce::String(eng.getBarCount());
}

void D110SequencerRetroPanel::paintListScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	auto &s = top();
	auto items = s.buildItems();
	const int rows = bodyRows();
	const float lineH = textArea.getHeight() / (float) rows;

	int scroll = 0;
	if (!items.empty()) {
		s.cursor = juce::jlimit(0, (int) items.size() - 1, s.cursor);
		const int maxScroll = juce::jmax(0, (int) items.size() - rows);
		// (rows - 1) / 2 keeps the cursor row centred in the window when there's room either
		// side of it (rows == 3: cursor - 1, same as before this was generalised); in
		// compact mode (rows == 1) it collapses to cursor - 0, i.e. show exactly the
		// selected row, nothing else.
		scroll = juce::jlimit(0, maxScroll, s.cursor - (rows - 1) / 2);
	}

	if ((int) s.quickIndex.size() < (int) items.size()) s.quickIndex.resize(items.size(), 0);

	for (int row = 0; row < rows; ++row) {
		auto lineArea = textArea.removeFromTop(lineH);
		const int idx = scroll + row;
		if (idx < 0 || idx >= (int) items.size()) continue;
		const auto &it = items[(size_t) idx];
		const bool selected = idx == s.cursor;
		g.setColour(it.enabled ? kLcdInk : kLcdInk.withAlpha(0.35f));
		// A quick-bar row shows whichever action is currently dialled (LEFT/RIGHT-cycled,
		// see pressLeft/pressRight) instead of a plain value - same column, same layout.
		juce::String valueText = it.value;
		if (!it.quickActions.empty()) {
			const int qi = juce::jlimit(0, (int) it.quickActions.size() - 1, s.quickIndex[(size_t) idx]);
			valueText = (selected ? juce::String("<") : juce::String()) + it.quickActions[(size_t) qi].label
			            + (selected ? juce::String(">") : juce::String());
		}
		// The label/value split used to be a fixed 62/38 - fine for a short value like
		// REC/PLAY, but it clipped an empty-value row's label (TEMPO/SIG/METRO,
		// PRECOUNT/LOOP...) for no reason once drawDotText started clipping to its own
		// column (Alan's report, 2026-08-23: "c'est coupe n'importe comment"). Size the
		// label off the value that's actually there instead: a row with nothing on the
		// right gets (almost) the whole line, one with a real value only gives up exactly
		// the room that value needs.
		const float valueW = valueText.isEmpty() ? 0.0f : dotTextWidth(valueText, charPx) + charPx * 0.9f;
		const float labelW = juce::jmax(lineArea.getWidth() * 0.35f, lineArea.getWidth() - valueW);
		auto labelArea = lineArea.removeFromLeft(labelW);
		drawDotText(g, (selected ? juce::String(">") : juce::String(" ")) + it.label, labelArea,
		            juce::Justification::centredLeft, charPx);
		drawDotText(g, valueText, lineArea, juce::Justification::centredRight, charPx);
	}
}

void D110SequencerRetroPanel::paintFormScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	auto &s = top();
	if (!s.fields.empty()) s.cursor = juce::jlimit(0, (int) s.fields.size() - 1, s.cursor);
	const int rows = bodyRows();

	// Compact mode (rows == 1) has no room for the normal 2-fields-per-line static grid
	// below - every field still has to be reachable, so show just the one under the cursor
	// instead, the same "only the selected row is visible" idea paintListScreen uses.
	if (rows == 1) {
		if (!s.fields.empty()) {
			const auto &f = s.fields[(size_t) s.cursor];
			g.setColour(kLcdInk);
			drawDotText(g, ">" + f.label + " " + f.format(*f.value), textArea, juce::Justification::centredLeft, charPx);
		}
		return;
	}

	// verticalFields: one field per line instead of the two-per-line grid below, scrolling
	// like paintListScreen's own rows do when there are more fields than bodyRows() - see
	// Screen::verticalFields's own comment for why (a long label like "PRG(0=OFF)" packed
	// two to a line left no room for the value itself to actually show). Reuses the same
	// width-aware label/value split paintListScreen uses, for the same reason: a value-less
	// field would otherwise give up half the line for nothing.
	if (s.verticalFields) {
		const float lineH = textArea.getHeight() / (float) rows;
		int scroll = 0;
		if (!s.fields.empty()) {
			const int maxScroll = juce::jmax(0, (int) s.fields.size() - rows);
			scroll = juce::jlimit(0, maxScroll, s.cursor - (rows - 1) / 2);
		}
		for (int row = 0; row < rows; ++row) {
			auto lineArea = textArea.removeFromTop(lineH);
			const int idx = scroll + row;
			if (idx < 0 || idx >= (int) s.fields.size()) continue;
			const auto &f = s.fields[(size_t) idx];
			const bool selected = idx == s.cursor;
			const juce::String valueText = f.format(*f.value);
			g.setColour(kLcdInk);
			const float valueW = valueText.isEmpty() ? 0.0f : dotTextWidth(valueText, charPx) + charPx * 0.9f;
			const float labelW = juce::jmax(lineArea.getWidth() * 0.35f, lineArea.getWidth() - valueW);
			auto labelArea = lineArea.removeFromLeft(labelW);
			drawDotText(g, (selected ? juce::String(">") : juce::String(" ")) + f.label, labelArea,
			            juce::Justification::centredLeft, charPx);
			drawDotText(g, valueText, lineArea, juce::Justification::centredRight, charPx);
		}
		return;
	}

	const float lineH = textArea.getHeight() / (float) rows;

	for (int line = 0; line < rows; ++line) {
		auto lineArea = textArea.removeFromTop(lineH);
		for (int col = 0; col < 2; ++col) {
			const int idx = line * 2 + col;
			auto colArea = col == 0 ? lineArea.removeFromLeft(lineArea.getWidth() * 0.5f) : lineArea;
			if (idx >= (int) s.fields.size()) continue;
			const auto &f = s.fields[(size_t) idx];
			const bool selected = idx == s.cursor;
			const juce::String text = f.label + " " + f.format(*f.value);
			g.setColour(kLcdInk);
			drawDotText(g, (selected ? juce::String(">") : juce::String(" ")) + text, colArea,
			            juce::Justification::centredLeft, charPx);
		}
	}
}

void D110SequencerRetroPanel::paintConfirmScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	auto &s = top();
	const int rows = bodyRows();
	auto choiceArea = textArea;
	// Compact mode (rows == 1) has no spare row for the message body - the screen's own
	// title (drawn separately, above textArea) already states the question, so just the
	// YES/NO choice is shown here. rows > 1 keeps the original 2-lines-message/1-line-choice
	// split, generalised to however many rows are actually available.
	if (rows > 1) {
		const float lineH = textArea.getHeight() / (float) rows;
		auto msgArea = textArea.removeFromTop(lineH * (float) (rows - 1));
		choiceArea = textArea;
		g.setColour(kLcdInk);
		drawDotFitted(g, s.message, msgArea, juce::Justification::centredLeft, rows - 1, charPx);
	}

	auto noArea = choiceArea.removeFromLeft(choiceArea.getWidth() * 0.5f);
	auto yesArea = choiceArea;
	g.setColour(!s.confirmYes ? kLcdInk : kLcdInk.withAlpha(0.5f));
	drawDotText(g, !s.confirmYes ? ">NO" : " NO", noArea, juce::Justification::centredLeft, charPx);
	g.setColour(s.confirmYes ? kLcdInk : kLcdInk.withAlpha(0.5f));
	drawDotText(g, s.confirmYes ? ">YES" : " YES", yesArea, juce::Justification::centredLeft, charPx);
}

void D110SequencerRetroPanel::paintNameEditScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	const int rows = bodyRows();
	// rows > 1: 2 of the available rows, for a clearer caret box (unchanged from before this
	// was generalised). Compact mode (rows == 1) has only the one row to give it.
	auto nameArea = rows > 1 ? textArea.removeFromTop(textArea.getHeight() / (float) rows * (float) (rows - 1))
	                          : textArea;

	const float charW = nameArea.getWidth() / float(kNameEditLength);
	for (int i = 0; i < kNameEditLength && i < nameEditBuffer.length(); ++i) {
		auto cell = nameArea.removeFromLeft(charW);
		const bool caret = i == nameEditCaret;
		if (caret) {
			g.setColour(kLcdInk);
			g.fillRect(cell.reduced(1.0f));
			g.setColour(kLcdGlass);
		} else {
			g.setColour(kLcdInk);
		}
		drawDotText(g, juce::String::charToString(nameEditBuffer[i]), cell, juce::Justification::centred, charPx);
	}
}
