#include "D110SequencerGridPanel.h"
#include "SequencerTransportMenu.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "../AppModeIcons.h"
#include "../UiTheme.h"

using d110seq::D110SequencerEngine;
using d110seq::QuantizeGrid;

namespace {
constexpr float kDefaultRowH = 14.0f; // one pitch row, desktop default
constexpr float kKeysW = 46.0f;       // piano-key column
// Row heights the ROW button steps through - the last two are meant for fingers.
const float rowHeights[] = { 14.0f, 20.0f, 28.0f, 38.0f };
constexpr float kBtnRowH = 24.0f;
constexpr float kPad = 4.0f;

struct GridDef {
	QuantizeGrid grid;
	const char *label;
	double beats;
};
// The step sizes offered - QuantizeGrid values, so the persisted meaning is the engine's own.
const GridDef kGridDefs[] = {
	{QuantizeGrid::quarter, "1/4", 1.0},
	{QuantizeGrid::eighth, "1/8", 0.5},
	{QuantizeGrid::eighthTriplet, "1/8T", 1.0 / 3.0},
	{QuantizeGrid::sixteenth, "1/16", 0.25},
	{QuantizeGrid::sixteenthTriplet, "1/16T", 1.0 / 6.0},
	{QuantizeGrid::thirtySecond, "1/32", 0.125},
	// GRID OFF: no snapping. The 1/16 columns stay on screen as a guide only.
	{QuantizeGrid::off, "OFF", 0.25},
};

const GridDef &gridDef(QuantizeGrid g) {
	for (const auto &d : kGridDefs)
		if (d.grid == g) return d;
	return kGridDefs[3];
}

bool isBlackKey(int note) {
	switch (note % 12) {
		case 1: case 3: case 6: case 8: case 10: return true;
		default: return false;
	}
}

juce::String noteName(int note) { return juce::MidiMessage::getMidiNoteName(note, true, true, 4); }

juce::String trackButtonLabel(int t) {
	return t == D110SequencerEngine::kRhythmTrack ? juce::String("R") : juce::String(t + 1);
}

juce::String trackLongLabel(const D110SequencerEngine &eng, int t) {
	const auto custom = eng.getTrackName(t);
	if (custom.isNotEmpty()) return custom;
	if (t == D110SequencerEngine::kRhythmTrack) return "RHYTHM";
	if (t < D110SequencerEngine::kNumTracks) return "PART " + juce::String(t + 1);
	return "TRACK " + juce::String(t + 1);
}

// 13 normally; raised by paint() for the Android portrait layout's taller rows.
float gButtonFontMax = 13.0f;

void paintButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active, bool enabled) {
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
	// Shrink the text to fit a narrow button (a phone in portrait squeezes the transport row's
	// fractional columns to a few dozen pixels) instead of letting it truncate to "S...".
	float size = juce::jlimit(8.0f, gButtonFontMax, b.getHeight() * 0.5f);
	const float avail = b.getWidth() - 8.0f;
	const float wide = float(juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(size)), label));
	if (wide > avail && avail > 0.0f) size = juce::jmax(6.5f, size * avail / wide);
	g.setFont(juce::FontOptions(size));
	g.drawText(label, b, juce::Justification::centred);
}
} // namespace

D110SequencerGridPanel::D110SequencerGridPanel(D110SequencerHost &p) : processor(p) {
	addAndMakeVisible(vBar);
	vBar.setRangeLimits(0.0, 128.0);
	vBar.setAutoHide(false);
	vBar.addListener(this);
	syncRowHeightFromHost();
	startTimerHz(20);
}

D110SequencerGridPanel::~D110SequencerGridPanel() {
	stopAudition();
	vBar.removeListener(this);
}

D110SequencerEngine &D110SequencerGridPanel::engine() const { return processor.getSequencer(); }

// ---------------------------------------------------------------- row height

void D110SequencerGridPanel::setRowHeight(float newRowH, bool persist) {
	if (std::abs(newRowH - rowH) < 0.5f) return;
	// Keep whichever pitch is in the middle of the view in the middle.
	const int centre = topNote - visibleRows() / 2;
	rowH = newRowH;
	if (persist) processor.setGridRowHeight(juce::roundToInt(newRowH));
	buildLayout();
	setTopNote(centre + visibleRows() / 2);
	repaint();
}

void D110SequencerGridPanel::cycleRowHeight() {
	float next = rowHeights[0];
	for (float h : rowHeights)
		if (h > rowH + 0.5f) { next = h; break; }
	setRowHeight(next, true);
}

// The host's persisted value can arrive after this panel was built (the Android app loads its
// state a moment after constructing its views), so it is re-read rather than trusted once.
void D110SequencerGridPanel::syncRowHeightFromHost() {
	const int stored = processor.getGridRowHeight();
	const float wanted = stored > 0 ? juce::jlimit(10.0f, 60.0f, float(stored)) : kDefaultRowH;
	if (std::abs(wanted - rowH) >= 0.5f) {
		rowH = wanted;
		if (getWidth() > 0) {
			buildLayout();
			setTopNote(topNote);
		}
	}
}

// ---------------------------------------------------------------- layout

void D110SequencerGridPanel::addButton(juce::Rectangle<float> bounds, std::function<juce::String()> label,
                                        std::function<void()> onClick, std::function<bool()> active,
                                        std::function<bool()> enabled, std::function<void()> onRightClick) {
	Button b;
	b.bounds = bounds;
	b.label = std::move(label);
	b.onClick = std::move(onClick);
	b.active = std::move(active);
	b.enabled = std::move(enabled);
	b.onRightClick = std::move(onRightClick);
	buttons.push_back(std::move(b));
}

