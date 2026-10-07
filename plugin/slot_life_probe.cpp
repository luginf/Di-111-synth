// Who writes to the LA32 slot table, and when - and does anyone ever free it.
//
// The instrument plays about two notes at a time instead of eight, and it has been measured that with keys
// released the firmware has 31 of 32 slots busy (see `d110_polyphony`). The interrupt handler at
// 0x3138 picks its path by this very table - `rams[0x2DC0 + 2v] == 0x80` means "slot
// is free" - so it all comes down to the question of who puts 0x80 back there.
//
// The one to ask is not a disassembler but the running firmware: the write tap on the dispatch
// window (CPU 0xEDC0-0xEFFF) already exists, and it reports the ADDRESS, the VALUE and the VERY SAME
// PC the write was made from. If nobody writes 0x80 - there is nobody to free slots, and that is the
// answer. If someone does, but rarely - the answer is different, and the address will tell which subroutine does it.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2;   // part 1 on a factory instrument

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

int busySlots(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int busy = 0;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const uint8_t v = ram[size_t(D110Core::kSlotStateTable) + size_t(s) * 2];
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
	if (!proc.getCore().isRunning()) { std::printf("прошивка не работает\n"); return 1; }
	std::printf("занятых слотов до игры: %d из %d\n", busySlots(proc),
	            D110Core::kNumHardwareVoices);

	// FIXED: the previous version of this probe claimed to switch to La32Ramps, but the
	// setStuckPolicy call itself was lost during an edit and did not make it into the committed file - all the
	// experiments "under ramps" actually ran under the factory La32Stub. Discovered by going through the git history
	// (the call is absent in 879137a and 69619b9), not from an error in the results - those looked plausible
	// precisely because both trials are synchronized through the same handler and give
	// similar numbers. It is now checked explicitly, before the run, not after.
	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Ramps);
	// The status byte encoding for ramps (rampStatusByte) is its OWN, separate from the one already
	// proven by the disassembler for La32Stub (encodeLa32Status, default mode: slot+1, bit
	// 7 clear - docs/la32_register_map.md, "the number in the status byte is slot + 1"). In
	// mode 0 (default) rampStatusByte returns the bare slot WITHOUT +1 - a different slot, a different byte
	// in edc0/eec0. Mode 1 of rampStatusByte gives (slot+1)&0x1F without the bank flag - the right one.
	proc.getCore().setLa32StatusMode(1);
	std::printf("политика переключена на La32Ramps, режим байта состояния = 1 (слот+1)\n");

	// --- hypothesis 1: a two-partial tone may need BOTH to be freed at once ---
	//
	// Slot 0 in the experiment above tracked only one partial of the note. If the release of one partial
	// waits for the release of the other - via a shared per-part reference counter, f283[part]/f284[part] -
	// then in one of the two slots eec0 must reach 7 first and hang there, waiting for
	// the other. Here we watch slots 0 AND 1 in the same time window.
	std::printf("\nодна нота (2 партиала), слежение за ОБОИМИ слотами разом:\n");
	{
		// The dispatcher rotates slots (earlier traces got 4,5,6,7...), so we cannot
		// assume the new note lands in 0 and 1 - those are leftovers of the previous experiment.
		// This note's slots are taken by fact: a snapshot BEFORE and a snapshot AFTER, the difference is the answer.
		std::vector<uint8_t> before(D110Core::kRamSize, 0);
		proc.getCore().getRam(before.data());

		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 60, 0.9f), 0);
		renderBlocks(proc, 3, &on);

		std::vector<uint8_t> after(D110Core::kRamSize, 0);
		proc.getCore().getRam(after.data());
		std::vector<int> newSlots;
		for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
			const size_t at = size_t(D110Core::kSlotStateTable) + size_t(s) * 2;
			const bool wasBusy = before[at] == D110Core::kSlotBusyValue
			                   || before[at] == D110Core::kSlotBusyValueAlt;
			const bool isBusy = after[at] == D110Core::kSlotBusyValue
			                  || after[at] == D110Core::kSlotBusyValueAlt;
			if (isBusy && !wasBusy) newSlots.push_back(s);
		}
		std::printf("  слоты, доставшиеся этой ноте: ");
		for (int s : newSlots) std::printf("%d ", s);
		std::printf("(%d партиал%s)\n", int(newSlots.size()), newSlots.size() == 1 ? "" : "а");
		if (newSlots.size() < 2) {
			std::printf("  меньше двух слотов - опыт про пару партиалов здесь не проверить\n");
		}

		bool released = false;
		for (int step = 0; step < 16; ++step) {
			renderBlocks(proc, 45);
			std::vector<uint8_t> ram(D110Core::kRamSize, 0);
			proc.getCore().getRam(ram.data());
			std::printf("  t=%4.1fs", (step + 1) * 0.5 + 0.3);
			for (int s : newSlots) {
				const size_t at = size_t(D110Core::kSlotStateTable) + size_t(s) * 2;
				std::printf("   слот%d: edc0=0x%02X eec0=%d", s, ram[at], ram[0x2EC0 + s * 2]);
			}
			std::printf("%s\n", released ? "  (снята)" : "  (держим)");
			if (step == 5 && !released) {
				juce::MidiBuffer off;
				off.addEvent(juce::MidiMessage::noteOff(kChannel, 60), 0);
				renderBlocks(proc, 1, &off);
				released = true;
			}
		}
	}


	// ONE note, LONG: six short notes in a row do not let the envelope live out its real
	// time either to the end of the decay or, still less, to the release after key-off. Here we hold
	// the key down for three seconds, and hold silence after release for five - printing the counter
	// every half second during play, and not as a single end-of-run snapshot.
	proc.getCore().setVoiceCtxTap(true);
	std::printf("\nодна нота, слежение за eec0[слот 0] и edc0[слот 0] в реальном времени:\n");
	{
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 60, 0.9f), 0);
		renderBlocks(proc, 1, &on);
		bool released = false;
		for (int step = 0; step < 16; ++step) {
			renderBlocks(proc, 45);   // ~0.5 s per step
			std::vector<uint8_t> ram(D110Core::kRamSize, 0);
			proc.getCore().getRam(ram.data());
			std::printf("  t=%4.1fs  edc0[0]=0x%02X  eec0[0]=%d%s\n", (step + 1) * 0.5,
			            ram[D110Core::kSlotStateTable], ram[0x2EC0],
			            released ? "  (снята)" : "  (держим)");
			if (step == 5 && !released) {
				juce::MidiBuffer off;
				off.addEvent(juce::MidiMessage::noteOff(kChannel, 60), 0);
				renderBlocks(proc, 1, &off);
				released = true;
			}
		}
	}
	proc.getCore().setVoiceCtxTap(false);

	// The same six-note run, for comparing the counters at the end.
	proc.getCore().setVoiceCtxTap(true);
	for (int i = 0; i < 6; ++i) {
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 48 + i * 3, 0.9f), 0);
		renderBlocks(proc, 14, &on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, 48 + i * 3), 0);
		renderBlocks(proc, 10, &off);
	}
	render(proc, 6.0);
	proc.getCore().setVoiceCtxTap(false);

	const auto events = proc.getCore().takeCtxEvents();
	std::printf("событий записи в окно диспетчеризации: %d\n\n", int(events.size()));

	// Only the slot table itself. The tap records the RAM OFFSET, not the CPU address:
	// the table is rams 0x2DC0 + 2*slot, i.e. 0x2DC0..0x2DFF. The first version of this probe
	// filtered on 0xEDC0 and got "zero writes" where there are ninety-two.
	struct Key { uint16_t pc; uint8_t value; };
	std::map<uint32_t, int> byPcValue;
	int toSlotTable = 0;
	for (const auto &e : events) {
		if (e.addr < D110Core::kSlotStateTable || e.addr > D110Core::kSlotStateTable + 63) continue;
		++toSlotTable;
		byPcValue[(uint32_t(e.pc) << 8) | e.value] += 1;
	}
	std::printf("=== записи В ТАБЛИЦУ СЛОТОВ (0xEDC0..0xEDFF): %d ===\n", toSlotTable);
	std::printf("  откуда (PC)   значение   сколько раз   что это значит\n");
	for (const auto &kv : byPcValue) {
		const uint16_t pc = uint16_t(kv.first >> 8);
		const uint8_t value = uint8_t(kv.first & 0xff);
		const char *meaning = (value == D110Core::kSlotIdleValue)   ? "СВОБОДЕН"
		                    : (value == D110Core::kSlotBusyValue)   ? "занят (0x40)"
		                    : (value == D110Core::kSlotBusyValueAlt) ? "занят (0x20)"
		                                                            : "?";
		std::printf("  0x%04X        0x%02X       %6d        %s\n", pc, value, kv.second, meaning);
	}
	if (toSlotTable == 0)
		std::printf("  ни одной записи - таблицу слотов за этот прогон не трогали вовсе\n");

	// The first events in order: they show whether issue and return come as a pair or only issue.
	std::printf("\n=== первые двадцать записей в таблицу слотов, по порядку ===\n");
	int shown = 0;
	for (const auto &e : events) {
		if (e.addr < D110Core::kSlotStateTable || e.addr > D110Core::kSlotStateTable + 63) continue;
		std::printf("  PC 0x%04X  слот %2d  <- 0x%02X\n", e.pc, (e.addr - D110Core::kSlotStateTable) / 2, e.value);
		if (++shown >= 20) break;
	}

	// And the neighbouring arrays of the same window - to see what else the handler is busy with.
	std::map<uint16_t, int> byArea;
	for (const auto &e : events) byArea[uint16_t(e.addr & 0xFFC0)] += 1;
	std::printf("\n=== куда ещё писали в этом окне ===\n");
	for (const auto &kv : byArea)
		std::printf("  0x%04X..0x%04X  %6d записей\n", kv.first, kv.first + 0x3F, kv.second);

	// Envelope step counter: eec0[voice], RAM 0x2EC0 + voice. The disassembler (0x32AA,
	// 0x3300-0x3308) says that freeing a slot (0x34FA: stb #0x80, edc0[64]) happens
	// ONLY when this counter reaches 7 - `inc 80; cmpb 80,#07; je 32aa`. If it
	// gets stuck below seven, the freeing code IS in the firmware but unreachable - the slot hangs not
	// because the stub does not write 0x80, but because it does not let the counter reach the threshold.
	// The index in the disassembler is register 64, and it is always voice*2 (`shlb 64,#01`, the same index
	// as for edc0[64]). So the real counter sits only at EVEN offsets of this window;
	// the odd ones are a memory neighbour, a different array. The first version of this probe scooped up both and
	// saw values like 250, which a counter of 0..7 cannot have.
	std::map<int, int> eec0Values;
	for (const auto &e : events) {
		if (e.addr < 0x2EC0 || e.addr > 0x2EFF) continue;
		if ((e.addr - 0x2EC0) % 2 != 0) continue;
		eec0Values[e.value] += 1;
	}
	std::printf("\n=== значения, записанные в eec0[] (счётчик ступени огибающей) ===\n");
	for (const auto &kv : eec0Values)
		std::printf("  значение %d: %d раз%s\n", kv.first, kv.second,
		            kv.first >= 7 ? "   <- порог освобождения" : "");
	if (eec0Values.empty() || eec0Values.rbegin()->first < 7)
		std::printf("  семёрка НИ РАЗУ не записана - освобождение недостижимо на этих данных\n");

	std::printf("\nзанятых слотов после игры и шести секунд тишины: %d из %d\n",
	            busySlots(proc), D110Core::kNumHardwareVoices);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
