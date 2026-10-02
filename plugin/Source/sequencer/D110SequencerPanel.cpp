#include "D110SequencerPanel.h"
#include "SequencerTransportMenu.h"

#include <cmath>

#include "../AppModeIcons.h"
#include "../UiTheme.h"

using d110seq::D110SequencerEngine;

namespace {
constexpr int kNumTracks = D110SequencerEngine::kNumTracks;
constexpr int kMaxTracks = D110SequencerEngine::kMaxTracks;

// "PART N" for the 8 D-110 parts, "RHYTHM" for track 8, "TRACK N" for the 7 extra tracks (9-15,
// Nonet Sequencer only) - shared by paint() and every dialog/menu below that names a track.
juce::String defaultTrackLabel(int t) {
	if (t == D110SequencerEngine::kRhythmTrack) return "RHYTHM";
	if (t < kNumTracks) return "PART " + juce::String(t + 1);
	return "TRACK " + juce::String(t + 1);
}

// Inverse of juce::MidiMessage::getMidiNoteName(note, true, true, 4) - the convention used
// everywhere in this file, where "C4" is MIDI note 60. Accepts "C4", "C#4", "Db3", "E5", case-
// insensitive, octave may be negative ("C-1" = note 0). Returns -1 if unparseable or out of range,
// so the note-edit dialog can take a human note name instead of a meaningless raw MIDI number.
int noteNameToNumber(const juce::String &nameIn) {
	auto name = nameIn.trim().toUpperCase();
	if (name.isEmpty()) return -1;
	static const int pitchClassForLetter[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
	const int letterIndex = juce::String("ABCDEFG").indexOfChar(name[0]);
	if (letterIndex < 0) return -1;
	int pitchClass = pitchClassForLetter[letterIndex];
	int pos = 1;
	if (pos < name.length() && name[pos] == '#') { pitchClass += 1; ++pos; }
	else if (pos < name.length() && name[pos] == 'B') { pitchClass -= 1; ++pos; }
	auto octaveStr = name.substring(pos);
	if (octaveStr.isEmpty() || !octaveStr.containsOnly("-0123456789")) return -1;
	const int octave = octaveStr.getIntValue();
	const int note = 12 * (octave + 1) + pitchClass;
	return (note >= 0 && note <= 127) ? note : -1;
}

// enabled=false dims the button (used only by UNDO, greyed out while its stack is empty) -
// distinct from active, which picks the on/off colour pair rather than an alpha.
// Upper bound for the label font. 13 on a desktop-sized layout; the Android portrait layout
// (D110SequencerPanel::stackedTransport) raises it for its taller rows via paint().
float gToggleFontMax = 13.0f;

void paintToggleButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active,
                        bool enabled = true) {
	const auto &pal = d110ui::palette();
	auto fill = active ? pal.seqActiveFill : pal.seqInactiveFill;
	auto text = active ? pal.seqActiveText : pal.seqInactiveText;
	if (!enabled) {
		fill = fill.withAlpha(0.35f);
		text = text.withAlpha(0.35f);
	}
	g.setColour(fill);
	g.fillRect(b.reduced(2.0f));
	g.setColour(text);
	g.setFont(juce::FontOptions(juce::jlimit(8.0f, gToggleFontMax, b.getHeight() * 0.5f)));
	g.drawText(label, b, juce::Justification::centred);
}

// The ARM button: a plain filled box like every other button here, but with a small
// record-style dot instead of a text label - filled red while armed, a dim hollow ring
// otherwise, the same convention most DAWs use for a track's record-enable.
void paintArmButton(juce::Graphics &g, juce::Rectangle<float> b, bool armed) {
	const auto &pal = d110ui::palette();
	g.setColour(pal.seqInactiveFill);
	g.fillRect(b.reduced(2.0f));
	const float d = juce::jmin(b.getWidth(), b.getHeight()) * 0.4f;
	const juce::Rectangle<float> dot(b.getCentreX() - d * 0.5f, b.getCentreY() - d * 0.5f, d, d);
	if (armed) {
		g.setColour(pal.seqArmDot);
		g.fillEllipse(dot);
	} else {
		g.setColour(pal.seqInactiveText.withAlpha(0.6f));
		g.drawEllipse(dot, 1.5f);
	}
}

struct TimeSig {
	int num, den;
};
// Shared by the left-click cycle and the right-click pick-from-list menu.
const std::array<TimeSig, 6> &timeSigPresets() {
	static const std::array<TimeSig, 6> presets{{{4, 4}, {3, 4}, {6, 8}, {2, 4}, {5, 4}, {7, 8}}};
	return presets;
}

juce::String recordModeLabel(d110seq::RecordMode mode) {
	switch (mode) {
		case d110seq::RecordMode::overdub: return "REC: OVERDUB";
		case d110seq::RecordMode::replaceRange: return "REC: REPLACE";
		case d110seq::RecordMode::replaceToEnd: return "REC: REPLACE+END";
	}
	return {};
}

juce::String loopModeLabel(d110seq::LoopMode mode) {
	switch (mode) {
		case d110seq::LoopMode::off: return "LOOP OFF";
		case d110seq::LoopMode::bar: return "LOOP: BAR";
		case d110seq::LoopMode::punch: return "LOOP: PUNCH";
	}
	return {};
}

// QuantizeGrid::off is not a valid step duration (see D110SequencerEngine::setStepDuration()),
// so it's left out of both this list and its label below. Largest to smallest, whole note first -
// the dotted modifier (see the DOT button) covers the in-between durations (dotted half = 3
// beats, ...) without needing a dotted entry for every base grid here.
const std::array<d110seq::QuantizeGrid, 8> &stepGridPresets() {
	using d110seq::QuantizeGrid;
	static const std::array<QuantizeGrid, 8> presets{
		{QuantizeGrid::whole, QuantizeGrid::half, QuantizeGrid::quarter, QuantizeGrid::eighth,
	     QuantizeGrid::sixteenth, QuantizeGrid::eighthTriplet, QuantizeGrid::sixteenthTriplet,
	     QuantizeGrid::thirtySecond}};
	return presets;
}

juce::String stepDurationLabel(d110seq::QuantizeGrid grid) {
	using d110seq::QuantizeGrid;
	switch (grid) {
		case QuantizeGrid::whole: return "STEP 1/1";
		case QuantizeGrid::half: return "STEP 1/2";
		case QuantizeGrid::quarter: return "STEP 1/4";
		case QuantizeGrid::eighth: return "STEP 1/8";
		case QuantizeGrid::sixteenth: return "STEP 1/16";
		case QuantizeGrid::eighthTriplet: return "STEP 1/8 T";
		case QuantizeGrid::sixteenthTriplet: return "STEP 1/16 T";
		case QuantizeGrid::thirtySecond: return "STEP 1/32";
		case QuantizeGrid::off: default: return "STEP 1/4";
	}
}

// "Vel"/"Dur" spelled out (not "v"/a trailing "b") so the columns read on their own without
// a separate header row - Alan couldn't tell what "v108"/"0.06b" meant at a glance.
juce::String formatEventRow(const d110seq::D110SequencerEngine::NoteEventInfo &ev) {
	return "Beat " + juce::String(ev.beatInBar + 1.0, 2) + "   "
		+ juce::MidiMessage::getMidiNoteName(ev.note, true, true, 4) + "   Vel " + juce::String(ev.velocity)
		+ "   Dur " + juce::String(ev.durationBeats, 2) + "b";
}

// promptForEventList()'s own list view: one row per note in the bar, each with a small
// delete ("X") button - "pas un piano roll", just a plain scrollable list, since the point is
// picking out a specific (usually wrong) note, not drawing/moving pitch or timing by hand.
// Clicking anywhere else on a row prompts for a new pitch (see onEditPitch) - the note NAME
// itself is the click target, not a separate icon, since the row's too narrow to spare one.
// Lives inside a juce::Viewport (see promptForEventList()) so a busy bar (the rhythm track
// especially) scrolls rather than growing the dialog without bound.
class NoteEventListContent : public juce::Component {
public:
	struct Row {
		int index; // D110SequencerEngine::NoteEventInfo::index - passed back to onDelete/onEditPitch
		int note;  // current pitch - passed to onEditPitch so its prompt can default to it
		juce::String text;
	};

	std::function<void(int index)> onDelete;
	std::function<void(int index, int currentNote)> onEditPitch;

	void setRows(std::vector<Row> newRows) {
		rows = std::move(newRows);
		setSize(getWidth() > 0 ? getWidth() : 380, juce::jmax(kRowH, int(rows.size()) * kRowH));
		repaint();
	}

	void paint(juce::Graphics &g) override {
		const auto &pal = d110ui::palette();
		// Own opaque background, filled explicitly rather than left to whatever's behind this
		// component (the AlertWindow it lives in doesn't itself follow d110ui's light/dark
		// theme) - otherwise seqInactiveText below can land on a background it was never paired
		// with (e.g. this component's dark-theme text on the AlertWindow's own fixed-dark
		// background looked fine by accident, but the light-theme pairing didn't - see paint()'s
		// own text colour, which is chosen to always be readable against THIS fill).
		g.fillAll(pal.box);
		if (rows.empty()) {
			g.setColour(pal.seqInactiveText);
			g.drawText("(no notes in this bar)", getLocalBounds().toFloat(), juce::Justification::centred);
			return;
		}
		for (size_t i = 0; i < rows.size(); ++i) {
			auto b = juce::Rectangle<float>(0.0f, float(i) * kRowH, float(getWidth()), float(kRowH));
			if (i % 2 == 1) {
				g.setColour(pal.seqInactiveFill.withAlpha(0.25f));
				g.fillRect(b);
			}
			auto textArea = b;
			auto del = textArea.removeFromRight(kRowH).reduced(6.0f);
			// seqInactiveText, not seqActiveText: the latter is meant for text ON TOP of the
			// green seqActiveFill (an active toggle button), not plain readout text - using it
			// here was the actual cause of the low-contrast "black on dark grey" text.
			g.setColour(pal.seqInactiveText);
			g.setFont(juce::FontOptions(13.0f));
			g.drawText(rows[i].text, textArea.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
			// A plain X, in the same red as the ARM dot - reads as destructive without needing
			// a whole icon set for one button.
			g.setColour(pal.seqArmDot);
			g.drawLine(del.getX(), del.getY(), del.getRight(), del.getBottom(), 1.5f);
			g.drawLine(del.getX(), del.getBottom(), del.getRight(), del.getY(), 1.5f);
		}
	}

	void mouseDown(const juce::MouseEvent &e) override {
		const int row = int(e.position.y / kRowH);
		if (row < 0 || row >= int(rows.size())) return;
		auto b = juce::Rectangle<float>(0.0f, float(row) * kRowH, float(getWidth()), float(kRowH));
		const auto &r = rows[static_cast<size_t>(row)];
		if (b.removeFromRight(kRowH).contains(e.position)) { if (onDelete) onDelete(r.index); return; }
		if (onEditPitch) onEditPitch(r.index, r.note);
	}

private:
	static constexpr int kRowH = 24;
	std::vector<Row> rows;
};

// promptForEventList()'s own bar-nav strip - "< BAR N >", added as its own custom component
// ABOVE the scrollable list (not inside the Viewport), so it stays put rather than scrolling
// away, and switching bars never has to close and reopen the whole dialog. "<" dims rather
// than disappearing below bar 1, matching paintToggleButton's enabled/disabled convention
// elsewhere in this panel; ">" has no upper bound, same as gotoBar() itself.
class NoteEventListHeader : public juce::Component {
public:
	std::function<void()> onPrev, onNext;

	void setBar(int newBar) {
		bar = newBar;
		repaint();
	}

