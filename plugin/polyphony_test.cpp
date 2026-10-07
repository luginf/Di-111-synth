// Where do the notes go during fast playing: does the instrument lose them, or is there nowhere to play them?
//
// The complaint sounds the same in both cases - "some notes do not sound" - but the causes are
// opposite, and they are cured differently. So the probe counts a note at THREE successive checkpoints:
//
//   1. how many notes were sent to the plugin;
//   2. how many of them the FIRMWARE accepted - it decides itself which part plays and whether there are enough
//      voices, and writes each accepted note into its tables, from which the bridge reads them;
//   3. how many partials are busy in the sound engine meanwhile and how many parts sound.
//
// Between the first and second checkpoint stands the LA32 stub (D110Core::StuckPolicy::La32Stub):
// neither MAME nor this project emulates the synthesis chip, and the firmware is answered on its behalf. If
// it is the one losing notes, the losses show up exactly here - more sent than accepted.
//
// Between the second and third - polyphony: the D-110 has thirty-two partials for everything, a tone costs from
// one to four partials, and a released note holds its partials until its
// decay is finished. A four-partial tone means eight notes for the whole instrument, and that is not a fault but a
// property of the machine. So as not to mistake one for the other, each run goes TWICE: with a two-partial
// tone and with a four-partial one. If the losses double along with the partials, it is
// polyphony; if they are equal, it is not.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Part 1 answers on channel 2 on a factory instrument.
constexpr int kChannel = 2;

struct Tally {
	int sent = 0;
	int firmwareStarted = 0;
	int peakPartials = 0;
	int peakVoices = 0;   // how many voices the firmware held at once
};

void renderBlocks(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

// How many voices the firmware holds right now - from its own LA32 slot table.
int busySlots(const std::vector<uint8_t> &ram) {
	int busy = 0;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110Core::kSlotStateTable) + size_t(s) * 2;
		if (at >= ram.size()) continue;
		const uint8_t v = ram[at];
		if (v == D110Core::kSlotBusyValue || v == D110Core::kSlotBusyValueAlt) ++busy;
	}
	return busy;
}

// One run: `count` notes in a row, `noteMs` each, with a gap of `gapMs`.
Tally play(D110AudioProcessor &proc, int count, int noteMs, int gapMs) {
	Tally t;
	const uint64_t startedBefore = proc.getCore().firmwareNoteOns();

	for (int i = 0; i < count; ++i) {
		const int note = 48 + (i % 13);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, note, 0.9f), 0);
		renderBlocks(proc, juce::jmax(1, int(double(noteMs) * kSampleRate / (kBlock * 1000.0))),
		             &on);
		++t.sent;
		t.peakPartials = std::max(t.peakPartials, proc.engineActivePartials());
		t.peakVoices = std::max(t.peakVoices, busySlots(snapshot(proc)));

		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
		renderBlocks(proc, juce::jmax(1, int(double(gapMs) * kSampleRate / (kBlock * 1000.0))),
		             &off);
	}
	render(proc, 1.5);   // let the decays finish
	t.firmwareStarted = int(proc.getCore().firmwareNoteOns() - startedBefore);
	return t;
}

// Sets part 1's tone by group and number - these are two bytes of its record in Timbre Temporary.
void setPartTone(D110AudioProcessor &proc, int group, int number) {
	proc.sendTimbreTempParam(0, 0, uint8_t(group));
	proc.sendTimbreTempParam(0, 1, uint8_t(number));
	render(proc, 1.2);
}