void D110SequencerGridPanel::buildLayout() {
	buttons.clear();
	auto &eng = engine();
	builtTrackCount = eng.activeTrackCount();

	// ---- row 1: the same transport row, at the same columns and height, as D110SequencerPanel's
	// (see its layout()), so STOP/PLAY/REC, the tempo, the loop and the bar buttons stay where
	// the normal view has them when switching between the two. Only the bar-menu slot at the far
	// right is left empty.
	auto full = getLocalBounds().toFloat();
	// Android portrait (mode-switch button set, panel taller than wide): two taller rows instead of
	// one fine 13-column row - see D110SequencerPanel::layout()'s stacked branch for the same idea.
	//   1. STOP PLAY REC | BPM | SIG | METRO | PRECOUNT | mode | menu
	//   2. LOOP < BAR >
	stackedTransport = bool(onModeButton) && getHeight() > getWidth();
	auto transport = full.removeFromTop(stackedTransport ? 46.0f : juce::jmin(40.0f, full.getHeight() * 0.22f));
	juce::Rectangle<float> transport2;
	if (stackedTransport) {
		full.removeFromTop(2.0f);
		transport2 = full.removeFromTop(46.0f);
	}
	const float tw = transport.getWidth();
	// With a mode-switch button (onModeButton, Android only) every column but the last two
	// squeezes by `sq` to free one more 0.042 slot left of the bar-menu button.
	const float sq = onModeButton ? 0.954f : 1.0f;
	auto colA = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(transport.getX() + tw * frac, transport.getY(), tw * widthFrac - 4.0f,
		                               transport.getHeight());
	};
	auto colT = [&](float frac, float widthFrac) { return colA(frac * sq, widthFrac * sq); };
	// Stacked layout cells, indexed by the id each button passes to P() below.
	juce::Rectangle<float> cell[13];
	if (stackedTransport) {
		auto fill = [](juce::Rectangle<float> row, std::initializer_list<std::pair<int, float>> cols,
		               juce::Rectangle<float> *out) {
			float total = 0.0f;
			for (auto &c : cols) total += c.second;
			float x = row.getX();
			for (auto &c : cols) {
				const float cw = row.getWidth() * c.second / total;
				out[c.first] = juce::Rectangle<float>(x, row.getY(), cw - 4.0f, row.getHeight());
				x += cw;
			}
		};
		fill(transport, { { 0, 0.10f }, { 1, 0.10f }, { 2, 0.09f }, { 3, 0.15f }, { 4, 0.09f }, { 5, 0.11f },
		                  { 6, 0.17f }, { 11, 0.085f }, { 12, 0.085f } }, cell);
		fill(transport2, { { 7, 0.20f }, { 8, 0.14f }, { 9, 0.32f }, { 10, 0.14f } }, cell);
	}
	auto P = [&](int id, float frac, float widthFrac) { return stackedTransport ? cell[id] : colT(frac, widthFrac); };
	addButton(P(0, 0.000f, 0.060f), [] { return juce::String("STOP"); },
	          [this] { engine().stop(); processor.midiPanic(); }, [this] { return !engine().isPlaying(); },
	          {}, [this] { d110seq::showTransportMenu(processor); });
	addButton(P(1, 0.060f, 0.060f), [] { return juce::String("PLAY"); }, [this] { engine().play(); },
	          [this] { return engine().isPlaying() && !engine().isRecording(); },
	          {}, [this] { d110seq::showTransportMenu(processor); });
	// REC records into the track selected here (a normal panel arms tracks by hand; the grid
	// has no ARM button, so the selected track is the armed one).
	addButton(P(2, 0.120f, 0.060f), [] { return juce::String("REC"); },
	          [this] {
		          auto &e = engine();
		          if (e.isRecording()) { e.stopRecording(); return; }
		          e.armTrack(selTrack);
		          e.startRecording();
	          },
	          [this] { return engine().isRecording(); });
	tempoBounds = P(3, 0.185f, 0.100f);
	addButton(P(4, 0.288f, 0.064f),
	          [this] {
		          return juce::String(engine().getTimeSigNumerator()) + "/" + juce::String(engine().getTimeSigDenominator());
	          },
	          [this] { cycleTimeSignature(); }, {}, {}, [this] { showTimeSignatureMenu(); });
	addButton(P(5, 0.360f, 0.120f), [] { return juce::String("METRO"); },
	          [this] { engine().setMetronomeEnabled(!engine().getMetronomeEnabled()); },
	          [this] { return engine().getMetronomeEnabled(); });
	addButton(P(6, 0.485f, 0.130f),
	          [this] {
		          const int bars = engine().getPrecountBars();
		          return bars == 0 ? juce::String("PRECOUNT OFF") : "PRECOUNT " + juce::String(bars);
	          },
	          [this] { engine().setPrecountBars((engine().getPrecountBars() + 1) % 3); },
	          [this] { return engine().getPrecountBars() > 0; });
	addButton(P(7, 0.620f, 0.090f),
	          [this] {
		          switch (engine().getLoopMode()) {
			          case d110seq::LoopMode::off: return juce::String("LOOP OFF");
			          case d110seq::LoopMode::bar: return juce::String("LOOP: BAR");
			          case d110seq::LoopMode::punch: return juce::String("LOOP: PUNCH");
		          }
		          return juce::String();
	          },
	          [this] {
		          auto &e = engine();
		          switch (e.getLoopMode()) {
			          case d110seq::LoopMode::off: e.setLoopMode(d110seq::LoopMode::bar); break;
			          case d110seq::LoopMode::bar: e.setLoopMode(d110seq::LoopMode::punch); break;
			          case d110seq::LoopMode::punch: e.setLoopMode(d110seq::LoopMode::off); break;
		          }
	          },
	          [this] { return engine().getLoopMode() != d110seq::LoopMode::off; });
	// Same as the normal panel: a click steps one bar, a right-click jumps to the first/last bar.
	addButton(P(8, 0.715f, 0.045f), [] { return juce::String("<"); },
	          [this] { engine().gotoBar(juce::jmax(1, engine().getCurrentBar() - 1)); }, {}, {},
	          [this] { engine().gotoBar(1); });
	barReadoutBounds = P(9, 0.763f, 0.148f);
	addButton(P(10, 0.915f, 0.040f), [] { return juce::String(">"); },
	          [this] {
		          auto &e = engine();
		          // One empty bar past the end is reachable, so a song can be extended from here.
		          e.gotoBar(juce::jmin(e.getBarCount() + 1, e.getCurrentBar() + 1));
	          },
	          {}, {}, [this] { engine().gotoBar(engine().getBarCount()); });
	builtWithBarMenu = bool(onBarMenuButtonExtra);
	builtWithModeButton = bool(onModeButton);
	if (builtWithModeButton) {
		addButton(stackedTransport ? cell[11] : colA(0.915f, 0.042f), [] { return juce::String(); }, [this] { onModeButton(); });
		buttons.back().customPaint = [this](juce::Graphics &g, juce::Rectangle<float> b) {
			d110ui::paintAppModeButton(g, b, d110ui::AppMode(modeButtonMode ? modeButtonMode() : 1));
		};
	}
	if (builtWithBarMenu)
		addButton(stackedTransport ? cell[12] : colA(0.958f, 0.042f), [] { return juce::String::fromUTF8("\xe2\x98\xb0"); }, // U+2630
		          [this] { showBarMenu(); });

	auto area = full.reduced(kPad, 2.0f);
	auto row2 = area.removeFromTop(kBtnRowH);
	auto row3 = area.removeFromTop(kBtnRowH);
	area.removeFromTop(2.0f);
	auto take = [](juce::Rectangle<float> &row, float w) { return row.removeFromLeft(w); };

	// ---- row 2: track selector, then MUTE/SOLO for the selected track
	const int trackCount = eng.activeTrackCount();
	const float muteSoloW = 52.0f;
	auto tracksArea = row2.withTrimmedRight(3.0f * muteSoloW + 8.0f);
	const float trackW = juce::jmin(36.0f, tracksArea.getWidth() / float(juce::jmax(1, trackCount)));
	for (int t = 0; t < trackCount; ++t)
		addButton(take(tracksArea, trackW), [t] { return trackButtonLabel(t); }, [this, t] { selectTrack(t); },
		          [this, t] { return selTrack == t; });
	hintBounds = tracksArea.reduced(8.0f, 0.0f); // whatever the track buttons leave free
	auto right = row2.removeFromRight(3.0f * muteSoloW + 4.0f);
	// Pitch-row height: steps through the presets (bigger rows are for fingers).
	addButton(take(right, muteSoloW), [this] { return "ROW " + juce::String(juce::roundToInt(rowH)); },
	          [this] { cycleRowHeight(); });
	addButton(take(right, muteSoloW), [] { return juce::String("MUTE"); },
	          [this] { engine().setTrackMuted(selTrack, !engine().isTrackMuted(selTrack)); },
	          [this] { return engine().isTrackMuted(selTrack); });
	addButton(take(right, muteSoloW), [] { return juce::String("SOLO"); },
	          [this] { engine().setTrackSoloed(selTrack, !engine().isTrackSoloed(selTrack)); },
	          [this] { return engine().isTrackSoloed(selTrack); });

	// ---- row 3: step size / note length / velocity, the track name, then UNDO/REDO at the same
	// columns the normal view gives them, and a hint on whatever room is left
	auto colR = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(row3.getX() + row3.getWidth() * frac, row3.getY(),
		                               row3.getWidth() * widthFrac - 4.0f, row3.getHeight());
	};
	addButton(colR(0.000f, 0.125f), [this] { return "GRID " + juce::String(gridDef(gridChoice).label); },
	          [this] { showGridMenu(); });
	addButton(colR(0.130f, 0.095f), [this] { return "LEN " + juce::String(noteLenSteps); },
	          [this] { showLengthMenu(); });
	addButton(colR(0.230f, 0.095f), [this] { return "VEL " + juce::String(defaultVelocity); },
	          [this] { showVelocityMenu(); });
	// The 4 song slots, at the columns the normal view gives them (click to switch; the
	// copy-song and sound-snapshot menus stay in the normal view).
	for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
		slotBounds[size_t(slot)] = colR(0.335f + float(slot) * 0.048f, 0.044f);
		addButton(slotBounds[size_t(slot)], [slot] { return juce::String(slot + 1); },
		          [this, slot] { stopAudition(); engine().selectSongSlot(slot); },
		          [this, slot] { return engine().getCurrentSongSlot() == slot; });
	}
	addButton(colR(0.535f, 0.075f), [] { return juce::String("UNDO"); },
	          [this] { if (engine().canUndo()) engine().undo(); }, {}, [this] { return engine().canUndo(); });
	addButton(colR(0.615f, 0.075f), [] { return juce::String("REDO"); },
	          [this] { if (engine().canRedo()) engine().redo(); }, {}, [this] { return engine().canRedo(); });
	infoBounds = colR(0.700f, 0.300f);

	// ---- the editing area
	keysBounds = area.removeFromLeft(kKeysW);
	auto scrollArea = area.removeFromRight(juce::jmax(12.0f, rowH * 0.7f));
	// A squat drawer gives some of the velocity lane back to the pitch rows; bigger rows (touch)
	// get a taller lane so a stick's tip is a fair target.
	const float velH = juce::jlimit(24.0f, juce::jmax(46.0f, rowH * 2.2f), area.getHeight() * 0.22f);
	velBounds = area.removeFromBottom(velH);
	gridBounds = area;
	// The key column lines up with the grid rows; the space under it is the lane's label cell.
	keysBounds.setHeight(gridBounds.getHeight());
	vBar.setBounds(scrollArea.withHeight(gridBounds.getHeight()).toNearestInt());
}