	void paint(juce::Graphics &g) override {
		const auto &pal = d110ui::palette();
		g.fillAll(pal.box); // see NoteEventListContent::paint()'s own comment on why
		auto b = getLocalBounds().toFloat();
		prevBounds = b.removeFromLeft(28.0f);
		nextBounds = b.removeFromRight(28.0f);
		g.setFont(juce::FontOptions(15.0f));
		// Each arrow's own enabled state, not one colour shared by both - drawing them together
		// meant "<" being dimmed on bar 1 also dimmed ">" right along with it, which read as
		// though ">" (never actually bounded - any bar can be navigated to) had disappeared.
		g.setColour(bar > 1 ? pal.seqInactiveText : pal.seqInactiveText.withAlpha(0.35f));
		g.drawText("<", prevBounds, juce::Justification::centred);
		g.setColour(pal.seqInactiveText);
		g.drawText(">", nextBounds, juce::Justification::centred);
		g.setFont(juce::FontOptions(13.0f));
		g.drawText("Bar " + juce::String(bar), b, juce::Justification::centred);
	}

	void mouseDown(const juce::MouseEvent &e) override {
		if (prevBounds.contains(e.position)) { if (bar > 1 && onPrev) onPrev(); return; }
		if (nextBounds.contains(e.position)) { if (onNext) onNext(); return; }
	}

private:
	int bar = 1;
	juce::Rectangle<float> prevBounds, nextBounds;
};
} // namespace

D110SequencerPanel::D110SequencerPanel(D110SequencerHost &p) : processor(p) { startTimerHz(15); }

D110SequencerPanel::~D110SequencerPanel() { stopTimer(); }

d110seq::D110SequencerEngine &D110SequencerPanel::engine() { return processor.getSequencer(); }

void D110SequencerPanel::timerCallback() {
	auto &eng = engine();
	if (eng.isPrecounting()) {
		// Edge-detect a new precount beat (positionBeats itself is frozen throughout precount,
		// so it can't drive this the way the normal scrolling LED does) and light the downbeat
		// LED for a short, fixed window - a flash on every beat of the count-in, audio or not,
		// per Alan's own ask (2026-08-07): something to follow along with even without sound.
		const int elapsed = eng.precountBeatsElapsed();
		if (elapsed != lastPrecountBeatsElapsed) {
			lastPrecountBeatsElapsed = elapsed;
			constexpr int kFlashMs = 120;
			precountFlashUntilMs = juce::Time::getMillisecondCounter() + kFlashMs;
		}
	} else {
		lastPrecountBeatsElapsed = -1;
	}
	// The only other things that change on their own, without a click on this panel, are the bar
	// readout (and the transport buttons' own on/off look) while the transport is rolling, and
	// the step-recording readout, which advances from notes played on an external MIDI
	// controller or the on-screen keyboard drawer - neither of which is a click on THIS panel,
	// so nothing else would ever trigger the repaint that shows it.
	if (eng.isPlaying() || eng.isStepRecording()) repaint();
}

void D110SequencerPanel::cycleTimeSignature() {
	const auto &presets = timeSigPresets();
	auto &eng = engine();
	size_t idx = 0;
	for (size_t i = 0; i < presets.size(); ++i)
		if (presets[i].num == eng.getTimeSigNumerator() && presets[i].den == eng.getTimeSigDenominator()) {
			idx = i;
			break;
		}
	idx = (idx + 1) % presets.size();
	eng.setTimeSignature(presets[idx].num, presets[idx].den);
}

// Right-click: pick any of the presets directly instead of stepping through them one at a
// time - handy once you're past the first couple of clicks of cycleTimeSignature() - plus
// a "Custom..." entry (promptForTimeSignature()) for anything the six presets don't cover.
void D110SequencerPanel::showTimeSignatureMenu() {
	const auto &presets = timeSigPresets();
	auto &eng = engine();

	juce::PopupMenu m;
	for (size_t i = 0; i < presets.size(); ++i) {
		const bool current =
			presets[i].num == eng.getTimeSigNumerator() && presets[i].den == eng.getTimeSigDenominator();
		m.addItem(int(i) + 1, juce::String(presets[i].num) + "/" + juce::String(presets[i].den), true, current);
	}
	m.addSeparator();
	const int customItemId = int(presets.size()) + 1;
	m.addItem(customItemId, "Custom...");

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
	                [this, customItemId](int result) {
		if (result == customItemId) { promptForTimeSignature(); return; }
		const auto &presets = timeSigPresets();
		if (result < 1 || result > int(presets.size())) return;
		engine().setTimeSignature(presets[size_t(result - 1)].num, presets[size_t(result - 1)].den);
		repaint();
	});
}

// "Custom..." entry on showTimeSignatureMenu() - the engine itself already accepts any
// numerator/denominator in [1,32] (D110SequencerEngine::setTimeSignature() clamps to
// that), the six presets above are purely a UI convenience for the common cases.
void D110SequencerPanel::promptForTimeSignature() {
	auto &eng = engine();
	auto *aw = new juce::AlertWindow("Time signature", "Enter a custom time signature (1-32 over 1-32).",
	                                  juce::AlertWindow::NoIcon);
	aw->addTextEditor("num", juce::String(eng.getTimeSigNumerator()), "Numerator:");
	aw->addTextEditor("den", juce::String(eng.getTimeSigDenominator()), "Denominator:");
	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			const int num = aw->getTextEditorContents("num").getIntValue();
			const int den = aw->getTextEditorContents("den").getIntValue();
			if (num >= 1 && den >= 1) engine().setTimeSignature(num, den);
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::cycleStepDuration() {
	const auto &presets = stepGridPresets();
	auto &eng = engine();
	size_t idx = 0;
	for (size_t i = 0; i < presets.size(); ++i)
		if (presets[i] == eng.getStepDuration()) {
			idx = i;
			break;
		}
	idx = (idx + 1) % presets.size();
	eng.setStepDuration(presets[idx]);
}

// Right-click the step-duration readout: pick any grid directly, same convention as
// showTimeSignatureMenu().
void D110SequencerPanel::showStepDurationMenu() {
	const auto &presets = stepGridPresets();
	auto &eng = engine();

	juce::PopupMenu m;
	for (size_t i = 0; i < presets.size(); ++i)
		m.addItem(int(i) + 1, stepDurationLabel(presets[i]), true, presets[i] == eng.getStepDuration());

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this](int result) {
		const auto &presets = stepGridPresets();
		if (result < 1 || result > int(presets.size())) return;
		engine().setStepDuration(presets[size_t(result - 1)]);
		repaint();
	});
}

void D110SequencerPanel::cycleRecordMode() {
	using d110seq::RecordMode;
	auto &eng = engine();
	switch (eng.getRecordMode()) {
		case RecordMode::overdub: eng.setRecordMode(RecordMode::replaceRange); break;
		case RecordMode::replaceRange: eng.setRecordMode(RecordMode::replaceToEnd); break;
		case RecordMode::replaceToEnd: eng.setRecordMode(RecordMode::overdub); break;
	}
}

void D110SequencerPanel::showRecordModeMenu() {
	using d110seq::RecordMode;
	using d110seq::QuantizeMode;
	auto &eng = engine();
	const auto current = eng.getRecordMode();
	const auto qMode = eng.getQuantizeMode();

	juce::PopupMenu m;
	m.addItem(1, "Overdub - adds to what's already there", true, current == RecordMode::overdub);
	m.addItem(2, "Replace - erases only the punched span", true, current == RecordMode::replaceRange);
	m.addItem(3, "Replace to end - erases from the punch-in point onward", true,
	          current == RecordMode::replaceToEnd);
	// Same engine-wide setting as the outer app Options dialog's own "Quantize mode" row
	// (NonetSeqMain.cpp/PluginEditor.cpp) - Alan's request, 2026-08-23: reachable from inside
	// the sequencer itself too, without backing out to that separate dialog. Grouped onto
	// this particular popup (rather than a new on-screen control of its own) since it's
	// already the sequencer's one existing "how recording/quantizing behaves" menu.
	m.addSeparator();
	m.addItem(4, "Quantize mode: HARD - moves recorded notes onto the grid for good", true,
	          qMode == QuantizeMode::hard);
	m.addItem(5, "Quantize mode: SOFT - leaves the recording as played, snaps it live instead", true,
	          qMode == QuantizeMode::soft);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this](int result) {
		using d110seq::RecordMode;
		using d110seq::QuantizeMode;
		switch (result) {
			case 1: engine().setRecordMode(RecordMode::overdub); break;
			case 2: engine().setRecordMode(RecordMode::replaceRange); break;
			case 3: engine().setRecordMode(RecordMode::replaceToEnd); break;
			case 4: engine().setQuantizeMode(QuantizeMode::hard); break;
			case 5: engine().setQuantizeMode(QuantizeMode::soft); break;
			default: return;
		}
		repaint();
	});
}