void report(const char *what, const Tally &t, int partialsPerNote) {
	const int lost = t.sent - t.firmwareStarted;
	std::printf("  %-34s отправлено %3d, прошивка взяла %3d%s   пик: партиалов %2d, "
	            "голосов %2d   потолок по партиалам ~%d нот\n",
	            what, t.sent, t.firmwareStarted,
	            lost > 0 ? "  <-- ПОТЕРЯ" : "            ", t.peakPartials, t.peakVoices,
	            partialsPerNote > 0 ? 32 / partialsPerNote : 0);
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   движок: %s   партиалов у движка: %d\n\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.engineIsOpen() ? "открыт" : "НЕТ",
	            int(proc.enginePartialCount()));
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) return 1;

	// La32Ramps + the correct status byte encoding (slot+1, docs/la32_register_map.md)
	// brings the step counter eec0[voice] up to 7 and really frees the slot
	// (plugin/slot_life_probe.cpp). We check here that this fixes polyphony specifically, and not
	// merely the fact of the table being freed.
	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Ramps);
	proc.getCore().setLa32StatusMode(1);
	std::printf("политика: La32Ramps, режим байта состояния = 1 (слот+1)\n\n");

	// Two tones with a KNOWN different number of partials, per the laminated Preset Tones card:
	// a02 "Acou Piano 2" - two partials, b01 "Fantasy" - four. This is the control: if
	// the losses come from polyphony they must differ; if from the stub, they must be equal.
	struct Case { const char *name; int group, number, partials; };
	const Case kCases[] = {
		{ "a02 Acou Piano 2 (2 партиала)", 0, 1, 2 },
		{ "b01 Fantasy (4 партиала)",      1, 0, 4 },
		// The third case is an INTERNAL tone, group 2 "i INTERNAL". The factory groups a and b
		// live in ROM and are the same for everyone, while this memory is filled with an outside bank, and its
		// envelopes are foreign. The complaint "notes last and do not decay" came exactly when
		// a bank first appeared in this memory, so it must be checked separately: the probe
		// has so far had no case where a tone was not taken from ROM.
		//
		// The number of partials of a loaded tone is not known in advance, so the "ceiling" column
		// holds 0 - there is nothing to compute it from, and nothing to invent. What to look at is different:
		// whether accepted notes match released ones and whether partials fall to zero at rest.
		{ "i01 (внутренний, из залитого банка)", 2, 0, 0 },
	};

	for (const Case &c : kCases) {
		std::printf("=== %s ===\n", c.name);
		setPartTone(proc, c.group, c.number);

		// Slow: note 250 ms, pause 250 ms. Nobody loses anything like this on the instrument, and this is
		// the lower bound - if notes are lost HERE, it is not polyphony at all.
		report("медленно, 2 ноты в секунду", play(proc, 12, 250, 250), c.partials);
		// Fast: 100 ms note, 20 ms pause - about eight notes a second, the tempo of a run.
		report("быстро, ~8 нот в секунду", play(proc, 24, 100, 20), c.partials);
		// And overlapping: notes are not released until eight are held - this way partials run out
		// for certain, and we see on which voice the instrument starts stealing.
		{
			Tally t;
			const uint64_t before = proc.getCore().firmwareNoteOns();
			for (int i = 0; i < 10; ++i) {
				juce::MidiBuffer on;
				on.addEvent(juce::MidiMessage::noteOn(kChannel, 48 + i * 2, 0.9f), 0);
				renderBlocks(proc, 6, &on);
				++t.sent;
				t.peakPartials = std::max(t.peakPartials, proc.engineActivePartials());
				t.peakVoices = std::max(t.peakVoices, busySlots(snapshot(proc)));
			}
			juce::MidiBuffer off;
			for (int i = 0; i < 10; ++i)
				off.addEvent(juce::MidiMessage::noteOff(kChannel, 48 + i * 2), 0);
			renderBlocks(proc, 4, &off);
			render(proc, 2.0);
			t.firmwareStarted = int(proc.getCore().firmwareNoteOns() - before);
			report("аккорд из 10 внахлёст", t, c.partials);
		}
		std::printf("\n");
	}

	// --- who exactly is the limit: the engine or the firmware ----------------------------
	//
	// Both have a partial reserve. The firmware hands out its voices by it, and the engine its own
	// partials, and the bytes are THE SAME for both: the system area is carried over by the mirror. So
	// raising the reserve the usual way means raising it in both at once, and from such an experiment you cannot
	// tell which one was in the way.
	//
	// So the experiment is run twice. First the reserve is raised ONLY IN THE ENGINE, bypassing
	// the firmware (engineWriteSysexForTest - that is what it is for), then the usual way, in both.
	// If the partial peak grows from the first - the limit was set by the engine; if only from
	// the second - by the firmware.
	{
		auto chord = [&proc](const char *what) {
			int peak = 0;
			for (int i = 0; i < 10; ++i) {
				juce::MidiBuffer on;
				on.addEvent(juce::MidiMessage::noteOn(kChannel, 48 + i * 2, 0.9f), 0);
				renderBlocks(proc, 6, &on);
				peak = std::max(peak, proc.engineActivePartials());
			}
			juce::MidiBuffer off;
			for (int i = 0; i < 10; ++i)
				off.addEvent(juce::MidiMessage::noteOff(kChannel, 48 + i * 2), 0);
			renderBlocks(proc, 4, &off);
			render(proc, 2.0);
			std::printf("  %-46s пик партиалов %2d\n", what, peak);
			return peak;
		};

		std::printf("=== КТО СТАВИТ ПРЕДЕЛ ===\n");
		setPartTone(proc, 0, 1);            // two-partial tone: ten notes make twenty
		chord("как есть, заводской резерв 4 4 4 4 3 3 3 2 5");

		// Reserve in the engine only: all thirty-two to part 1, zero to the others.
		{
			uint8_t data[9] = { 32, 0, 0, 0, 0, 0, 0, 0, 0 };
			uint8_t msg[D110Core::kMaxSysexBytes];
			const int n = D110Core::buildDt1Message(D110Core::kSysexSystem, 4, data, 9, msg);
			if (n > 0) proc.engineWriteSysexForTest(msg, n);
			render(proc, 0.5);
		}
		chord("резерв 32 ТОЛЬКО у движка");

		// And now the usual way - through the firmware, as the editor does it.
		for (int i = 0; i < 9; ++i) proc.sendSystemParam(4 + i, i == 0 ? 32 : 0);
		render(proc, 1.5);
		chord("резерв 32 у прошивки И у движка");

		// Restore the factory reserve. The nine values are tied by a sum of 32, so they go
		// in ONE message: one at a time the instrument will reject them, and rightly so.
		const uint8_t factory[9] = { 4, 4, 4, 4, 3, 3, 3, 2, 5 };
		proc.sendAreaData(D110Core::kSysexSystem, 4, factory, 9);
		render(proc, 1.5);
		std::printf("  резерв возвращён к заводскому 4 4 4 4 3 3 3 2 5\n");
	}
	std::printf("\n");

	// What is left hanging after everything. A busy slot with released keys is a voice
	// leak, and it would explain "starts eating notes over time" far better than polyphony.
	render(proc, 3.0);
	const auto ram = snapshot(proc);
	std::printf("=== после всего, при отпущенных клавишах ===\n");
	std::printf("  занятых слотов у прошивки: %d из %d\n", busySlots(ram),
	            D110Core::kNumHardwareVoices);
	std::printf("  партиалов у движка: %d\n", proc.engineActivePartials());
	std::printf("  нот принято прошивкой всего: %llu, отпущено: %llu\n",
	            (unsigned long long)proc.getCore().firmwareNoteOns(),
	            (unsigned long long)proc.getCore().firmwareNoteOffs());

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