void D110SequencerGridPanel::resized() {
	buildLayout();
	const int rows = visibleRows();
	if (!viewInitialised && gridBounds.getHeight() > 0.0f) {
		viewInitialised = true;
		setTopNote(65 + rows / 2);
		ensureNotesVisible(true);
	} else {
		setTopNote(topNote); // re-clamp against the new row count and refresh the scrollbar
	}
}

// ---------------------------------------------------------------- geometry

D110SequencerGridPanel::Geometry D110SequencerGridPanel::geometry() const {
	auto &eng = engine();
	Geometry g;
	g.bar = eng.getCurrentBar();
	g.barLen = eng.barLengthBeats();
	g.barStart = double(g.bar - 1) * g.barLen;
	g.stepBeats = gridDef(gridChoice).beats;
	g.free = gridChoice == QuantizeGrid::off;
	g.columns = juce::jmax(1, int(std::ceil(g.barLen / g.stepBeats - 1.0e-6)));
	g.validSteps = juce::jmax(1, int(std::floor(g.barLen / g.stepBeats + 1.0e-6)));
	const double beatUnit = 4.0 / double(eng.getTimeSigDenominator());
	g.stepsPerBeat = juce::jmax(1, juce::roundToInt(beatUnit / g.stepBeats));
	g.colW = gridBounds.getWidth() / float(g.columns);
	return g;
}

int D110SequencerGridPanel::stepAtX(float x, const Geometry &g) const {
	return juce::jlimit(0, g.columns - 1, int(std::floor((x - gridBounds.getX()) / g.colW)));
}

double D110SequencerGridPanel::beatAtX(float x, const Geometry &g) const {
	return g.barStart + double((x - gridBounds.getX()) / g.colW) * g.stepBeats;
}

float D110SequencerGridPanel::xForBeat(double beat, const Geometry &g) const {
	return gridBounds.getX() + float((beat - g.barStart) / g.stepBeats) * g.colW;
}

int D110SequencerGridPanel::visibleRows() const { return juce::jmax(1, int(gridBounds.getHeight() / rowH)); }

float D110SequencerGridPanel::yForNote(int note) const { return gridBounds.getY() + float(topNote - note) * rowH; }

int D110SequencerGridPanel::noteAtY(float y) const {
	return topNote - int(std::floor((y - gridBounds.getY()) / rowH));
}

void D110SequencerGridPanel::setTopNote(int newTop) {
	const int rows = visibleRows();
	topNote = juce::jlimit(juce::jmin(127, rows - 1), 127, newTop);
	vBar.setCurrentRange(double(127 - topNote), double(rows), juce::dontSendNotification);
	repaint();
}