void D110SequencerPanel::showUndoRedoInfo(bool isUndo) {
	auto &eng = engine();
	const juce::String desc = isUndo ? eng.getUndoDescription() : eng.getRedoDescription();
	juce::PopupMenu m;
	m.addItem(1, desc.isNotEmpty() ? desc : juce::String(isUndo ? "Nothing to undo" : "Nothing to redo"), false);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void D110SequencerPanel::showResyncInfo() {
	const juce::String fields = "Program Change/Bank" + juce::String(processor.supportsTrackVolumePan() ? "/Volume/Pan" : "");
	juce::PopupMenu m;
	m.addItem(1, "Send: re-sends every track's " + fields + " to the live patch right now, in case it's "
	                 "drifted from what's stored here.",
	          false);
	if (processor.supportsCaptureLivePatch())
		m.addItem(2, "Capture: overwrites every track's stored " + fields + " with what the live patch "
		                 "actually has right now (confirms first).",
		          false);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

// Pull direction - see D110SequencerHost.h's resyncProgramChanges()/captureLivePatchIntoTracks()
// comments for how this is the reverse of what SYNC's own "send" action does. Destructive to
// whatever every track's stored Program Change/Bank/Volume/Pan currently is, hence the confirm.
void D110SequencerPanel::confirmCaptureLivePatch() {
	auto *aw = new juce::AlertWindow(
		"Capture patch into song",
		"Overwrites every track's stored Program Change/Bank/Volume/Pan with what the live "
		"patch actually has right now, part by part. This can't be undone.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Capture", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			processor.captureLivePatchIntoTracks();
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::showMetronomeModeMenu() {
	using d110seq::MetronomeMode;
	auto &eng = engine();
	const auto current = eng.getMetronomeMode();

	juce::PopupMenu m;
	m.addItem(1, "Visual only - LED strip, no click", true, current == MetronomeMode::visualOnly);
	m.addItem(2, "Audio only - click, no LED strip", true, current == MetronomeMode::audioOnly);
	m.addItem(3, "Both", true, current == MetronomeMode::both);
	m.addSeparator();
	m.addItem(4, "Use rhythm channel (MIDI ch. 10) instead of the click sound", true,
	          eng.getMetronomeUseChannel10());
	m.addItem(5, "Only while recording, not during plain playback", true,
	          eng.getMetronomeRecordOnly());

	// Result codes 10-15 index into this same array on the way back out.
	static constexpr float kVolumePresets[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f };
	static constexpr const char *kVolumeLabels[] = { "25%", "50%", "75%", "100%", "125%", "150%" };
	const float currentVolume = eng.getMetronomeVolume();
	juce::PopupMenu volumeMenu;
	for (int i = 0; i < 6; ++i)
		volumeMenu.addItem(10 + i, kVolumeLabels[i], true,
		                    std::abs(currentVolume - kVolumePresets[i]) < 0.01f);
	m.addSubMenu("Volume", volumeMenu);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this](int result) {
		using d110seq::MetronomeMode;
		auto &eng = engine();
		if (result >= 10 && result < 16) {
			eng.setMetronomeVolume(kVolumePresets[result - 10]);
			repaint();
			return;
		}
		switch (result) {
			case 1: eng.setMetronomeMode(MetronomeMode::visualOnly); break;
			case 2: eng.setMetronomeMode(MetronomeMode::audioOnly); break;
			case 3: eng.setMetronomeMode(MetronomeMode::both); break;
			case 4: eng.setMetronomeUseChannel10(!eng.getMetronomeUseChannel10()); break;
			case 5: eng.setMetronomeRecordOnly(!eng.getMetronomeRecordOnly()); break;
			default: return;
		}
		repaint();
	});
}

void D110SequencerPanel::withLocalFileForLoad(const juce::URL &url, std::function<void(const juce::File &)> action) {
	// isLocalFile()/getLocalFile() on Android reconstruct a raw filesystem path from a
	// content:// SAF URI's document ID (e.g. "primary:Download/foo.mid" ->
	// "/storage/emulated/0/Download/foo.mid") without ever checking it's actually readable -
	// confirmed empirically, 2026-08-23 (Alan: sequencer LOAD silently did nothing): the
	// reconstructed path passes File::getSize() (a stat()) but FileInputStream fails to open
	// it under scoped storage, since this app never requested (and on modern Android mostly
	// can't get) raw filesystem access outside its own sandbox. A stat()-only check like
	// existsAsFile() doesn't catch this either, since stat() is exactly what still succeeds -
	// only an actual open attempt reveals it. Falling through to the AndroidDocument path
	// below (unconditionally correct, just slower) whenever that open fails costs nothing on
	// desktop, where isLocalFile() only ever reports genuine file:// URLs and this always
	// succeeds on the first try.
	if (url.isLocalFile()) {
		auto file = url.getLocalFile();
		juce::FileInputStream testStream(file);
		if (testStream.openedOk()) { action(file); return; }
	}
	auto document = juce::AndroidDocument::fromDocument(url);
	auto stream = document.hasValue() ? document.createInputStream() : nullptr;
	if (stream == nullptr) return;
	auto tempFile = juce::File::createTempFile("mid");
	{
		juce::FileOutputStream out(tempFile);
		if (out.openedOk()) {
			out.writeFromInputStream(*stream, -1);
			out.flush();
		}
	}
	action(tempFile);
	tempFile.deleteFile();
}

void D110SequencerPanel::withLocalFileForSave(const juce::URL &url, const juce::String &extension,
                                              std::function<void(const juce::File &)> action) {
	// Same reconstructed-path unreliability as withLocalFileForLoad's own comment - a save
	// target can hit it too (CREATE_DOCUMENT already creates the empty destination file
	// before returning its URI), so verify real write access the same way rather than
	// trusting isLocalFile() alone.
	if (url.isLocalFile()) {
		auto file = url.getLocalFile();
		if (!file.hasFileExtension(extension)) file = file.withFileExtension(extension);
		juce::FileOutputStream testStream(file);
		if (testStream.openedOk()) {
			action(file);
			return;
		}
	}
	auto tempFile = juce::File::createTempFile(extension);
	action(tempFile);
	auto document = juce::AndroidDocument::fromDocument(url);
	auto outStream = document.hasValue() ? document.createOutputStream() : nullptr;
	if (outStream != nullptr) {
		juce::FileInputStream in(tempFile);
		if (in.openedOk()) outStream->writeFromInputStream(in, -1);
		outStream->flush();
	}
	tempFile.deleteFile();
}

// Plain click loads/saves just the current song (.mid) - see mouseDown(). Right-click is the
// shortcut for all 4 slots at once (.midiseq), so there's exactly one action per gesture
// rather than a one-item menu restating what the plain click already does.
void D110SequencerPanel::showLoadMenu() {
	// Also accepts the old *.d110songs extension (same XML underneath, only the file's own
	// name changed - see D110SequencerSongsFile.h) so a file saved before the rename still
	// opens.
	auto *chooser =
		new juce::FileChooser("Load all 4 sequencer songs", processor.getLastDialogDir(), "*.midiseq;*.d110songs");
	chooser->launchAsync(
		juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
		[this, chooser](const juce::FileChooser &fc) {
			const auto url = fc.getURLResult();
			if (url != juce::URL())
				withLocalFileForLoad(url, [this](const juce::File &file) {
					processor.setLastDialogDir(file.getParentDirectory());
					processor.importSequencerSongs(file);
				});
			delete chooser;
			repaint();
		});
}

void D110SequencerPanel::showSaveMenu() {
	// Default filename dated rather than a bare "song.midiseq" so repeated saves during a
	// session don't collide/overwrite each other by default (Alan's request, 2026-08-20).
	const auto defaultFile =
		processor.getLastDialogDir().getChildFile(juce::Time::getCurrentTime().formatted("song-%Y-%m-%d.midiseq"));
	auto *chooser = new juce::FileChooser("Save all 4 sequencer songs", defaultFile, "*.midiseq");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			const auto url = fc.getURLResult();
			if (url != juce::URL())
				withLocalFileForSave(url, "midiseq", [this](const juce::File &file) {
					processor.setLastDialogDir(file.getParentDirectory());
					processor.exportSequencerSongs(file);
				});
			delete chooser;
		});
}

void D110SequencerPanel::showQuantizeMenu(int track) {
	using d110seq::QuantizeGrid;
	const auto current = engine().getTrackQuantize(track);

	juce::PopupMenu m;
	m.addItem(12, "Rename track...", true);
	// Same dialog as the CH readout's own "Program Change..." submenu item (see
	// showTrackChannelMenu()) - just a second way to reach it, since a right-click on the row
	// itself is a more discoverable spot for it than the narrow channel readout.
	// Rhythm has no Program Change equivalent but does have its own fixed Volume/Pan (2026-08-21,
	// Alan's request) - labelled "CC Change" there instead, since Program/Bank don't apply.
	const bool hasProgram = processor.supportsProgramChangeForTrack(track);
	const bool hasVolPanOnly = !hasProgram && processor.supportsTrackVolumePanForTrack(track);
	if (hasProgram) {
		const int program = processor.getTrackProgram(track);
		m.addItem(14,
		          program < 0 ? juce::String("Program Change: none...")
		                      : "Program change " + juce::String(program + 1) + " / bank "
		                            + juce::String(processor.getTrackBank(track)) + "...",
		          true);
	} else if (hasVolPanOnly) {
		const int volume = processor.getTrackVolume(track);
		const int pan = processor.getTrackPan(track);
		juce::StringArray parts;
		if (volume >= 0) parts.add("vol " + juce::String(volume));
		if (pan >= 0) parts.add("pan " + juce::String(pan));
		m.addItem(14, "CC Change: " + (parts.isEmpty() ? juce::String("none") : parts.joinIntoString(", ")) + "...",
		           true);
	}
	m.addSeparator();
	m.addItem(1, "Quantize: Off", true, current == QuantizeGrid::off);
	m.addItem(2, "Quantize: 1/4", true, current == QuantizeGrid::quarter);
	m.addItem(3, "Quantize: 1/8", true, current == QuantizeGrid::eighth);
	m.addItem(4, "Quantize: 1/16", true, current == QuantizeGrid::sixteenth);
	m.addItem(5, "Quantize: 1/8 triplet", true, current == QuantizeGrid::eighthTriplet);
	m.addItem(6, "Quantize: 1/16 triplet", true, current == QuantizeGrid::sixteenthTriplet);
	m.addItem(7, "Quantize: 1/32", true, current == QuantizeGrid::thirtySecond);
	m.addSeparator();
	m.addItem(8, "Clear track", engine().trackHasEvents(track));
	m.addItem(9, "Delete bar(s) on this track...", engine().getBarCount() >= 1);
	m.addItem(10, "Copy bar(s) on this track to...", engine().trackHasEvents(track));
	m.addItem(11, "Transpose bar(s) on this track...", engine().trackHasEvents(track));
	// Always enabled, even when the current bar itself is empty - the dialog's own "< Bar N >"
	// strip can browse to a bar that does have notes without closing and reopening the menu,
	// but only once it's actually open (Alan's own point: needing bar 3 or 4 while looking at
	// an empty bar 1).
	m.addItem(13, "Edit events in bar " + juce::String(engine().getCurrentBar()) + "...", true);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this, track](int result) {
		using d110seq::QuantizeGrid;
		if (result == 8) { confirmClearTrack(track); return; }
		if (result == 9) { promptForDeleteBars(track); return; }
		if (result == 10) { promptForCopyBars(track); return; }
		if (result == 11) { promptForTransposeBars(track); return; }
		if (result == 12) { promptForRenameTrack(track); return; }
		if (result == 13) { promptForEventList(track); return; }
		if (result == 14) { promptForTrackProgram(track); return; }
		QuantizeGrid grid;
		switch (result) {
			case 1: grid = QuantizeGrid::off; break;
			case 2: grid = QuantizeGrid::quarter; break;
			case 3: grid = QuantizeGrid::eighth; break;
			case 4: grid = QuantizeGrid::sixteenth; break;
			case 5: grid = QuantizeGrid::eighthTriplet; break;
			case 6: grid = QuantizeGrid::sixteenthTriplet; break;
			case 7: grid = QuantizeGrid::thirtySecond; break;
			default: return;
		}
		engine().pushUndoSnapshot("Quantize (" + defaultTrackLabel(track) + ")");
		engine().quantizeTrack(track, grid);
		repaint();
	});
}

// Only reachable when processor.supportsTrackChannelEdit() - see D110SequencerHost.h.
void D110SequencerPanel::showTrackChannelMenu(int track) {
	const int current = engine().channelForTrack(track);
	juce::PopupMenu m;
	for (int ch = 1; ch <= 16; ++ch) m.addItem(ch, "Channel " + juce::String(ch), true, ch == current);
	// Item IDs 1-16 are channels (above); 100 is the Program Change prompt, out of that range
	// so the two never collide - see promptForTrackProgram().
	if (processor.supportsProgramChangeForTrack(track)) {
		const int program = processor.getTrackProgram(track);
		m.addSeparator();
		m.addItem(100, program < 0 ? juce::String("Program Change: none...")
		                           : "Program change " + juce::String(program + 1) + " / bank "
		                                 + juce::String(processor.getTrackBank(track)) + "...");
	}
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
	                 [this, track](int result) {
		                 if (result == 100) { promptForTrackProgram(track); return; }
		                 if (result < 1 || result > 16) return;
		                 processor.setTrackChannel(track, result);
		                 repaint();
	                 });
}

