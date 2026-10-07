// "I press notes on all parts, a note sticks and does not release."
//
// All earlier d110_polyphony runs went through ONE part, on channel 2, and never showed
// a stuck note: 168 notes taken, 167 released, at rest zero slots and zero partials.
// The difference the owner names is precisely many parts at once, and no test bench has
// reproduced it so far.
//
// Why it may matter. Note release in the engine is born by TWO paths
// (D110Core.cpp, releaseContext): the firmware marked the voice as returned - f460 bit 6 - or
// the context was handed to a new note while the old one still sounded, i.e. the voice was stolen.
// While slots leaked, stealing went on constantly and the second path worked in place of the first.
// Now slots are returned, there is no stealing, and the whole load fell on f460. On one part it
// holds; the question is whether it holds on nine, when contexts are consumed ten times as often.
//
// Measured by accumulation over rounds, not by a single snapshot: a stuck note is what does NOT
// go away, so the only honest sign is the remainder with keys released, and it must
// grow from round to round if the complaint is correct.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Part N answers on channel N+1 on a factory unit, part 9 is rhythm on channel 10.
constexpr int kFirstChannel = 2;
constexpr int kNumParts = 9;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (b == 0 && midi) ? *midi : none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

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

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	std::printf("прошивка: работает   партиалов у движка: %d\n", proc.engineActivePartials());
	std::printf("политика: %s\n\n", "шаблонная (La32Ramps + режим 1)");

	std::printf("круг | взято | отпущено | разница | слотов | партиалов\n");
	std::printf("     |       |          | нарастающим итогом при ОТПУЩЕННЫХ клавишах\n");

	for (int round = 1; round <= 6; ++round) {
		// A chord across all nine parts at once - exactly as the complaint describes. The notes
		// differ per part so the firmware has no way to count them as a repeat and silence the
		// old one instead of issuing a new one.
		juce::MidiBuffer on;
		for (int p = 0; p < kNumParts; ++p)
			on.addEvent(juce::MidiMessage::noteOn(kFirstChannel + p, 48 + p * 2, 0.9f), 0);
		render(proc, 1.0, &on);

		juce::MidiBuffer off;
		for (int p = 0; p < kNumParts; ++p)
			off.addEvent(juce::MidiMessage::noteOff(kFirstChannel + p, 48 + p * 2), 0);
		render(proc, 0.2, &off);

		// Three seconds of silence AFTER release: any honest decay has plenty of margin in that,
		// while a stuck note would just stay hanging for the same time.
		render(proc, 3.0);

		const uint64_t ons = proc.getCore().firmwareNoteOns();
		const uint64_t offs = proc.getCore().firmwareNoteOffs();
		std::printf("  %2d | %5llu | %8llu | %7lld | %6d | %9d\n", round,
		            (unsigned long long)ons, (unsigned long long)offs,
		            (long long)ons - (long long)offs, busySlots(snapshot(proc)),
		            proc.engineActivePartials());
	}

	std::printf("\nчитается так: разница и партиалы обязаны стоять на месте от круга к кругу.\n"
	            "Если они РАСТУТ - ноты вправду не заканчиваются, и жалоба воспроизведена.\n");

	// Breakdown by single part. The overall run above shows HOW MANY did not end, but
	// not on whose side - and the nine parts are not alike: the eighth is rhythm,
	// where the strike is one-sided and the release comes differently from a keyboard part. Until it is
	// known which part leaves a remainder, any fix would be a shot in the dark.
	std::printf("\n=== по одной партии ===\n");
	std::printf("партия | канал | взято | отпущено | разница | партиалов после\n");
	for (int p = 0; p < kNumParts; ++p) {
		const uint64_t onsBefore = proc.getCore().firmwareNoteOns();
		const uint64_t offsBefore = proc.getCore().firmwareNoteOffs();

		// Three notes in a row, each with its own release - fewer would not show repeatability,
		// more is not needed, because the remainder is visible already on the first.
		for (int i = 0; i < 3; ++i) {
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(kFirstChannel + p, 48 + i * 3, 0.9f), 0);
			render(proc, 0.6, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(kFirstChannel + p, 48 + i * 3), 0);
			render(proc, 0.2, &off);
		}
		render(proc, 3.0);

		const long long ons = (long long)(proc.getCore().firmwareNoteOns() - onsBefore);
		const long long offs = (long long)(proc.getCore().firmwareNoteOffs() - offsBefore);
		std::printf("  %4d | %5d | %5lld | %8lld | %7lld | %14d  %s\n", p + 1, kFirstChannel + p,
		            ons, offs, ons - offs, proc.engineActivePartials(),
		            (ons != offs) ? "<-- ОСТАТОК" : "");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