void D110SequencerGridPanel::scrollBarMoved(juce::ScrollBar *, double newRangeStart) {
	setTopNote(127 - juce::roundToInt(newRangeStart));
}

void D110SequencerGridPanel::ensureNotesVisible(bool force) {
	const auto geo = geometry();
	int lo = 127, hi = 0;
	bool any = false;
	for (const auto &n : notesNear(geo)) {
		if (!isInBar(n, geo)) continue;
		lo = juce::jmin(lo, n.note);
		hi = juce::jmax(hi, n.note);
		any = true;
	}
	if (!any) return;
	const int rows = visibleRows();
	const int bottom = topNote - rows + 1;
	if (!force && hi >= bottom && lo <= topNote) return; // at least part of the bar is already in view
	setTopNote((lo + hi) / 2 + rows / 2);
}

// ---------------------------------------------------------------- notes

std::vector<D110SequencerGridPanel::NoteInfo> D110SequencerGridPanel::notesNear(const Geometry &g) const {
	std::vector<NoteInfo> out;
	const auto all = engine().eventsInBarRange(selTrack, juce::jmax(1, g.bar - 8), g.bar + 1);
	const double windowEnd = g.barStart + 2.0 * g.barLen;
	for (const auto &n : all) {
		if (n.startBeat >= windowEnd - 1.0e-9) continue;
		if (noteEnd(n, g) <= g.barStart + 1.0e-9) continue; // ended before this bar
		out.push_back(n);
	}
	return out;
}

juce::Rectangle<float> D110SequencerGridPanel::noteRect(const NoteInfo &n, const Geometry &g) const {
	const double barEndBeat = stepTime(g, g.columns);
	const float x1 = xForBeat(juce::jmax(n.startBeat, g.barStart), g);
	const float x2 = xForBeat(juce::jmin(noteEnd(n, g), barEndBeat), g);
	return { x1, yForNote(n.note), juce::jmax(4.0f, x2 - x1), rowH };
}

D110SequencerGridPanel::Hit D110SequencerGridPanel::hitNote(const std::vector<NoteInfo> &notes, const Geometry &g,
                                                             juce::Point<float> p) const {
	for (int i = int(notes.size()) - 1; i >= 0; --i) {
		const auto &n = notes[size_t(i)];
		if (!isEditable(n, g)) continue;
		const auto r = noteRect(n, g);
		if (r.contains(p)) return { i, p.x >= r.getRight() - juce::jmin(juce::jmax(7.0f, rowH * 0.45f), r.getWidth() * 0.4f) };
	}
	return {};
}

int D110SequencerGridPanel::hitVelocityStick(const std::vector<NoteInfo> &notes, const Geometry &g,
                                              juce::Point<float> p) const {
	int best = -1;
	float bestDist = 1.0e9f;
	for (int i = 0; i < int(notes.size()); ++i) {
		const auto &n = notes[size_t(i)];
		if (!isEditable(n, g)) continue;
		const float x = xForBeat(n.startBeat, g) + 1.0f;
		if (std::abs(p.x - x) > juce::jmax(6.0f, rowH * 0.45f)) continue;
		const float tipY = velBounds.getBottom() - 4.0f - float(n.velocity) / 127.0f * (velBounds.getHeight() - 8.0f);
		// Chords put several sticks at one x: take the one whose tip is nearest the pointer.
		const float d = std::abs(p.y - tipY);
		if (d < bestDist) { bestDist = d; best = i; }
	}
	return best;
}

bool D110SequencerGridPanel::overlapsSamePitch(const std::vector<NoteInfo> &notes, int ignoreIndex, int note,
                                                double start, double end) const {
	const auto geo = geometry();
	for (const auto &n : notes) {
		if (n.index == ignoreIndex || n.note != note) continue;
		if (start < noteEnd(n, geo) - 1.0e-9 && n.startBeat < end - 1.0e-9) return true;
	}
	return false;
}

double D110SequencerGridPanel::nextSamePitchStart(const std::vector<NoteInfo> &notes, int ignoreIndex, int note,
                                                   double start, double limit) const {
	double best = limit;
	for (const auto &n : notes) {
		if (n.index == ignoreIndex || n.note != note) continue;
		if (n.startBeat > start + 1.0e-9 && n.startBeat < best) best = n.startBeat;
	}
	return best;
}

double D110SequencerGridPanel::endForLength(const Geometry &g, double start, int lenSteps) const {
	const int s0 = juce::roundToInt((start - g.barStart) / g.stepBeats);
	if (std::abs(start - stepTime(g, s0)) < 1.0e-6) return stepTime(g, s0 + lenSteps);
	return start + double(lenSteps) * g.stepBeats;
}

bool D110SequencerGridPanel::gestureNoteStillValid(const std::vector<NoteInfo> &notes) {
	for (const auto &n : notes)
		if (n.index == gesture.engineIndex && n.note == gesture.curNote
		    && std::abs(n.startBeat - gesture.curStart) < 1.0e-9)
			return true;
	stopAudition();
	gesture = {};
	return false;
}

// ---------------------------------------------------------------- audition

void D110SequencerGridPanel::startAudition(int note, int velocity) {
	stopAudition();
	auditionNote = juce::jlimit(0, 127, note);
	processor.auditionTrackNote(selTrack, auditionNote, juce::jlimit(1, 127, velocity), true);
}

void D110SequencerGridPanel::stopAudition() {
	if (auditionNote < 0) return;
	processor.auditionTrackNote(selTrack, auditionNote, 0, false);
	auditionNote = -1;
}

// ---------------------------------------------------------------- menus / prompts

namespace {
struct TimeSig {
	int num, den;
};
// The same presets D110SequencerPanel offers, so the two views step through identical meters.
const TimeSig kTimeSigs[] = { {4, 4}, {3, 4}, {6, 8}, {2, 4}, {5, 4}, {7, 8} };
} // namespace

void D110SequencerGridPanel::cycleTimeSignature() {
	auto &eng = engine();
	size_t idx = 0;
	for (size_t i = 0; i < std::size(kTimeSigs); ++i)
		if (kTimeSigs[i].num == eng.getTimeSigNumerator() && kTimeSigs[i].den == eng.getTimeSigDenominator()) {
			idx = i;
			break;
		}
	idx = (idx + 1) % std::size(kTimeSigs);
	eng.setTimeSignature(kTimeSigs[idx].num, kTimeSigs[idx].den);
}