// Called when either supportsProgramChangeForTrack(track) or supportsTrackVolumePanForTrack(track)
// is true - see D110SequencerHost.h. Values entered/shown 1-128 (standard musician-facing
// patch/bank numbering); stored/sent as raw MIDI 0-127 - see setTrackProgram()/getTrackProgram()/
// setTrackBank()/getTrackBank() (NonetSeqHost.cpp or PluginProcessor.cpp, depending on host).
// Rhythm (D-110 plugin only) has no Program Change equivalent but does have its own fixed
// Volume/Pan (2026-08-21, Alan's request) - hasProgram is false there, so Bank/BankLsb/Program
// are skipped entirely and the dialog becomes a plain "CC Change" (Volume/Pan only).
void D110SequencerPanel::promptForTrackProgram(int track) {
	const bool hasProgram = processor.supportsProgramChangeForTrack(track);
	const int current = hasProgram ? processor.getTrackProgram(track) : -1;
	const int currentBank = hasProgram ? processor.getTrackBank(track) : 1;
	const bool hasLsb = hasProgram && processor.supportsBankLsb();
	const int currentBankLsb = hasLsb ? processor.getTrackBankLsb(track) : 1;
	const bool hasVolPan = processor.supportsTrackVolumePanForTrack(track);
	const int currentVolume = hasVolPan ? processor.getTrackVolume(track) : -1;
	const int currentPan = hasVolPan ? processor.getTrackPan(track) : -1;
	// No override set yet - offer getTrackProgramHint()/getTrackVolumeHint()/getTrackPanHint()
	// (whatever this Part is actually playing right now - the D-110 plugin has these, Nonet-Seq
	// doesn't) as a greyed placeholder rather than committed text: these fields must stay
	// genuinely EMPTY when no override is set, so (a) it's visually obvious nothing's actually
	// stored here - important right after New, which resets the override but not whatever the
	// instrument happens to still be playing - and (b) pressing OK without touching a field
	// can't silently turn a mere hint into a real stored override.
	const int programHint = hasProgram ? processor.getTrackProgramHint(track) : -1;
	const int volumeHint = hasVolPan ? processor.getTrackVolumeHint(track) : -1;
	const int panHint = hasVolPan ? processor.getTrackPanHint(track) : -1;
	auto *aw = new juce::AlertWindow(
		hasProgram ? "Program Change" : "CC Change",
		(hasProgram
			 ? juce::String(
				   "Sent once on this track's channel when PLAY or REC starts, so the receiving synth "
				   "picks the right patch on its own. Leave Program blank to send none.\n\n"
				   "On the D-110 itself: no separate Bank Select exists on this instrument, so BANK just "
				   "folds into the raw value - Bank 1/Program 1-128 addresses one of its 128 Timbre "
				   "Memory slots directly (same numbering as the TIMBRES tab), Bank 2/Program 1-64 reaches "
				   "the second half, the \"B\" page on the instrument's own panel. On any other synth "
				   "(Nonet Sequencer), MIDI actually has two Bank Select controllers - CC0 (Bank/high, "
				   "the MSB) and CC32 (Bank LSB/low) - sent ahead of the Program Change, in that order, "
				   "as most external gear expects; many synths only look at one of the two, but both are "
				   "here since which one varies by device.")
			 : juce::String(
				   "This track has no Program Change equivalent (its sounds are picked per key on the "
				   "RHYTHM tab, not by a single patch number), but it can still carry a fixed Volume/Pan, "
				   "sent once the same moment PLAY or REC starts."))
			+ juce::String(hasVolPan
				? (hasLsb ? "\n\nVOLUME (0-100) and PAN (0-14, 7=centre) send real MIDI CC7/CC10, "
				            "scaled to the wire's 0-127, the same moment as the Program Change. "
				            "Leave either blank to send neither."
				          : "\n\nVOLUME (0-100) and PAN (0-14, 7=centre) are the same Part LEVEL/PAN "
				            "the PARTS tab edits, sent the same moment as the Program Change. Leave "
				            "either blank to send neither.")
				: ""),
		juce::AlertWindow::NoIcon);
	if (hasProgram) {
		aw->addTextEditor("bank", juce::String(currentBank), hasLsb ? "Bank/high (1-128):" : "Bank (1-128):");
		if (hasLsb) aw->addTextEditor("bankLsb", juce::String(currentBankLsb), "Bank LSB/low (1-128):");
		aw->addTextEditor("program", current >= 0 ? juce::String(current + 1) : juce::String(), "Program (1-128):");
	}
	if (hasVolPan) {
		aw->addTextEditor("volume", currentVolume >= 0 ? juce::String(currentVolume) : juce::String(),
		                   "Volume (0-100):");
		aw->addTextEditor("pan", currentPan >= 0 ? juce::String(currentPan) : juce::String(), "Pan (0-14):");
	}
	// Greyed placeholder, shown only while the field itself is genuinely empty - see this
	// function's own comment above on why a hint must never become committed text.
	if (hasProgram) {
		if (auto *programEditor = aw->getTextEditor("program"))
			if (current < 0 && programHint >= 0)
				programEditor->setTextToShowWhenEmpty(
					"now: " + juce::String(programHint + 1), juce::Colours::grey);
	}
	if (hasVolPan) {
		if (auto *volumeEditor = aw->getTextEditor("volume"))
			if (currentVolume < 0 && volumeHint >= 0)
				volumeEditor->setTextToShowWhenEmpty("now: " + juce::String(volumeHint), juce::Colours::grey);
		if (auto *panEditor = aw->getTextEditor("pan"))
			if (currentPan < 0 && panHint >= 0)
				panEditor->setTextToShowWhenEmpty("now: " + juce::String(panHint), juce::Colours::grey);
	}

	// "Pick instrument..." - Nonet Sequencer only (supportsInstrumentDefinitions()), and only
	// once a .idf has actually been loaded (OPTIONS > Instrument Definition) - see
	// D110SequencerHost::supportsInstrumentDefinitions()'s own comment for why the D-110 plugin
	// never offers this. Fills the Program/Bank/Bank LSB fields above by name rather than
	// requiring the number to be looked up by hand; still just text in those same fields
	// afterwards; nothing commits until OK. A patch whose .idf entry left bank/bankLsb unset
	// (-1) leaves whatever's already typed there alone, rather than clobbering a manual pick
	// with a meaningless value.
	juce::TextButton *pickButton = nullptr;
	if (hasProgram && processor.supportsInstrumentDefinitions()) {
		const auto patches = processor.getInstrumentPatches();
		if (!patches.empty()) {
			pickButton = new juce::TextButton("Pick instrument (" + processor.getInstrumentDefinitionName() + ")...");
			// AlertWindow draws each custom component's OWN Component::getName() as a floating
			// label above it (see its own paint(), the same way it labels a text editor) -
			// TextButton's constructor sets that name to the button text itself, which would
			// otherwise draw the button's own caption a second time right above it.
			pickButton->setName({});
			// AlertWindow::updateLayout() sizes its whole dialog off each custom component's
			// OWN getWidth()/getHeight() (see its own loop over customComps) - an unsized
			// component contributes zero height and is invisible, so this has to be set before
			// addCustomComponent() below, not left to the button's own default.
			pickButton->setSize(280, 26);
			pickButton->onClick = [aw, pickButton, patches, hasLsb] {
				juce::PopupMenu menu;
				juce::PopupMenu sub;
				juce::String currentGroup;
				int id = 0;
				auto flushGroup = [&] {
					if (sub.getNumItems() > 0) menu.addSubMenu(currentGroup.isEmpty() ? "(ungrouped)" : currentGroup, sub);
					sub = juce::PopupMenu();
				};
				for (auto &p : patches) {
					if (p.group != currentGroup) { flushGroup(); currentGroup = p.group; }
					sub.addItem(++id, p.name);
				}
				flushGroup();
				menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(pickButton), [aw, patches, hasLsb](int result) {
					if (result <= 0 || result > (int) patches.size()) return;
					const auto &p = patches[(size_t) (result - 1)];
					if (auto *programEditor = aw->getTextEditor("program"))
						programEditor->setText(juce::String(p.prog + 1), juce::dontSendNotification);
					if (p.bank >= 0)
						if (auto *bankEditor = aw->getTextEditor("bank"))
							bankEditor->setText(juce::String(p.bank + 1), juce::dontSendNotification);
					if (hasLsb && p.bankLsb >= 0)
						if (auto *bankLsbEditor = aw->getTextEditor("bankLsb"))
							bankLsbEditor->setText(juce::String(p.bankLsb + 1), juce::dontSendNotification);
				});
			};
			aw->addCustomComponent(pickButton);
		}
	}

	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create(
		[this, aw, track, hasProgram, hasLsb, hasVolPan, pickButton](int result) {
		if (result == 1) {
			if (hasProgram) {
				const juce::String text = aw->getTextEditorContents("program").trim();
				processor.setTrackProgram(track, text.isEmpty() ? -1 : juce::jlimit(1, 128, text.getIntValue()) - 1);
				processor.setTrackBank(track, aw->getTextEditorContents("bank").trim().getIntValue());
				if (hasLsb) processor.setTrackBankLsb(track, aw->getTextEditorContents("bankLsb").trim().getIntValue());
			}
			if (hasVolPan) {
				const juce::String volText = aw->getTextEditorContents("volume").trim();
				processor.setTrackVolume(track, volText.isEmpty() ? -1 : volText.getIntValue());
				const juce::String panText = aw->getTextEditorContents("pan").trim();
				processor.setTrackPan(track, panText.isEmpty() ? -1 : panText.getIntValue());
			}
			repaint();
		}
		delete pickButton;
		delete aw;
	}));
}

// Only reachable when processor.supportsExtraTracks() - see D110SequencerHost.h. Right-click
// anywhere in extraTracksZoneBounds, whether or not extra tracks are currently on - this is
// the only way to turn them on in the first place.
void D110SequencerPanel::showExtraTracksMenu() {
	auto &eng = engine();
	const bool on = eng.getExtraTracksEnabled();
	juce::PopupMenu m;
	m.addItem(1, "Activate extra tracks (16 total)", true, on);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
	                 [this](int result) {
		                 if (result != 1) return;
		                 toggleExtraTracks();
	                 });
}

void D110SequencerPanel::toggleExtraTracks() {
	auto &e = engine();
	e.setExtraTracksEnabled(!e.getExtraTracksEnabled());
	// Switching off: page 1 would otherwise keep showing tracks that no longer play/export/
	// undo as a group - jump back to the page that's always meaningful.
	if (!e.getExtraTracksEnabled()) trackPage = 0;
	repaint();
}

// Empty input clears back to the default "PART N"/"RHYTHM" label (isNotEmpty() in
// D110SequencerEngine::saveMidiFile()/paint() below both read an empty name as "not set").
void D110SequencerPanel::promptForRenameTrack(int track) {
	const juce::String defaultName = defaultTrackLabel(track);
	auto *aw = new juce::AlertWindow(
		"Rename track", "Shown here and in the MIDI file this track is exported to. Leave blank for \""
			+ defaultName + "\".",
		juce::AlertWindow::NoIcon);
	aw->addTextEditor("name", engine().getTrackName(track), "Name:");
	aw->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot("Rename (" + defaultTrackLabel(track) + ")");
			engine().setTrackName(track, aw->getTextEditorContents("name").trim());
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::confirmClearTrack(int track) {
	const juce::String name = defaultTrackLabel(track);
	auto *aw = new juce::AlertWindow(
		"Clear this track?",
		"This clears every recorded event on " + name
			+ ". Mute/solo/quantize and every other track are kept. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track, name](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot("Clear track (" + name + ")");
			engine().clearTrack(track);
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::cycleLoopMode() {
	using d110seq::LoopMode;
	auto &eng = engine();
	switch (eng.getLoopMode()) {
		case LoopMode::off: eng.setLoopMode(LoopMode::bar); break;
		case LoopMode::bar: eng.setLoopMode(LoopMode::punch); break;
		case LoopMode::punch: eng.setLoopMode(LoopMode::off); break;
	}
}

// Right-click on the BAR readout: jump to an entered bar, or set the punch in/out range
// that LoopMode::punch (and, while it's active, recording) uses.
void D110SequencerPanel::showBarMenu() {
	auto &eng = engine();
	juce::PopupMenu m;
	m.addItem(1, "Go to bar...");
	m.addSeparator();
	m.addItem(2, "Set punch in here (bar " + juce::String(eng.getCurrentBar()) + ")");
	m.addItem(3, "Set punch out here (bar " + juce::String(eng.getCurrentBar()) + ")");
	m.addItem(4, "Set punch in/out...");
	m.addSeparator();
	m.addItem(5, "Delete bar(s) (all tracks)...");
	m.addItem(6, "Copy bar(s) to... (all tracks)");
	m.addItem(7, "Transpose bar(s) (all tracks)...");

	// See onBarMenuButtonExtra's own comment - self-contained action callbacks (the
	// std::function overload of addItem, not the numeric-ID one every item above uses), so
	// they need no coordination with the switch below at all.
	if (onBarMenuButtonExtra) {
		m.addSeparator();
		onBarMenuButtonExtra(m);
	}

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this](int result) {
		auto &e = engine();
		switch (result) {
			case 1: promptForBar(); return;
			case 2: e.setPunchIn(e.getCurrentBar()); break;
			case 3: e.setPunchOut(e.getCurrentBar()); break;
			case 4: promptForPunchRange(); return;
			case 5: promptForDeleteBars(-1); return;
			case 6: promptForCopyBars(-1); return;
			case 7: promptForTransposeBars(-1); return;
			default: return;
		}
		repaint();
	});
}

void D110SequencerPanel::promptForTempo() {
	auto &eng = engine();
	auto *aw = new juce::AlertWindow("Set tempo", "Enter a tempo in BPM (20-300), or tap the beat.",
	                                  juce::AlertWindow::NoIcon);
	aw->addTextEditor("bpm", juce::String(eng.getTempo(), 1), "BPM:");

	// TAP used to be its own permanent panel button (Alan's request, 2026-08-22: freed that
	// column for the bar-navigation menu button instead - see layout()'s own comment) - it
	// lives here now. addCustomComponent(), not addButton(): every addButton() exits the
	// AlertWindow's modal state on click, but tapping several times in a row to find a tempo
	// needs the dialog to stay open and the BPM field to keep updating live.
	auto *tapButton = new juce::TextButton("Tap");
	tapButton->setSize(80, 24);
	tapButton->onClick = [this, aw] {
		// Not a captured reference to the eng local above - this outlives promptForTempo()
		// returning, so it re-fetches the (long-lived) engine fresh, same as the modal
		// callback below already has to.
		auto &liveEngine = engine();
		liveEngine.registerTapTempo();
		if (auto *editor = aw->getTextEditor("bpm"))
			editor->setText(juce::String(liveEngine.getTempo(), 1), juce::dontSendNotification);
	};
	aw->addCustomComponent(tapButton);

	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, tapButton](int result) {
		if (result == 1) {
			const double bpm = aw->getTextEditorContents("bpm").getDoubleValue();
			if (bpm > 0.0) engine().setTempo(bpm);
			repaint();
		}
		delete tapButton; // addCustomComponent() takes no ownership - see its own doc comment
		delete aw;
	}));
}

void D110SequencerPanel::promptForBar() {
	auto &eng = engine();
	auto *aw = new juce::AlertWindow("Go to bar", "Enter the bar number to jump to.",
	                                  juce::AlertWindow::NoIcon);
	aw->addTextEditor("bar", juce::String(eng.getCurrentBar()), "Bar:");
	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			const int bar = aw->getTextEditorContents("bar").getIntValue();
			if (bar >= 1) engine().gotoBar(bar);
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::promptForPunchRange() {
	auto &eng = engine();
	auto *aw = new juce::AlertWindow(
		"Punch in / out",
		"Used by LOOP: PUNCH for playback looping, and to record only within this range "
		"while that loop mode is active.",
		juce::AlertWindow::NoIcon);
	aw->addTextEditor("in", juce::String(eng.getPunchIn()), "Punch in bar:");
	aw->addTextEditor("out", juce::String(eng.getPunchOut()), "Punch out bar:");
	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			auto &e = engine();
			const int in = aw->getTextEditorContents("in").getIntValue();
			const int out = aw->getTextEditorContents("out").getIntValue();
			if (in >= 1 && out >= 1) e.setPunchRange(in, out);
			repaint();
		}
		delete aw;
	}));
}

// Destructive (see D110SequencerEngine::deleteBars()) and unrecoverable, so it confirms with a
// warning icon the same way confirmClearTrack() does, rather than acting straight from the menu.
void D110SequencerPanel::promptForDeleteBars(int track) {
	auto &eng = engine();
	const juce::String scope = track < 0 ? "every track" : defaultTrackLabel(track);
	auto *aw = new juce::AlertWindow(
		"Delete bar(s)",
		"Removes the given bar range from " + scope
			+ " and closes the gap by shifting everything after it earlier. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addTextEditor("from", juce::String(eng.getCurrentBar()), "From bar:");
	aw->addTextEditor("to", juce::String(eng.getCurrentBar()), "To bar:");
	aw->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track, scope](int result) {
		if (result == 1) {
			const int from = aw->getTextEditorContents("from").getIntValue();
			const int to = aw->getTextEditorContents("to").getIntValue();
			if (from >= 1 && to >= from) {
				engine().pushUndoSnapshot("Delete bars " + juce::String(from) + "-" + juce::String(to) + " (" + scope + ")");
				engine().deleteBars(track, from, to);
			}
			repaint();
		}
		delete aw;
	}));
}

// Inserts (see D110SequencerEngine::copyBars()) - pushes whatever's already at/after the
// destination later rather than overwriting it, so this is destructive only in the sense that
// it changes bar numbers from the destination onward; still confirmed with a warning icon since
// that reflow can't be undone either.
void D110SequencerPanel::promptForCopyBars(int track) {
	auto &eng = engine();
	const juce::String scope = track < 0 ? "every track, independently" : defaultTrackLabel(track);
	auto *aw = new juce::AlertWindow(
		"Copy bar(s)",
		"Copies the given bar range and inserts it at the destination bar on " + scope
			+ ", pushing anything already there later to make room. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addTextEditor("from", juce::String(eng.getCurrentBar()), "From bar:");
	aw->addTextEditor("to", juce::String(eng.getCurrentBar()), "To bar:");
	aw->addTextEditor("dest", juce::String(eng.getBarCount() + 1), "Destination bar:");
	// Only a single track's worth of notes can go to a chosen OTHER track - copying "every
	// track" always keeps each one going to its own same track, so they stay aligned (see
	// copyBars()'s own comment on srcTrack == destTrack == -1). Offers every currently ACTIVE
	// track as a destination, not just the base 9, so an extra track can be a copy target too
	// once enabled.
	if (track >= 0) {
		juce::StringArray names;
		for (int t = 0; t < eng.activeTrackCount(); ++t) names.add(defaultTrackLabel(t));
		aw->addComboBox("destTrack", names, "Destination track:");
		aw->getComboBoxComponent("destTrack")->setSelectedItemIndex(track, juce::dontSendNotification);
	}
	aw->addButton("Copy", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track, scope](int result) {
		if (result == 1) {
			const int from = aw->getTextEditorContents("from").getIntValue();
			const int to = aw->getTextEditorContents("to").getIntValue();
			const int dest = aw->getTextEditorContents("dest").getIntValue();
			const int destTrack = track >= 0 ? aw->getComboBoxComponent("destTrack")->getSelectedItemIndex() : -1;
			if (from >= 1 && to >= from && dest >= 1) {
				engine().pushUndoSnapshot("Copy bars " + juce::String(from) + "-" + juce::String(to) + " (" + scope + ")");
				engine().copyBars(track, destTrack, from, to, dest);
			}
			repaint();
		}
		delete aw;
	}));
}

// Transposes in place - see D110SequencerEngine::transposeBars(). Not confirmed with a warning
// dialog the way delete/copy bars are (nothing is removed or reflowed, only pitches shift), but
// still checkpointed for UNDO since it can touch a lot of notes at once.
void D110SequencerPanel::promptForTransposeBars(int track) {
	auto &eng = engine();
	const juce::String scope = track < 0 ? "every track, independently" : defaultTrackLabel(track);
	auto *aw = new juce::AlertWindow(
		"Transpose bar(s)",
		"Shifts the pitch of every note in the given bar range on " + scope
			+ " by the given number of semitones (negative to transpose down).",
		juce::AlertWindow::NoIcon);
	aw->addTextEditor("from", juce::String(eng.getCurrentBar()), "From bar:");
	aw->addTextEditor("to", juce::String(eng.getCurrentBar()), "To bar:");
	aw->addTextEditor("semitones", "0", "Semitones:");
	aw->addButton("Transpose", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track, scope](int result) {
		if (result == 1) {
			const int from = aw->getTextEditorContents("from").getIntValue();
			const int to = aw->getTextEditorContents("to").getIntValue();
			const int semitones = aw->getTextEditorContents("semitones").getIntValue();
			if (from >= 1 && to >= from && semitones != 0) {
				engine().pushUndoSnapshot("Transpose bars " + juce::String(from) + "-" + juce::String(to) + " (" + scope + ")");
				engine().transposeBars(track, from, to, semitones);
			}
			repaint();
		}
		delete aw;
	}));
}

// See the header comment - a plain list of the bar's own notes, each deletable on its own or
// retunable to a new pitch, rather than a bar-range operation or a piano roll. The dialog
// stays open across edits (so several wrong notes can be fixed in one sitting) and across bar
// changes (the "< Bar N >" strip - see NoteEventListHeader), closing only on CLOSE/Escape.
// Every edit is its own undo checkpoint, same as every other destructive edit here, so UNDO
// reverts them one at a time regardless of how many happened in this one dialog session.
void D110SequencerPanel::promptForEventList(int track) {
	// Shared, mutable across every lambda below (refresh, the header's Prev/Next, the pitch
	// prompt) - a shared_ptr rather than a raw captured int so its lifetime is tied to
	// whichever of those closures outlives the others, with nothing here to free by hand.
	auto barState = std::make_shared<int>(engine().getCurrentBar());

	auto *header = new NoteEventListHeader();
	header->setSize(380, 24);
	header->setBar(*barState);

	auto *content = new NoteEventListContent();
	auto *viewport = new juce::Viewport();
	viewport->setViewedComponent(content, true);
	viewport->setSize(380, 220);
	viewport->setScrollBarsShown(true, false);

	auto refresh = [this, track, barState, content] {
		std::vector<NoteEventListContent::Row> newRows;
		for (const auto &ev : engine().eventsInBarRange(track, *barState, *barState))
			newRows.push_back({ ev.index, ev.note, formatEventRow(ev) });
		content->setRows(std::move(newRows));
	};
	content->onDelete = [this, track, refresh](int index) {
		engine().pushUndoSnapshot("Delete note (" + defaultTrackLabel(track) + ")");
		engine().deleteNoteEvent(track, index);
		refresh();
		repaint();
	};
	content->onEditPitch = [this, track, refresh](int index, int currentNote) {
		auto *aw2 = new juce::AlertWindow(
			"Change note",
			"Note name, e.g. E5 or C#4 (currently " + juce::MidiMessage::getMidiNoteName(currentNote, true, true, 4)
				+ ").",
			juce::AlertWindow::NoIcon);
		aw2->addTextEditor("note", juce::MidiMessage::getMidiNoteName(currentNote, true, true, 4), "New note:");
		aw2->addButton("Change", 1, juce::KeyPress(juce::KeyPress::returnKey));
		aw2->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
		aw2->enterModalState(true, juce::ModalCallbackFunction::create([this, aw2, track, index, refresh](int result) {
			const int newNote = noteNameToNumber(aw2->getTextEditorContents("note"));
			if (result == 1 && newNote >= 0) {
				engine().pushUndoSnapshot("Edit note pitch (" + defaultTrackLabel(track) + ")");
				engine().setNoteEventPitch(track, index, newNote);
				refresh();
				repaint();
			}
			delete aw2;
		}));
	};
	header->onPrev = [barState, header, refresh] {
		*barState = juce::jmax(1, *barState - 1);
		header->setBar(*barState);
		refresh();
	};
	header->onNext = [barState, header, refresh] {
		*barState += 1;
		header->setBar(*barState);
		refresh();
	};
	refresh();

	auto *aw = new juce::AlertWindow("Events - " + defaultTrackLabel(track),
	                                  "Click a note to change its pitch, or the X to delete it. Each edit can be "
	                                  "undone with the main UNDO button.",
	                                  juce::AlertWindow::NoIcon);
	aw->addCustomComponent(header);
	aw->addCustomComponent(viewport);
	aw->addButton("Close", 0, juce::KeyPress(juce::KeyPress::returnKey), juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([aw, header, viewport](int) {
		delete header;
		delete viewport; // owns content (see setViewedComponent(content, true) above)
		delete aw;
	}));
}

// Right-click any of the 4 slot buttons: offers copying the CURRENT song into one of the
// other 3 (regardless of which slot's button was actually clicked - there's only one sensible
// source, whichever song is live right now, so that half of the menu doesn't need to
// distinguish), plus - only when processor.supportsSoundSnapshots() - storing/loading the
// CLICKED slot's own sound snapshot (unlike the copy items, these DO care which slot was
// clicked: "store" and "load" only make sense against one specific slot).
void D110SequencerPanel::showCopySongMenu(int clickedSlot) {
	auto &eng = engine();
	const int current = eng.getCurrentSongSlot();

	juce::PopupMenu m;
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s) {
		if (s == current) continue;
		juce::String label = "Copy this song to Slot " + juce::String(s + 1);
		if (eng.songSlotHasContent(s)) label += " (overwrites it)";
		m.addItem(s + 1, label);
	}

	if (processor.supportsSoundSnapshots()) {
		m.addSeparator();
		m.addItem(100, "Store current sounds in Slot " + juce::String(clickedSlot + 1)
		                   + (processor.hasSoundSnapshot(clickedSlot) ? " (overwrites it)" : juce::String()));
		m.addItem(101, "Load Slot " + juce::String(clickedSlot + 1) + "'s stored sounds",
		          processor.hasSoundSnapshot(clickedSlot));
	}

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
	                [this, clickedSlot](int result) {
		if (result == 100) { processor.storeSoundSnapshotForSlot(clickedSlot); repaint(); return; }
		if (result == 101) { confirmLoadSoundSnapshot(clickedSlot); return; }
		if (result < 1) return;
		confirmCopySongTo(result - 1);
	});
}