void D110SequencerGridPanel::showTimeSignatureMenu() {
	auto &eng = engine();
	juce::PopupMenu m;
	for (size_t i = 0; i < std::size(kTimeSigs); ++i)
		m.addItem(int(i) + 1, juce::String(kTimeSigs[i].num) + "/" + juce::String(kTimeSigs[i].den), true,
		          kTimeSigs[i].num == eng.getTimeSigNumerator() && kTimeSigs[i].den == eng.getTimeSigDenominator());
	juce::Component::SafePointer<D110SequencerGridPanel> safe(this);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe](int result) {
		if (safe == nullptr || result <= 0) return;
		safe->engine().setTimeSignature(kTimeSigs[result - 1].num, kTimeSigs[result - 1].den);
		safe->repaint();
	});
}

void D110SequencerGridPanel::selectTrack(int track) {
	stopAudition();
	selTrack = juce::jlimit(0, engine().activeTrackCount() - 1, track);
	ensureNotesVisible(true);
	repaint();
}

void D110SequencerGridPanel::showBarMenu() {
	juce::PopupMenu m;
	m.addItem("First bar", [this] { engine().gotoBar(1); repaint(); });
	m.addItem("Last bar", [this] { engine().gotoBar(engine().getBarCount()); repaint(); });
	if (onBarMenuButtonExtra) {
		m.addSeparator();
		onBarMenuButtonExtra(m);
	}
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void D110SequencerGridPanel::armLongPress(std::function<void()> action, juce::Point<float> at) {
	longPressStart = at;
	const int token = ++longPressToken;
	juce::Component::SafePointer<D110SequencerGridPanel> safe(this);
	juce::Timer::callAfterDelay(500, [safe, token, action = std::move(action)] {
		if (safe == nullptr || token != safe->longPressToken) return;
		action();
	});
}

void D110SequencerGridPanel::showGridMenu() {
	const double barLen = engine().barLengthBeats();
	juce::PopupMenu m;
	int id = 1;
	for (const auto &d : kGridDefs) {
		const int steps = int(std::floor(barLen / d.beats + 1.0e-6));
		m.addItem(id++,
		          d.grid == QuantizeGrid::off
		              ? juce::String("OFF   (free position, 1/16 guide lines)")
		              : juce::String(d.label) + "   (" + juce::String(steps) + " steps per bar)",
		          true, d.grid == gridChoice);
	}
	juce::Component::SafePointer<D110SequencerGridPanel> safe(this);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe](int result) {
		if (safe == nullptr || result <= 0) return;
		safe->gridChoice = kGridDefs[result - 1].grid;
		safe->noteLenSteps = 1;
		safe->repaint();
	});
}

void D110SequencerGridPanel::showLengthMenu() {
	juce::PopupMenu m;
	static const int lengths[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
	for (int len : lengths)
		m.addItem(len, juce::String(len) + (len == 1 ? " step" : " steps"), true, len == noteLenSteps);
	juce::Component::SafePointer<D110SequencerGridPanel> safe(this);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe](int result) {
		if (safe == nullptr || result <= 0) return;
		safe->noteLenSteps = result;
		safe->repaint();
	});
}

void D110SequencerGridPanel::showVelocityMenu() {
	juce::PopupMenu m;
	static const int velocities[] = { 127, 112, 100, 90, 80, 64, 48, 32, 16 };
	for (int v : velocities) m.addItem(v, juce::String(v), true, v == defaultVelocity);
	juce::Component::SafePointer<D110SequencerGridPanel> safe(this);
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe](int result) {
		if (safe == nullptr || result <= 0) return;
		safe->defaultVelocity = result;
		safe->repaint();
	});
}

void D110SequencerGridPanel::promptForTempo() {
	auto *aw = new juce::AlertWindow("Set tempo", "Enter a tempo in BPM (20-300).", juce::AlertWindow::NoIcon);
	aw->addTextEditor("bpm", juce::String(engine().getTempo(), 1), "BPM:");
	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	juce::Component::SafePointer<D110SequencerGridPanel> safe(this);
	aw->enterModalState(true, juce::ModalCallbackFunction::create([safe, aw](int result) {
		if (result == 1 && safe != nullptr) {
			const double bpm = aw->getTextEditorContents("bpm").getDoubleValue();
			if (bpm > 0.0) safe->engine().setTempo(bpm);
			safe->repaint();
		}
		delete aw; // enterModalState(true, ...) does not delete on its own when a callback is supplied
	}), false);
}

// ---------------------------------------------------------------- timer

void D110SequencerGridPanel::timerCallback() {
	if (!isShowing()) return;
	auto &eng = engine();
	syncRowHeightFromHost();
	if (eng.activeTrackCount() != builtTrackCount || bool(onBarMenuButtonExtra) != builtWithBarMenu
	    || bool(onModeButton) != builtWithModeButton
	    || stackedTransport != (bool(onModeButton) && getHeight() > getWidth())) {
		if (selTrack >= eng.activeTrackCount()) selTrack = 0;
		buildLayout();
	}
	const int bar = eng.getCurrentBar();
	if (bar != lastBar) {
		lastBar = bar;
		ensureNotesVisible(false); // a bar with notes, none in view: bring them in
	}
	repaint();
}

// ---------------------------------------------------------------- painting