// Loading a sound snapshot power-cycles the instrument and replaces its entire live memory
// (see D110AudioProcessor::loadSoundSnapshotForSlot()) - a real, felt interruption and a real
// loss of whatever sounds are live right now if they were never stored anywhere themselves,
// so this confirms first, same reasoning as confirmCopySongTo().
void D110SequencerPanel::confirmLoadSoundSnapshot(int slot) {
	auto *aw = new juce::AlertWindow(
		"Load Slot " + juce::String(slot + 1) + "'s stored sounds?",
		"This replaces the instrument's entire current memory (every Patch, Timbre and Tone) "
		"with what was stored for this slot, and power-cycles it to do so - a brief reboot, "
		"felt immediately. Whatever sounds are live right now are lost unless they were "
		"stored somewhere first.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Load", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, slot](int result) {
		if (result == 1) {
			processor.loadSoundSnapshotForSlot(slot);
			repaint();
		}
		delete aw;
	}));
}

// Destructive to destSlot's existing content (see D110SequencerEngine::copyCurrentSongTo()), so
// it always confirms first, the same as confirmClearTrack()/confirmNewSong() - except when
// destSlot is empty, where there's nothing to lose and confirming would just be a needless click.
void D110SequencerPanel::confirmCopySongTo(int destSlot) {
	auto &eng = engine();
	const int current = eng.getCurrentSongSlot();
	if (!eng.songSlotHasContent(destSlot)) {
		eng.pushUndoSnapshot("Copy song to Slot " + juce::String(destSlot + 1));
		eng.copyCurrentSongTo(destSlot);
		repaint();
		return;
	}
	auto *aw = new juce::AlertWindow(
		"Copy song to Slot " + juce::String(destSlot + 1) + "?",
		"This overwrites Slot " + juce::String(destSlot + 1) + " with a copy of Slot "
			+ juce::String(current + 1) + " (the current song). UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Copy", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, destSlot](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot("Copy song to Slot " + juce::String(destSlot + 1));
			engine().copyCurrentSongTo(destSlot);
			repaint();
		}
		delete aw;
	}));
}

// NEW clears the current song slot's tracks - destructive and unrecoverable, so it always
// confirms first (Alan's own explicit ask, to avoid an accidental wipe).
void D110SequencerPanel::confirmNewSong() {
	const int slot = engine().getCurrentSongSlot();
	const juce::String description = "New song (Slot " + juce::String(slot + 1) + ")";
	auto *aw = new juce::AlertWindow(
		"Clear this song?",
		"This clears every track's recorded MIDI in song " + juce::String(slot + 1)
			+ ". Tempo, time signature and other settings are kept. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, description](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot(description);
			// newSong() also resets tempo and the fixed per-track Program Change/Bank/Volume/
			// Pan override for this slot's own tracks - see its own comment.
			engine().newSong();
			repaint();
		}
		delete aw;
	}));
}

int D110SequencerPanel::trackForRow(int row) const {
	return row + (trackPage == 0 ? 0 : kNumTracks);
}

int D110SequencerPanel::rowsOnCurrentPage() const {
	return trackPage == 0 ? kNumTracks : (kMaxTracks - kNumTracks);
}

void D110SequencerPanel::layout() {
	auto area = getLocalBounds().toFloat();
	if (area.getWidth() < 1.0f || area.getHeight() < 1.0f) return;

	// Android portrait (a mode-switch button is set and the panel is taller than wide): the
	// single 13-column transport row is far too fine for a phone held upright, so the same
	// controls are stacked in three taller rows instead, with a bigger label font:
	//   1. STOP PLAY REC | BPM | SIG | METRO | PRECOUNT | mode | menu
	//   (metronome LED strip)
	//   2. LOOP < BAR > | REC MODE | NEW | song slots 1-4
	//   3. UNDO REDO | SYNC | LOAD | SAVE
	stackedTransport = bool(onModeButton) && getHeight() > getWidth();
	if (stackedTransport) {
		const float rowH = 46.0f;
		auto place = [](juce::Rectangle<float> row, std::initializer_list<float> weights,
		                std::initializer_list<juce::Rectangle<float> *> out) {
			float total = 0.0f;
			for (float w : weights) total += w;
			float x = row.getX();
			auto o = out.begin();
			for (float w : weights) {
				const float cw = row.getWidth() * w / total;
				**o = juce::Rectangle<float>(x, row.getY(), cw - 4.0f, row.getHeight());
				x += cw;
				++o;
			}
		};
		auto row1 = area.removeFromTop(juce::jmin(rowH, area.getHeight() * 0.1f));
		place(row1, { 0.10f, 0.10f, 0.09f, 0.15f, 0.09f, 0.11f, 0.17f, 0.085f, 0.085f },
		      { &stopBounds, &playBounds, &recBounds, &tempoBounds, &timeSigBounds, &metronomeBounds,
		        &precountBounds, &modeBounds, &barMenuBounds });
		area.removeFromTop(2.0f);
		metroLedBounds = area.removeFromTop(14.0f).reduced(2.0f, 0.0f);
		area.removeFromTop(2.0f);
		auto row2 = area.removeFromTop(juce::jmin(rowH, area.getHeight() * 0.1f));
		place(row2, { 0.13f, 0.09f, 0.17f, 0.09f, 0.22f, 0.08f, 0.055f, 0.055f, 0.055f, 0.055f },
		      { &loopBounds, &barPrevBounds, &barReadoutBounds, &barNextBounds, &recModeBounds, &newBounds,
		        &slotBounds[0], &slotBounds[1], &slotBounds[2], &slotBounds[3] });
		area.removeFromTop(2.0f);
		auto row3 = area.removeFromTop(juce::jmin(rowH, area.getHeight() * 0.1f));
		if (processor.supportsCaptureLivePatch()) {
			place(row3, { 0.16f, 0.16f, 0.16f, 0.26f, 0.26f },
			      { &undoBounds, &redoBounds, &resyncBounds, &loadBounds, &saveBounds });
		} else {
			resyncBounds = {};
			place(row3, { 0.20f, 0.20f, 0.30f, 0.30f }, { &undoBounds, &redoBounds, &loadBounds, &saveBounds });
		}
		area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));
	} else {
		auto transport = area.removeFromTop(juce::jmin(40.0f, area.getHeight() * 0.22f));
		const float tw = transport.getWidth();
		// With a mode-switch button (onModeButton, Android only) every column but the last two
		// squeezes by `sq` to free one more 0.042 slot left of the bar-menu button.
		const float sq = onModeButton ? 0.954f : 1.0f;
		auto colA = [&](float frac, float widthFrac) {
			return juce::Rectangle<float>(transport.getX() + tw * frac, transport.getY(),
			                               tw * widthFrac - 4.0f, transport.getHeight());
		};
		auto colT = [&](float frac, float widthFrac) { return colA(frac * sq, widthFrac * sq); };
		stopBounds = colT(0.000f, 0.060f);
		playBounds = colT(0.060f, 0.060f);
		recBounds = colT(0.120f, 0.060f);
		// TEMPO/TIME SIG share the span that used to also carry a standalone TAP TEMPO button
		// (Alan's request, 2026-08-22: TAP moved into the "Set tempo" dialog itself -
		// promptForTempo() now has its own Tap button that updates the BPM field live without
		// closing the dialog - freeing this column for barMenuBounds at the very end instead).
		tempoBounds = colT(0.185f, 0.100f);
		timeSigBounds = colT(0.288f, 0.064f);
		metronomeBounds = colT(0.360f, 0.120f);
		precountBounds = colT(0.485f, 0.130f);
		loopBounds = colT(0.620f, 0.090f);
		barPrevBounds = colT(0.715f, 0.045f);
		barReadoutBounds = colT(0.763f, 0.148f);
		barNextBounds = colT(0.915f, 0.040f);
		// The bar-navigation menu (showBarMenu()) used to only be reachable by right-click/long-
		// press on barReadoutBounds - this is the same menu, one tap away, in the column TAP's
		// removal freed up. See onBarMenuButtonExtra's own comment for why a host might also want
		// this specific button.
		barMenuBounds = colA(0.958f, 0.042f);
		modeBounds = onModeButton ? colA(0.915f, 0.042f) : juce::Rectangle<float>();

		area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

		// Visual metronome LED strip - one LED per metronome click in the bar, drawn only when
		// METRO is on (see paint()). Thin, non-interactive, sits right under the transport row.
		metroLedBounds = area.removeFromTop(juce::jmin(14.0f, area.getHeight() * 0.09f)).reduced(2.0f, 0.0f);
		area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

		// Second strip: record mode, NEW and the 4 song-slot buttons on the left, Load/Save
		// right-aligned on the right - all thinner than the busy transport row above.
		auto fileStrip = area.removeFromTop(juce::jmin(26.0f, area.getHeight() * 0.14f));
		const float fw = fileStrip.getWidth();
		auto colF = [&](float frac, float widthFrac) {
			return juce::Rectangle<float>(fileStrip.getX() + fw * frac, fileStrip.getY(), fw * widthFrac - 4.0f,
			                               fileStrip.getHeight());
		};
		recModeBounds = colF(0.000f, 0.220f);
		newBounds = colF(0.225f, 0.075f);
		for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s)
			slotBounds[static_cast<size_t>(s)] = colF(0.310f + float(s) * 0.048f, 0.044f);
		undoBounds = colF(0.535f, 0.075f);
		redoBounds = colF(0.615f, 0.075f);
		// SYNC (see showResyncInfo()'s own comment) is D-110-only - Nonet Sequencer has no live
		// patch to sync with, so it has no button here at all, and LOAD/SAVE reclaim its space.
		if (processor.supportsCaptureLivePatch()) {
			resyncBounds = colF(0.695f, 0.080f);
			loadBounds = colF(0.780f, 0.100f);
			saveBounds = colF(0.885f, 0.100f);
		} else {
			resyncBounds = {};
			loadBounds = colF(0.695f, 0.145f);
			saveBounds = colF(0.845f, 0.145f);
		}
	}
	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	// Third strip: step recording (see D110SequencerEngine's step API) - a toggle, the step
	// duration readout, REST/BACK, and a plain "bar N step M" readout while it's active.
	auto stepStrip = area.removeFromTop(juce::jmin(24.0f, area.getHeight() * 0.13f));
	const float sw = stepStrip.getWidth();
	auto colS = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(stepStrip.getX() + sw * frac, stepStrip.getY(), sw * widthFrac - 4.0f,
		                               stepStrip.getHeight());
	};
	stepBounds = colS(0.000f, 0.115f);
	stepDurationBounds = colS(0.125f, 0.130f);
	stepDotBounds = colS(0.265f, 0.075f);
	restBounds = colS(0.350f, 0.115f);
	backBounds = colS(0.475f, 0.115f);
	// Wider than a bare "Bar N step M" needs, now that it shows two lines (current step/total
	// and how many are left, per Alan's own ask 2026-08-19) - takes a bit of room from the
	// extra-tracks zone below, right of it, which is Nonet Sequencer only and rarely used.
	stepInfoBounds = colS(0.600f, 0.260f);
	// Nonet Sequencer only (processor.supportsExtraTracks()) - right-click anywhere in
	// extraTracksZoneBounds always opens "Activate extra tracks" (see showExtraTracksMenu());
	// the two page buttons inside it only draw/hit-test once that's on (see paint()/
	// mouseDown()). Computed unconditionally either way - cheap, and simpler than threading a
	// capability check through layout() too.
	extraTracksZoneBounds = colS(0.870f, 0.115f);
	trackPage1Bounds = colS(0.870f, 0.055f);
	trackPage2Bounds = colS(0.930f, 0.055f);
	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	// Row height always divides by kNumTracks (9), even on page 1 (only 7 real tracks) - so
	// switching pages never changes row height, just how many rows have a track behind them
	// (rowsOnCurrentPage()). Page 1's 2 unused row slots simply stay blank at the bottom.
	const float rowH = area.getHeight() / float(kNumTracks);
	const float rw = area.getWidth();
	auto colR = [&](juce::Rectangle<float> row, float frac, float widthFrac) {
		return juce::Rectangle<float>(row.getX() + rw * frac, row.getY(), rw * widthFrac - 4.0f,
		                               row.getHeight());
	};
	for (int rowIdx = 0; rowIdx < rowsOnCurrentPage(); ++rowIdx) {
		const int t = trackForRow(rowIdx);
		auto rowArea = area.removeFromTop(rowH);
		auto &r = rows[static_cast<size_t>(t)];
		r.rowBounds = rowArea;
		r.label = colR(rowArea, 0.000f, 0.190f);
		r.channelReadout = colR(rowArea, 0.195f, 0.100f);
		// MUTE/SOLO narrower than before (Alan: too wide) and ARM down to a small square
		// around its own record-style dot (see paintArmButton) rather than a text button -
		// the freed width goes to the activity bar and the new part-number reminder below.
		r.muteBounds = colR(rowArea, 0.300f, 0.085f);
		r.soloBounds = colR(rowArea, 0.390f, 0.085f);
		r.armBounds = colR(rowArea, 0.480f, 0.065f);
		r.activityBounds = colR(rowArea, 0.555f, 0.340f);
		// Extreme right: just the bare digit ("1".."8") or "R" for rhythm - a compact
		// reminder of which part this is, independent of whatever custom name/CH the rest
		// of the row shows (see setTrackName()).
		r.partNumberBounds = colR(rowArea, 0.905f, 0.080f);
	}
}

void D110SequencerPanel::resized() { layout(); }

void D110SequencerPanel::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);
	auto &eng = engine();

	gToggleFontMax = stackedTransport ? 17.0f : 13.0f;
	paintToggleButton(g, stopBounds, "STOP", !eng.isPlaying());
	paintToggleButton(g, playBounds, "PLAY", eng.isPlaying() && !eng.isRecording());
	paintToggleButton(g, recBounds, "REC", eng.isRecording());
	paintToggleButton(g, tempoBounds, juce::String(eng.getTempo(), 1) + " BPM", false);
	paintToggleButton(
		g, timeSigBounds,
		juce::String(eng.getTimeSigNumerator()) + "/" + juce::String(eng.getTimeSigDenominator()), false);
	paintToggleButton(g, metronomeBounds, "METRO", eng.getMetronomeEnabled());
	const int precountBars = eng.getPrecountBars();
	paintToggleButton(g, precountBounds,
	                   precountBars == 0 ? "PRECOUNT OFF" : "PRECOUNT " + juce::String(precountBars),
	                   precountBars > 0);
	paintToggleButton(g, loopBounds, loopModeLabel(eng.getLoopMode()), eng.getLoopMode() != d110seq::LoopMode::off);
	paintToggleButton(g, barPrevBounds, "<", false);
	paintToggleButton(g, barNextBounds, ">", false);
	paintToggleButton(g, barMenuBounds, juce::String::fromUTF8("\xe2\x98\xb0"), false); // U+2630
	if (onModeButton && modeButtonMode)
		d110ui::paintAppModeButton(g, modeBounds, d110ui::AppMode(modeButtonMode()));
	paintToggleButton(g, barReadoutBounds,
	                   "BAR " + juce::String(eng.getCurrentBar()) + "/" + juce::String(eng.getBarCount()),
	                   eng.isPrecounting());

	if (eng.isStepRecording() && metroLedBounds.getWidth() > 0.0f) {
		// Repurposed while step recording: the transport isn't actually running (there's no
		// real-time clock to click along to while typing steps one at a time), so the same LED
		// strip instead shows where the step cursor sits in the bar - re-subdivided to the step
		// grid rather than the beat grid, per Alan's own ask (2026-08-19): a quarter-note step
		// lights one whole LED, same as a normal beat click would; an eighth-note step
		// re-subdivides the same strip into twice as many, half-as-wide LEDs; and so on for any
		// grid (see D110SequencerEngine::getStepsPerBar()). Shown regardless of METRO being on -
		// it's a step-position indicator now, not a metronome.
		const int total = juce::jmax(1, eng.getStepsPerBar());
		const int active = eng.getStepIndexInBar() - 1;
		const float ledW = metroLedBounds.getWidth() / float(total);
		for (int i = 0; i < total; ++i) {
			juce::Rectangle<float> led(metroLedBounds.getX() + ledW * float(i), metroLedBounds.getY(),
			                            juce::jmax(1.0f, ledW - 3.0f), metroLedBounds.getHeight());
			const juce::Colour c = i == 0 ? pal.seqMetroDownbeat : pal.seqMetroBeat;
			g.setColour(i == active ? c : c.withAlpha(0.16f));
			g.fillRect(led);
		}
	} else if (eng.getMetronomeEnabled() && eng.getMetronomeMode() != d110seq::MetronomeMode::audioOnly
	           && (!eng.getMetronomeRecordOnly() || eng.isRecording()) && metroLedBounds.getWidth() > 0.0f) {
		const int total = juce::jmax(1, eng.clicksPerBar());
		// Precounting: only the downbeat LED is ever used, but it FLASHES once per beat of the
		// count-in (timerCallback() edge-detects each beat and opens a short window here) -
		// deliberately not scrolling through the strip the way normal recording does below, so
		// it still reads at a glance as "counting in, not recording yet", but each beat is
		// visible even without audio, per Alan's own ask.
		const bool precountFlashOn =
			eng.isPrecounting() && juce::Time::getMillisecondCounter() < precountFlashUntilMs;
		const int active = eng.isPrecounting() ? (precountFlashOn ? 0 : -1) : eng.currentClickInBar();
		const float ledW = metroLedBounds.getWidth() / float(total);
		for (int i = 0; i < total; ++i) {
			juce::Rectangle<float> led(metroLedBounds.getX() + ledW * float(i), metroLedBounds.getY(),
			                            juce::jmax(1.0f, ledW - 3.0f), metroLedBounds.getHeight());
			// Downbeat (leftmost) is orange, every other click is green - only the currently
			// active one is fully lit, the rest sit dim so the strip reads as "scrolling".
			const juce::Colour c = i == 0 ? pal.seqMetroDownbeat : pal.seqMetroBeat;
			g.setColour(i == active ? c : c.withAlpha(0.16f));
			g.fillRect(led);
		}
	}

	paintToggleButton(g, recModeBounds, recordModeLabel(eng.getRecordMode()), false);
	paintToggleButton(g, newBounds, "NEW", false);
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s) {
		const auto &b = slotBounds[static_cast<size_t>(s)];
		paintToggleButton(g, b, juce::String(s + 1), eng.getCurrentSongSlot() == s);
		if (eng.songSlotHasContent(s)) {
			g.setColour(pal.seqActiveFill);
			g.fillEllipse(b.getRight() - 8.0f, b.getY() + 3.0f, 4.0f, 4.0f);
		}
		// Second, separate dot for a stored sound snapshot (see D110SequencerHost.h's own
		// comment on why this is tracked apart from song content) - bottom-right so it never
		// overlaps the content dot above, a different colour so the two are never confused at
		// a glance.
		if (processor.supportsSoundSnapshots() && processor.hasSoundSnapshot(s)) {
			g.setColour(pal.seqMetroDownbeat);
			g.fillEllipse(b.getRight() - 8.0f, b.getBottom() - 7.0f, 4.0f, 4.0f);
		}
	}
	paintToggleButton(g, undoBounds, "UNDO", false, eng.canUndo());
	paintToggleButton(g, redoBounds, "REDO", false, eng.canRedo());
	if (processor.supportsCaptureLivePatch())
		paintToggleButton(g, resyncBounds, "SYNC", false, processor.supportsProgramChange());
	paintToggleButton(g, loadBounds, "LOAD", false);
	paintToggleButton(g, saveBounds, "SAVE", false);

	paintToggleButton(g, stepBounds, "STEP", eng.isStepRecording(), eng.isStepRecording() || eng.getArmedTrack() >= 0);
	paintToggleButton(g, stepDurationBounds, stepDurationLabel(eng.getStepDuration()), false);
	paintToggleButton(g, stepDotBounds, "DOT", eng.getStepDotted());
	paintToggleButton(g, restBounds, "REST", false, eng.isStepRecording());
	paintToggleButton(g, backBounds, "BACK", false, eng.isStepRecording());
	if (eng.isStepRecording()) {
		const int stepIndex = eng.getStepIndexInBar();
		const int stepsPerBar = eng.getStepsPerBar();
		auto infoArea = stepInfoBounds;
		auto line1 = infoArea.removeFromTop(infoArea.getHeight() * 0.5f);
		g.setColour(pal.handleLabel);
		g.setFont(juce::FontOptions(juce::jlimit(7.0f, 12.0f, line1.getHeight() * 0.85f)));
		g.drawText("Bar " + juce::String(eng.getStepBar()) + " step " + juce::String(stepIndex) + "/"
		               + juce::String(stepsPerBar),
		           line1, juce::Justification::centredLeft);
		g.setColour(pal.seqInactiveText);
		g.setFont(juce::FontOptions(juce::jlimit(7.0f, 12.0f, infoArea.getHeight() * 0.85f)));
		g.drawText(juce::String(juce::jmax(0, stepsPerBar - stepIndex)) + " step(s) left", infoArea,
		           juce::Justification::centredLeft);
	}

	// Page-switch buttons - Nonet Sequencer only, and only once extra tracks are actually on
	// (see D110SequencerHost::supportsExtraTracks()/showExtraTracksMenu()). Right-clicking
	// extraTracksZoneBounds to turn extras on/off works either way - see mouseDown().
	if (processor.supportsExtraTracks() && eng.getExtraTracksEnabled()) {
		paintToggleButton(g, trackPage1Bounds, "1-9", trackPage == 0);
		paintToggleButton(g, trackPage2Bounds, "10-16", trackPage == 1);
	}

	for (int rowIdx = 0; rowIdx < rowsOnCurrentPage(); ++rowIdx) {
		const int t = trackForRow(rowIdx);
		const auto &r = rows[static_cast<size_t>(t)];
		const bool isRhythm = t == D110SequencerEngine::kRhythmTrack;
		const juce::String defaultLabel = defaultTrackLabel(t);
		const juce::String customName = eng.getTrackName(t);

		g.setColour(pal.seqInactiveText);
		g.setFont(juce::FontOptions(juce::jlimit(9.0f, 14.0f, r.label.getHeight() * 0.5f)));
		g.drawText(customName.isNotEmpty() ? customName : defaultLabel, r.label,
		           juce::Justification::centredLeft);

		// A different colour when this readout is actually clickable (Nonet Sequencer only -
		// see D110SequencerHost::supportsTrackChannelEdit()), so it reads as a control there
		// and a plain readout in the plugin, where the channel only ever follows the live
		// firmware's own SYSTEM page.
		g.setColour(processor.supportsTrackChannelEdit() ? pal.value : pal.handleLabel);
		// Trailing "*" = this track has a Program Change set (see showTrackChannelMenu()) -
		// just a presence hint, the readout's too narrow for the actual program number.
		const bool hasProgram = processor.supportsProgramChange() && processor.getTrackProgram(t) >= 0;
		g.drawText("CH " + juce::String(eng.channelForTrack(t)) + (hasProgram ? "*" : ""), r.channelReadout,
		           juce::Justification::centredLeft);

		paintToggleButton(g, r.muteBounds, "MUTE", eng.isTrackMuted(t));
		paintToggleButton(g, r.soloBounds, "SOLO", eng.isTrackSoloed(t));
		paintArmButton(g, r.armBounds, eng.getArmedTrack() == t);

		g.setColour(eng.trackHasEvents(t) ? pal.seqTrackFilled : pal.seqTrackEmpty);
		g.fillRect(r.activityBounds.reduced(2.0f));

		// Far right: a bare digit/letter reminder of which part this is, independent of
		// whatever custom name is showing on the left (see promptForRenameTrack()).
		g.setColour(pal.seqInactiveText.withAlpha(0.7f));
		g.setFont(juce::FontOptions(juce::jlimit(9.0f, 14.0f, r.partNumberBounds.getHeight() * 0.5f)));
		g.drawText(isRhythm ? "R" : juce::String(t + 1), r.partNumberBounds, juce::Justification::centred);
	}
}