void D110SequencerGridPanel::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	auto &eng = engine();
	g.fillAll(pal.panelBg);

	gButtonFontMax = stackedTransport ? 17.0f : 13.0f;
	for (const auto &b : buttons) {
		if (b.customPaint) b.customPaint(g, b.bounds);
		else paintButton(g, b.bounds, b.label(), b.active && b.active(), !b.enabled || b.enabled());
	}

	// Tempo (drag / wheel / right-click) and the bar readout, styled like every other button.
	paintButton(g, tempoBounds, juce::String(eng.getTempo(), 1) + " BPM", false, true);
	const auto geo = geometry();
	paintButton(g, barReadoutBounds, "BAR " + juce::String(geo.bar) + "/" + juce::String(eng.getBarCount()),
	            eng.isPrecounting(), true);

	g.setColour(pal.label);
	g.setFont(juce::FontOptions(12.0f));
	g.drawText(trackLongLabel(eng, selTrack) + "  -  ch " + juce::String(eng.channelForTrack(selTrack)), infoBounds,
	           juce::Justification::centredLeft);
	if (hintBounds.getWidth() > 300.0f) {
		g.setColour(pal.dim);
		g.setFont(juce::FontOptions(10.0f));
		g.drawText("click: add/remove   drag: move   edge: resize   right-click: delete   middle: free move", hintBounds,
		           juce::Justification::centredLeft);
	}

	// A dot on every song slot that holds something, same as the normal view.
	for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot)
		if (eng.songSlotHasContent(slot)) {
			const auto &sb = slotBounds[size_t(slot)];
			g.setColour(pal.seqActiveFill);
			g.fillEllipse(sb.getRight() - 8.0f, sb.getY() + 3.0f, 4.0f, 4.0f);
		}

	if (gridBounds.isEmpty()) return;

	// ---- grid background: pitch rows, then beat groups, then step lines
	g.saveState();
	g.reduceClipRegion(gridBounds.toNearestInt());
	const int rows = visibleRows() + 1;
	for (int r = 0; r < rows; ++r) {
		const int note = topNote - r;
		if (note < 0) break;
		const juce::Rectangle<float> row(gridBounds.getX(), yForNote(note), gridBounds.getWidth(), rowH);
		g.setColour(isBlackKey(note) ? pal.box.darker(0.3f) : pal.box);
		g.fillRect(row);
		if (note % 12 == 0) {
			g.setColour(pal.boxBorder);
			g.drawHorizontalLine(int(row.getBottom()) - 1, row.getX(), row.getRight());
		}
	}
	const float gx = gridBounds.getX();
	for (int c = 0; c < geo.columns; c += geo.stepsPerBeat * 2) {
		const int endCol = juce::jmin(geo.columns, c + geo.stepsPerBeat * 2);
		// Every other beat group slightly lighter, the classic step-sequencer stripe.
		g.setColour(pal.dim.withAlpha(0.07f));
		g.fillRect(gx + float(c + geo.stepsPerBeat) * geo.colW, gridBounds.getY(),
		           float(juce::jmin(geo.stepsPerBeat, endCol - c - geo.stepsPerBeat)) * geo.colW,
		           gridBounds.getHeight());
	}
	for (int c = 0; c <= geo.columns; ++c) {
		const float x = gx + float(c) * geo.colW;
		const bool beat = c % geo.stepsPerBeat == 0;
		g.setColour(beat ? pal.boxBorder : pal.boxBorder.withAlpha(0.35f));
		g.drawVerticalLine(int(x), gridBounds.getY(), gridBounds.getBottom());
	}
	if (geo.validSteps < geo.columns) { // the partial last column (odd meter/grid combination)
		g.setColour(pal.panelBg.withAlpha(0.6f));
		g.fillRect(gx + float(geo.validSteps) * geo.colW, gridBounds.getY(),
		           float(geo.columns - geo.validSteps) * geo.colW, gridBounds.getHeight());
	}

	// ---- notes
	const auto notes = notesNear(geo);
	for (const auto &n : notes) {
		if (!isInBar(n, geo)) continue;
		const auto r = noteRect(n, geo).reduced(0.0f, 1.0f);
		const bool ghost = !isEditable(n, geo);
		const float bright = 0.45f + 0.55f * float(n.velocity) / 127.0f;
		g.setColour(pal.value.withAlpha(ghost ? 0.35f : bright));
		g.fillRect(r);
		g.setColour(pal.value.darker(0.5f));
		g.drawRect(r, 1.0f);
		if (!ghost && r.getWidth() > 34.0f) {
			g.setColour(pal.panelBg);
			g.setFont(juce::FontOptions(juce::jlimit(9.5f, 15.0f, rowH * 0.68f)));
			g.drawText(noteName(n.note), r.reduced(2.0f, 0.0f), juce::Justification::centredLeft);
		}
	}

	// ---- playhead
	if (eng.isPlaying()) {
		const double pos = eng.getPositionBeats();
		if (pos >= geo.barStart && pos < geo.barStart + geo.barLen) {
			g.setColour(pal.seqMetroDownbeat);
			g.fillRect(xForBeat(pos, geo) - 1.0f, gridBounds.getY(), 2.0f, gridBounds.getHeight());
		}
	}
	g.restoreState();

	// ---- piano keys column
	g.saveState();
	g.reduceClipRegion(keysBounds.toNearestInt());
	for (int r = 0; r < rows; ++r) {
		const int note = topNote - r;
		if (note < 0) break;
		const juce::Rectangle<float> key(keysBounds.getX(), yForNote(note), keysBounds.getWidth(), rowH);
		const bool black = isBlackKey(note);
		g.setColour(note == auditionNote ? pal.value : (black ? pal.keyBlack : pal.keyWhite));
		g.fillRect(key);
		g.setColour(pal.keyWhiteBorder);
		g.drawRect(key, 0.5f);
		if (note % 12 == 0 || note == auditionNote) {
			g.setColour(black ? pal.keyWhite : pal.keyCaption);
			g.setFont(juce::FontOptions(juce::jlimit(9.5f, 15.0f, rowH * 0.68f)));
			g.drawText(noteName(note), key.reduced(3.0f, 0.0f), juce::Justification::centredRight);
		}
	}
	g.restoreState();

	// ---- velocity lane
	g.setColour(pal.box);
	g.fillRect(velBounds);
	g.setColour(pal.boxBorder);
	g.drawRect(velBounds, 1.0f);
	g.setColour(pal.dim);
	g.setFont(juce::FontOptions(10.0f));
	g.drawText("VEL", juce::Rectangle<float>(kPad, velBounds.getY(), kKeysW, velBounds.getHeight()),
	           juce::Justification::centred);
	g.saveState();
	g.reduceClipRegion(velBounds.toNearestInt());
	for (const auto &n : notes) {
		if (!isEditable(n, geo)) continue;
		const float x = xForBeat(n.startBeat, geo) + 1.0f;
		const float tipY = velBounds.getBottom() - 4.0f - float(n.velocity) / 127.0f * (velBounds.getHeight() - 8.0f);
		g.setColour(pal.value);
		g.fillRect(x - 1.0f, tipY, 2.0f, velBounds.getBottom() - 4.0f - tipY);
		g.fillEllipse(x - 3.0f, tipY - 3.0f, 6.0f, 6.0f);
	}
	if (eng.isPlaying()) {
		const double pos = eng.getPositionBeats();
		if (pos >= geo.barStart && pos < geo.barStart + geo.barLen) {
			g.setColour(pal.seqMetroDownbeat);
			g.fillRect(xForBeat(pos, geo) - 1.0f, velBounds.getY(), 2.0f, velBounds.getHeight());
		}
	}
	g.restoreState();
}

// ---------------------------------------------------------------- mouse