// The exact body a real right-click has always run - extracted verbatim so the long-press
// path added below (mouseDown()'s own comment explains why) reaches identically the same
// menus rather than a second, easily-drifting copy of this dispatch table.
void D110SequencerPanel::handleContextAction(juce::Point<float> p) {
	auto &eng = engine();
	if (tempoBounds.contains(p)) { promptForTempo(); return; }
	if (timeSigBounds.contains(p)) { showTimeSignatureMenu(); return; }
	if (stepDurationBounds.contains(p)) { showStepDurationMenu(); return; }
	if (recModeBounds.contains(p)) { showRecordModeMenu(); return; }
	if (metronomeBounds.contains(p)) { showMetronomeModeMenu(); return; }
	if (loadBounds.contains(p)) { showLoadMenu(); return; }
	if (saveBounds.contains(p)) { showSaveMenu(); return; }
	if (undoBounds.contains(p)) { showUndoRedoInfo(true); return; }
	if (redoBounds.contains(p)) { showUndoRedoInfo(false); return; }
	if (processor.supportsCaptureLivePatch() && resyncBounds.contains(p)) { showResyncInfo(); return; }
	if (barReadoutBounds.contains(p)) { showBarMenu(); return; }
	if (barPrevBounds.contains(p)) { eng.gotoBar(1); repaint(); return; }
	if (barNextBounds.contains(p)) { eng.gotoBar(eng.getBarCount()); repaint(); return; }
	if (stopBounds.contains(p)) { d110seq::showTransportMenu(processor); return; }
	if (playBounds.contains(p)) {
		d110seq::showTransportMenu(processor, [this] { engine().gotoBar(1); engine().play(); repaint(); });
		return;
	}
	if (processor.supportsExtraTracks() && extraTracksZoneBounds.contains(p)) {
		showExtraTracksMenu();
		return;
	}
	for (int rowIdx = 0; rowIdx < rowsOnCurrentPage(); ++rowIdx) {
		const int t = trackForRow(rowIdx);
		if (rows[static_cast<size_t>(t)].rowBounds.contains(p)) { showQuantizeMenu(t); return; }
	}
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s)
		if (slotBounds[static_cast<size_t>(s)].contains(p)) { showCopySongMenu(s); return; }
}

void D110SequencerPanel::mouseDown(const juce::MouseEvent &e) {
	auto &eng = engine();
	const auto p = e.position;

	if (e.mods.isPopupMenu()) { handleContextAction(p); return; }

	// Touchscreens have no right mouse button - a long press (~500ms, roughly stationary)
	// reaches the exact same menus handleContextAction() above already gives a real
	// right-click. The ordinary tap action below still fires immediately either way (a quick
	// tap is exactly as responsive as it always was); only a genuine HOLD additionally opens
	// a menu on top of it. longPressToken invalidates this if the touch releases or moves
	// (mouseUp/mouseDrag bump it) before the delay elapses, and SafePointer covers the
	// component being destroyed outright in the meantime (e.g. Android's own tab switch).
	longPressStartPos = p;
	const int token = ++longPressToken;
	juce::Component::SafePointer<D110SequencerPanel> safeThis(this);
	juce::Timer::callAfterDelay(500, [safeThis, token, p] {
		auto *self = safeThis.getComponent();
		if (self == nullptr || token != self->longPressToken) return;
		self->handleContextAction(p);
	});

	if (recModeBounds.contains(p)) { cycleRecordMode(); repaint(); return; }
	if (newBounds.contains(p)) { confirmNewSong(); return; }
	if (undoBounds.contains(p)) {
		if (eng.canUndo()) eng.undo();
		repaint();
		return;
	}
	if (redoBounds.contains(p)) {
		if (eng.canRedo()) eng.redo();
		repaint();
		return;
	}
	// SYNC is D-110 only (see resyncBounds' own layout comment) - Nonet Sequencer has no live
	// patch, so this button doesn't exist there at all and resyncBounds is never clickable.
	if (processor.supportsCaptureLivePatch() && resyncBounds.contains(p)) {
		juce::PopupMenu m;
		m.addItem(1, "Send stored settings to patch now");
		m.addItem(2, "Capture patch into song...");
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
			[this](int result) {
				if (result == 1) processor.resyncProgramChanges();
				else if (result == 2) confirmCaptureLivePatch();
			});
		return;
	}
	if (stepBounds.contains(p)) {
		if (eng.isStepRecording()) eng.stopStepRecording();
		else if (eng.getArmedTrack() >= 0) eng.startStepRecording();
		repaint();
		return;
	}
	if (stepDurationBounds.contains(p)) { cycleStepDuration(); repaint(); return; }
	if (stepDotBounds.contains(p)) { eng.setStepDotted(!eng.getStepDotted()); repaint(); return; }
	if (restBounds.contains(p)) {
		if (eng.isStepRecording()) eng.stepRest();
		repaint();
		return;
	}
	if (backBounds.contains(p)) {
		if (eng.isStepRecording()) eng.stepBack();
		repaint();
		return;
	}
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s)
		if (slotBounds[static_cast<size_t>(s)].contains(p)) { eng.selectSongSlot(s); repaint(); return; }

	if (loadBounds.contains(p)) {
		// Bump the token NOW, not just on the mouseUp() this tap would ordinarily get - on
		// Android, launchAsync() below hands off to a real system Activity (the SAF picker),
		// and this window never sees that finger lift. Left alone, the long-press timer
		// scheduled above still fires 500ms later and calls handleContextAction() -> a SECOND,
		// competing juce::FileChooser (showLoadMenu()'s .midiseq one) while this one is still
		// awaiting its result - JUCE's Android FileChooser only supports one in flight at a
		// time, so the second one breaks the first's own callback and the file you picked
		// never actually loads. Alan hit exactly this, 2026-08-22.
		++longPressToken;
		// "*.MID" alongside "*.mid" because Linux's native picker (zenity/kdialog) matches
		// filters with a case-sensitive glob - see PluginEditor.cpp's SysEx-import chooser
		// (TieredNativeFileChooser) for the fuller explanation and its own "*.*" fallback,
		// deliberately not duplicated here to keep this dialog's default filter narrow.
		auto *chooser = new juce::FileChooser("Load a MIDI file into the sequencer", processor.getLastDialogDir(), "*.mid;*.MID");
		chooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
			[this, chooser](const juce::FileChooser &fc) {
				const auto url = fc.getURLResult();
				if (url != juce::URL())
					withLocalFileForLoad(url, [this](const juce::File &file) {
						processor.setLastDialogDir(file.getParentDirectory());
						engine().loadMidiFile(file);
					});
				delete chooser;
				repaint();
			});
		return;
	}
	if (saveBounds.contains(p)) {
		++longPressToken; // see loadBounds' own comment just above
		auto *chooser = new juce::FileChooser("Save the sequencer as a MIDI file", processor.getLastDialogDir(), "*.mid;*.MID");
		chooser->launchAsync(
			juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
				| juce::FileBrowserComponent::warnAboutOverwriting,
			[this, chooser](const juce::FileChooser &fc) {
				const auto url = fc.getURLResult();
				if (url != juce::URL())
					withLocalFileForSave(url, "mid", [this](const juce::File &file) {
						processor.setLastDialogDir(file.getParentDirectory());
						engine().saveMidiFile(file);
					});
				delete chooser;
			});
		return;
	}

	// STOP no longer just halts the transport - renderInto() stops walking the event
	// sequence the instant playing flips false, so a note-off scheduled later than the
	// stop point was previously never emitted, leaving that voice stuck audible (and
	// stuck showing in the Monitor tab) until someone found the right-click panic below.
	// A plain MIDI panic here covers it the same way right-click STOP always has.
	if (stopBounds.contains(p)) { eng.stop(); processor.midiPanic(); repaint(); return; }
	if (playBounds.contains(p)) { eng.play(); repaint(); return; }
	if (recBounds.contains(p)) {
		if (eng.isRecording()) eng.stopRecording();
		else if (eng.getArmedTrack() >= 0) eng.startRecording();
		repaint();
		return;
	}
	if (timeSigBounds.contains(p)) { cycleTimeSignature(); repaint(); return; }
	if (metronomeBounds.contains(p)) { eng.setMetronomeEnabled(!eng.getMetronomeEnabled()); repaint(); return; }
	if (precountBounds.contains(p)) {
		// 1st click = 1 bar, 2nd = 2 bars, 3rd = off, then back to 1 - see setPrecountBars().
		eng.setPrecountBars((eng.getPrecountBars() + 1) % 3);
		repaint();
		return;
	}
	if (loopBounds.contains(p)) { cycleLoopMode(); repaint(); return; }
	if (barPrevBounds.contains(p)) { eng.gotoBar(juce::jmax(1, eng.getCurrentBar() - 1)); repaint(); return; }
	if (barNextBounds.contains(p)) { eng.gotoBar(eng.getCurrentBar() + 1); repaint(); return; }
	if (barReadoutBounds.contains(p)) {
		draggingBar = true;
		barDragStartY = p.y;
		barDragStartValue = eng.getCurrentBar();
		return;
	}
	if (tempoBounds.contains(p)) {
		draggingTempo = true;
		tempoDragStartY = p.y;
		tempoDragStartValue = eng.getTempo();
		return;
	}
	if (barMenuBounds.contains(p)) { showBarMenu(); return; }
	if (onModeButton && modeBounds.contains(p)) { onModeButton(); return; }
	if (processor.supportsExtraTracks() && eng.getExtraTracksEnabled()) {
		// layout(), not just repaint(): rows[] only gets the OTHER page's bounds computed
		// once layout() actually runs over it - a bare repaint() would paint page 1's rows
		// with whatever stale (likely zero-sized, never laid out) bounds they last had.
		if (trackPage1Bounds.contains(p)) { trackPage = 0; layout(); repaint(); return; }
		if (trackPage2Bounds.contains(p)) { trackPage = 1; layout(); repaint(); return; }
	}

	for (int rowIdx = 0; rowIdx < rowsOnCurrentPage(); ++rowIdx) {
		const int t = trackForRow(rowIdx);
		const auto &r = rows[static_cast<size_t>(t)];
		if (r.muteBounds.contains(p)) { eng.setTrackMuted(t, !eng.isTrackMuted(t)); repaint(); return; }
		if (r.soloBounds.contains(p)) { eng.setTrackSoloed(t, !eng.isTrackSoloed(t)); repaint(); return; }
		if (r.armBounds.contains(p)) { eng.armTrack(eng.getArmedTrack() == t ? -1 : t); repaint(); return; }
		if (r.channelReadout.contains(p) && processor.supportsTrackChannelEdit()) { showTrackChannelMenu(t); return; }
	}
}

void D110SequencerPanel::mouseDrag(const juce::MouseEvent &e) {
	// A real drag (dragging the tempo/bar value, or just an inaccurate tap) isn't a long
	// press - see mouseDown()'s own comment on longPressToken.
	if (e.position.getDistanceFrom(longPressStartPos) > 10.0f) ++longPressToken;

	if (draggingTempo) {
		// Dragging up raises the tempo, down lowers it - same sense as the panel's own VALUE
		// fields (D110EditorPane's Cell drag), just not sharing their code since this drawer
		// isn't built out of Cells.
		const float dy = tempoDragStartY - e.position.y;
		engine().setTempo(tempoDragStartValue + double(dy) * 0.5);
		repaint();
		return;
	}
	if (draggingBar) {
		// Same drag sense as the tempo field, coarser step (a few pixels per bar rather than
		// per BPM) since bars are a small integer range, not a wide continuous one.
		const float dy = barDragStartY - e.position.y;
		const int bar = barDragStartValue + juce::roundToInt(dy / 6.0f);
		engine().gotoBar(juce::jmax(1, bar));
		repaint();
	}
}

void D110SequencerPanel::mouseUp(const juce::MouseEvent &) {
	++longPressToken; // released - see mouseDown()'s own comment on longPressToken
	draggingTempo = false;
	draggingBar = false;
}

void D110SequencerPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
	if (!tempoBounds.contains(e.position)) return;
	engine().setTempo(engine().getTempo() + (wheel.deltaY > 0 ? 1.0 : -1.0));
	repaint();
}