void D110SequencerGridPanel::mouseDown(const juce::MouseEvent &e) {
	gesture = {};
	const auto p = e.position;
	auto &eng = engine();

	++longPressToken; // any earlier press's pending long-press is void
	for (const auto &b : buttons) {
		if (!b.bounds.contains(p)) continue;
		if (b.enabled && !b.enabled()) return;
		if (e.mods.isPopupMenu()) {
			if (b.onRightClick) b.onRightClick();
		} else if (b.onClick) {
			if (b.onRightClick) armLongPress(b.onRightClick, p); // touch: hold = right-click
			b.onClick();
		}
		repaint();
		return;
	}

	if (tempoBounds.contains(p)) {
		if (e.mods.isPopupMenu()) { promptForTempo(); return; }
		armLongPress([this] { promptForTempo(); }, p);
		gesture.kind = GestureKind::tempoDrag;
		gesture.downPos = p;
		gesture.tempoStart = eng.getTempo();
		return;
	}

	if (keysBounds.contains(p)) {
		const int note = noteAtY(p.y);
		if (note < 0 || note > 127) return;
		gesture.kind = GestureKind::keyAudition;
		gesture.downPos = p;
		gesture.scrollStartTop = topNote;
		startAudition(note, defaultVelocity);
		repaint();
		return;
	}

	const auto geo = geometry();

	if (velBounds.contains(p)) {
		if (e.mods.isPopupMenu()) return;
		const auto notes = notesNear(geo);
		const int hit = hitVelocityStick(notes, geo, p);
		if (hit < 0) return;
		const auto &n = notes[size_t(hit)];
		gesture.kind = GestureKind::velocity;
		gesture.engineIndex = n.index;
		gesture.curStart = n.startBeat;
		gesture.curEnd = noteEnd(n, geo);
		gesture.curNote = n.note;
		gesture.curVelocity = n.velocity;
		gesture.downPos = p;
		mouseDrag(e); // a plain press already sets the value under the pointer
		return;
	}

	if (!gridBounds.contains(p)) return;
	const auto notes = notesNear(geo);
	const auto hit = hitNote(notes, geo, p);

	if (e.mods.isPopupMenu()) {
		if (hit.listIndex >= 0) {
			eng.pushUndoSnapshot("Grid: delete note");
			eng.deleteNoteEvent(selTrack, notes[size_t(hit.listIndex)].index);
			repaint();
		}
		return;
	}

	if (hit.listIndex >= 0) {
		// Not modified yet: a plain click removes the note on release, a drag moves/resizes it.
		const auto &n = notes[size_t(hit.listIndex)];
		gesture.kind = GestureKind::pendingNote;
		gesture.engineIndex = n.index;
		gesture.curStart = n.startBeat;
		gesture.curEnd = noteEnd(n, geo);
		gesture.curNote = n.note;
		gesture.curVelocity = n.velocity;
		gesture.downPos = p;
		gesture.onRightEdge = hit.onRightEdge;
		gesture.startStep = juce::roundToInt((n.startBeat - geo.barStart) / geo.stepBeats);
		gesture.grabStepOffset = stepAtX(p.x, geo) - gesture.startStep;
		gesture.grabOffsetBeats = beatAtX(p.x, geo) - n.startBeat;
		startAudition(n.note, n.velocity);
		if (e.mods.isMiddleButtonDown()) {
			// Middle button: move (or, on the right edge, resize) with no snapping whatever GRID
			// says, and no delete-on-click - nothing happens unless the pointer actually moves.
			gesture.freeOverride = true;
			gesture.kind = hit.onRightEdge ? GestureKind::resizeNote : GestureKind::moveNote;
		}
		return;
	}

	// Empty cell: create a note there (the middle button only ever grabs existing notes).
	if (e.mods.isMiddleButtonDown()) return;
	const int step = stepAtX(p.x, geo);
	const int note = noteAtY(p.y);
	if (note < 0 || note > 127) return;
	const double barEndBeat = geo.barStart + geo.barLen;
	double start, end;
	if (geo.free) {
		start = juce::jlimit(geo.barStart, barEndBeat - kMinFreeLen, roundToTick(beatAtX(p.x, geo)));
		if (overlapsSamePitch(notes, -1, note, start, start + 1.0e-6)) return;
		end = juce::jmin(roundToTick(start + double(noteLenSteps) * geo.stepBeats), barEndBeat);
		end = juce::jmin(end, nextSamePitchStart(notes, -1, note, start, end));
		if (end - start < kMinFreeLen - 1.0e-9) return;
	} else {
		if (step >= geo.validSteps) return;
		start = stepTime(geo, step);
		if (overlapsSamePitch(notes, -1, note, start, start + 1.0e-6)) return; // sits on a ghost/other note's tail
		int len = juce::jmin(noteLenSteps, geo.validSteps - step);
		end = stepTime(geo, step + len);
		const double limit = nextSamePitchStart(notes, -1, note, start, end);
		if (limit < end - 1.0e-9) {
			len = int(std::floor((limit - start) / geo.stepBeats + 1.0e-6));
			if (len < 1) return;
			end = stepTime(geo, step + len);
		}
	}
	eng.pushUndoSnapshot("Grid: add note");
	const int idx = eng.addNote(selTrack, start, end, note, defaultVelocity);
	if (idx < 0) return;
	gesture.kind = GestureKind::newNote;
	gesture.engineIndex = idx;
	gesture.curStart = start;
	gesture.curEnd = end;
	gesture.curNote = note;
	gesture.curVelocity = defaultVelocity;
	gesture.startStep = step;
	gesture.checkpointed = true;
	gesture.changed = true;
	gesture.downPos = p;
	startAudition(note, defaultVelocity);
	repaint();
}

// Shared by newNote and resizeNote: the cell under the pointer becomes the note's last step.
void D110SequencerGridPanel::resizeGestureNoteTo(float x) {
	auto &eng = engine();
	const auto geo = geometry();
	const auto notes = notesNear(geo);
	if (!gestureNoteStillValid(notes)) return;
	double end;
	const double limit = nextSamePitchStart(notes, gesture.engineIndex, gesture.curNote, gesture.curStart, 1.0e18);
	if (geo.free || gesture.freeOverride) {
		const double barEndBeat = geo.barStart + geo.barLen;
		end = juce::jlimit(gesture.curStart + kMinFreeLen, juce::jmax(gesture.curStart + kMinFreeLen, barEndBeat),
		                   roundToTick(beatAtX(x, geo)));
		end = juce::jmin(end, limit);
		if (end - gesture.curStart < kMinFreeLen - 1.0e-9) return;
	} else {
		int len = juce::jmax(1, stepAtX(x, geo) + 1 - gesture.startStep);
		len = juce::jmin(len, geo.validSteps - gesture.startStep);
		if (len < 1) return;
		end = endForLength(geo, gesture.curStart, len);
		if (end > limit + 1.0e-9) {
			len = int(std::floor((limit - gesture.curStart) / geo.stepBeats + 1.0e-6));
			if (len < 1) return;
			end = endForLength(geo, gesture.curStart, len);
		}
	}
	if (std::abs(end - gesture.curEnd) < 1.0e-9) return;
	if (!gesture.checkpointed) { eng.pushUndoSnapshot("Grid: resize note"); gesture.checkpointed = true; }
	const int idx = eng.updateNoteEvent(selTrack, gesture.engineIndex, gesture.curStart, end, gesture.curNote,
	                                    gesture.curVelocity);
	if (idx < 0) return;
	gesture.engineIndex = idx;
	gesture.curEnd = end;
	gesture.changed = true;
	repaint();
}

void D110SequencerGridPanel::mouseDrag(const juce::MouseEvent &e) {
	const auto p = e.position;
	auto &eng = engine();
	if (p.getDistanceFrom(longPressStart) > 10.0f) ++longPressToken;

	switch (gesture.kind) {
		case GestureKind::tempoDrag:
			eng.setTempo(gesture.tempoStart + std::round((gesture.downPos.y - p.y) * 0.5f));
			repaint();
			return;

		case GestureKind::velocity: {
			const auto notes = notesNear(geometry());
			if (!gestureNoteStillValid(notes)) return;
			const float span = velBounds.getHeight() - 8.0f;
			const int vel = juce::jlimit(
			    1, 127, juce::roundToInt((velBounds.getBottom() - 4.0f - p.y) / span * 127.0f));
			if (vel == gesture.curVelocity) return;
			if (!gesture.checkpointed) { eng.pushUndoSnapshot("Grid: velocity"); gesture.checkpointed = true; }
			const int idx = eng.updateNoteEvent(selTrack, gesture.engineIndex, gesture.curStart, gesture.curEnd,
			                                    gesture.curNote, vel);
			if (idx < 0) return;
			gesture.engineIndex = idx;
			gesture.curVelocity = vel;
			gesture.changed = true;
			repaint();
			return;
		}

		case GestureKind::newNote:
		case GestureKind::resizeNote:
			if (gesture.freeOverride && !gesture.changed && p.getDistanceFrom(gesture.downPos) < 3.0f) return;
			resizeGestureNoteTo(p.x);
			return;

		case GestureKind::pendingNote:
			// A wobble of a couple of pixels stays a click; past that it's a move or a resize.
			if (p.getDistanceFrom(gesture.downPos) < 4.0f) return;
			gesture.kind = gesture.onRightEdge ? GestureKind::resizeNote : GestureKind::moveNote;
			if (gesture.kind == GestureKind::resizeNote) { resizeGestureNoteTo(p.x); return; }
			[[fallthrough]];

		case GestureKind::moveNote: {
			const auto geo = geometry();
			const auto notes = notesNear(geo);
			if (!gestureNoteStillValid(notes)) return;
			// A middle-click wobble of a pixel or two is not a move.
			if (gesture.freeOverride && !gesture.changed && p.getDistanceFrom(gesture.downPos) < 3.0f) return;
			const int note = juce::jlimit(0, 127, noteAtY(p.y));
			const double dur = gesture.curEnd - gesture.curStart;
			double start, end;
			if (geo.free || gesture.freeOverride) {
				start = juce::jlimit(geo.barStart, geo.barStart + geo.barLen - kMinFreeLen,
				                     roundToTick(beatAtX(p.x, geo) - gesture.grabOffsetBeats));
				end = roundToTick(start + dur);
			} else {
				const int step = juce::jlimit(0, geo.validSteps - 1, stepAtX(p.x, geo) - gesture.grabStepOffset);
				start = stepTime(geo, step);
				const double lenSteps = dur / geo.stepBeats;
				end = std::abs(lenSteps - std::round(lenSteps)) < 1.0e-6 ? stepTime(geo, step + juce::roundToInt(lenSteps))
				                                                           : start + dur;
			}
			if (std::abs(start - gesture.curStart) < 1.0e-9 && note == gesture.curNote) return;
			if (overlapsSamePitch(notes, gesture.engineIndex, note, start, end)) return;
			if (!gesture.checkpointed) { eng.pushUndoSnapshot("Grid: move note"); gesture.checkpointed = true; }
			const int idx = eng.updateNoteEvent(selTrack, gesture.engineIndex, start, end, note, gesture.curVelocity);
			if (idx < 0) return;
			gesture.engineIndex = idx;
			gesture.curStart = start;
			gesture.curEnd = end;
			const bool pitchChanged = note != gesture.curNote;
			gesture.curNote = note;
			gesture.changed = true;
			if (pitchChanged) startAudition(note, gesture.curVelocity); // hear where it lands
			repaint();
			return;
		}

		case GestureKind::keyAudition:
			// Dragging the key column scrolls the pitch range (a touchscreen has no wheel, and the
			// scroll bar is a thin target).
			if (std::abs(p.y - gesture.downPos.y) < 6.0f) return;
			stopAudition();
			gesture.kind = GestureKind::keyScroll;
			[[fallthrough]];
		case GestureKind::keyScroll:
			setTopNote(gesture.scrollStartTop + juce::roundToInt((p.y - gesture.downPos.y) / rowH));
			return;
		case GestureKind::none: return;
	}
}

void D110SequencerGridPanel::mouseUp(const juce::MouseEvent &) {
	++longPressToken; // released: no long press
	auto &eng = engine();
	const auto geo = geometry();
	switch (gesture.kind) {
		case GestureKind::pendingNote: {
			// Pressed and released on a note without moving: remove it.
			if (gestureNoteStillValid(notesNear(geo))) {
				eng.pushUndoSnapshot("Grid: delete note");
				eng.deleteNoteEvent(selTrack, gesture.engineIndex);
			}
			break;
		}
		case GestureKind::newNote:
		case GestureKind::resizeNote:
			// The next click creates a note as long as this one - the LMMS habit.
			if (gesture.changed)
				noteLenSteps = juce::jmax(1, juce::roundToInt((gesture.curEnd - gesture.curStart) / geo.stepBeats));
			break;
		default: break;
	}
	stopAudition();
	gesture = {};
	repaint();
}

void D110SequencerGridPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
	if (tempoBounds.contains(e.position)) {
		engine().setTempo(engine().getTempo() + (wheel.deltaY > 0 ? 1.0 : -1.0));
		repaint();
		return;
	}
	if (gridBounds.contains(e.position) || keysBounds.contains(e.position)) {
		if (e.mods.isCtrlDown()) { // Ctrl+wheel: taller (up) / shorter (down) rows
			float target = rowH;
			if (wheel.deltaY > 0) {
				for (float h : rowHeights)
					if (h > rowH + 0.5f) { target = h; break; }
			} else {
				for (float h : rowHeights)
					if (h < rowH - 0.5f) target = h; // the list ascends, so this ends on the next one down
			}
			setRowHeight(target, true);
			return;
		}
		setTopNote(topNote + (wheel.deltaY > 0 ? 2 : -2));
	}
}
