// What bank 0x0CC0 carries WHILE a note sounds.
//
// The register map (docs/la32_register_map.md) covered everything written AT THE MOMENT a
// voice is issued: pulse width, cutoff, resonance, waveform, ROM wave selection, pitch. What
// remains is the part of the interface that works continuously rather than once: bank 0x0CC0
// is rewritten from ROM 0x2C0D the whole time the note is held.
//
// The question is not idle, it decides the shape of the whole emulation. The chip model in
// munt (LA32WaveGenerator) requires three quantities for EVERY sample - amplitude, pitch and
// cutoff - yet the stream carries two bytes per slot. So either the stream is multiplexed, or
// amplitude and cutoff travel by their own paths that the earlier capture did not separate.
//
// Earlier attempts on this bank failed THREE times, each time for the same reason:
// the streams were compared as SETS of values. Runs have different numbers of updates, so
// the vectors differ even when their beginnings match element by element - and the bank was
// declared "dependent on everything". Here the stream is compared with nothing: it is PRINTED
// as a time series, together with the address of the routine that made the write.
//
// Modes:
//   observe            one note, full time-resolved breakdown of the whole 0x0C00-0x0DFF window
//   env <group> <step> the same note after editing one envelope parameter - for comparison
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

constexpr int kBytesPerSlot = 2;

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

void press(D110AudioProcessor &proc, const char *name, int times = 1) {
	for (const auto &b : kButtons)
		if (std::strcmp(b.name, name) == 0) {
			const int idx = D110Core::buttonIndex(b.port, b.bit);
			for (int i = 0; i < times; ++i) {
				proc.getCore().setButton(idx, true);
				render(proc, 0.13);
				proc.getCore().setButton(idx, false);
				render(proc, 0.30);
			}
			return;
		}
	std::printf("  !!! нет такой кнопки: %s\n", name);
}

std::vector<uint8_t> ramOf(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

struct Run {
	std::vector<D110Core::SoWrite> writes;
	std::vector<int> slots;
	uint64_t dropped = 0;
	double heldMs = 0, releasedMs = 0;
	// How many times per note the firmware got an answer from the chip. On a real LA32 the INT
	// pin is raised by RAMP COMPLETION (munt: LA32Ramp::checkInterrupt), so there should be
	// roughly as many answers as there are envelope steps. If there are thousands or zero, the
	// firmware envelopes do not follow the same path as on hardware, and "the register did not
	// move" may mean exactly that, not that the parameter is absent.
	uint64_t servicesBefore = 0, servicesAfter = 0;
};

// The note is held for a long time ON PURPOSE. For most timbres the TVA envelope does not
// even reach the sustain phase within half a second, and "the stream carries the envelope"
// can be told from "the stream carries pitch" only where the envelope is definitely moving -
// that is, on attack and on decay.
Run playOne(D110AudioProcessor &proc, int note, int velocity, double hold, double tail) {
	Run r;
	r.servicesBefore = proc.getCore().la32Services();
	proc.getCore().startSoTrace();
	const auto t0 = Clock::now();
	const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)};
	proc.getCore().pushMidi(on, 3);
	render(proc, hold);
	r.heldMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	render(proc, tail);
	r.releasedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
	proc.getCore().stopSoTrace();

	r.servicesAfter = proc.getCore().la32Services();
	r.dropped = proc.getCore().soWritesDropped();
	r.writes = proc.getCore().takeSoWrites();

	// The slots of this note are those touched in the issue bank 0x0C00. The envelope bank is
	// unsuitable for that: it keeps playing out the previous notes in parallel.
	for (const auto &w : r.writes)
		if ((w.addr & 0xFFC0) == D110Core::kLa32TapBase) {
			const int slot = (w.addr & 0x3F) / kBytesPerSlot;
			if (std::find(r.slots.begin(), r.slots.end(), slot) == r.slots.end())
				r.slots.push_back(slot);
		}
	std::sort(r.slots.begin(), r.slots.end());
	return r;
}

const char *bankName(uint16_t bank) {
	switch (bank) {
	case 0x0C00: return "0C00";
	case 0x0C40: return "0C40";
	case 0x0C80: return "0C80";
	case 0x0CC0: return "0CC0";
	case 0x0D00: return "0D00";
	default: return "????";
	}
}

// How many times per note each bank is rewritten, and who writes it. This is the answer to
// "what is configured once and what flows": a one-time setup gives a handful of writes, a
// stream gives thousands.
void reportBanks(const Run &r) {
	std::map<uint16_t, size_t> perBank;
	std::map<uint16_t, std::map<uint16_t, size_t>> pcPerBank;
	for (const auto &w : r.writes) {
		const uint16_t bank = w.addr & 0xFFC0;
		++perBank[bank];
		++pcPerBank[bank][w.pc];
	}
	std::printf("\n  банк | записей | подпрограммы, которые в него пишут\n");
	for (const auto &[bank, n] : perBank) {
		std::printf("  %s | %7zu |", bankName(bank), n);
		std::vector<std::pair<size_t, uint16_t>> pcs;
		for (const auto &[pc, cnt] : pcPerBank[bank]) pcs.push_back({cnt, pc});
		std::sort(pcs.rbegin(), pcs.rend());
		for (size_t i = 0; i < pcs.size() && i < 4; ++i)
			std::printf(" ПЗУ %04X x%zu", pcs[i].second, pcs[i].first);
		std::printf("\n");
	}
}

// The series of values over time for one address. Only the MOMENTS OF CHANGE are printed: a
// stream rewriting the same value a thousand times and a stream driving an envelope are
// indistinguishable by write count, but entirely distinguishable by number of DIFFERENT values.
void reportSeries(const Run &r, uint16_t addr, double onMs, int maxShown = 24) {
	std::vector<std::pair<double, uint8_t>> changes;
	int have = -1;
	for (const auto &w : r.writes) {
		if (w.addr != addr) continue;
		if (have == int(w.value)) continue;
		changes.push_back({w.ms, w.value});
		have = int(w.value);
	}
	size_t total = 0;
	for (const auto &w : r.writes) if (w.addr == addr) ++total;
	std::printf("    %04X: записей %5zu, разных значений подряд %3zu |", addr, total,
	            changes.size());
	const int step = int(changes.size()) > maxShown ? int(changes.size()) / maxShown : 1;
	int shown = 0;
	for (size_t i = 0; i < changes.size(); i += size_t(step)) {
		if (shown++ >= maxShown) break;
		std::printf(" %.0f:%02X", changes[i].first - onMs, changes[i].second);
	}
	std::printf("\n");
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// SEVERAL edits may be needed at once, and that is a necessity, not a convenience. The TVA
	// attack time changes nothing by itself if the envelope levels sit at maximum: the ramp has
	// nowhere to go, and "the register did not move" would mean not "the parameter does not
	// arrive" but "the stimulus stimulated nothing". Triples: group, parameter, step (minus = down).
	const std::string mode = argc > 1 ? argv[1] : "observe";
	// In ramps mode the first argument is the status byte encoding, followed by edit triples:
	// edits are needed here too, because the whole point of the ramps check is that an envelope
	// that previously never got that far now does.
	const int statusMode = (mode == "ramps" && argc > 2) ? std::atoi(argv[2]) : 0;
	struct Edit { int group, bank, presses; };
	std::vector<Edit> edits;
	for (int i = (mode == "ramps" ? 3 : 2); i + 2 < argc; i += 3)
		edits.push_back({std::atoi(argv[i]), std::atoi(argv[i + 1]), std::atoi(argv[i + 2])});

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	if (!proc.getCore().isRunning()) { std::printf("прошивка не поднялась\n"); return 1; }

	// Order mode: both the chip registers AND the slot state table are captured, so that they
	// land on one time axis. For that the filter is widened to 0xEFFF - nobody writes to this log
	// between 0x0DFF and 0xEDC0, so nothing extra is collected.
	if (mode == "order")
		proc.getCore().setTraceFilter(D110Core::kLa32TapBase, 0xEFFF);
	else
		proc.getCore().setTraceFilter(D110Core::kLa32TapBase, D110Core::kLa32TapEnd);

	// Ramps mode: count ramps for real and raise the interrupt when they arrive.
	// Set AFTER power-on - setPoweredOn sets the policy itself.
	// The status byte encoding is tried in turn: the handler analysis left four variants,
	// and which one is right is decided by experiment - how many envelope steps follow.
	if (mode == "ramps") {
		proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Ramps);
		proc.getCore().setLa32StatusMode(statusMode);
		// Second digit of the mode, if present: whether to treat 0xFF as a preset without interrupt.
		const bool presetFf = statusMode >= 10;
		proc.getCore().setLa32StatusMode(statusMode % 10);
		proc.getCore().setLa32PresetFf(presetFf);
		std::printf("рампы включены, кодировка байта состояния: %d, 0xFF как установка: %s\n",
		            statusMode % 10, presetFf ? "да" : "нет");
	}

	std::printf("заводской сброс, чтобы тембр был известным...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);
	{
		const auto ram = ramOf(proc);
		std::printf("  тембр партии 1: группа %d, номер %d; структуры %d и %d\n",
		            ram[0x2000], ram[0x2001], ram[0x21E4 + 10], ram[0x21E4 + 11]);
	}

	// CONTROL. A window of the same length without a note must be silent: if it has writes, all
	// the rest has to be read with the background in mind, not as "caused by the note".
	std::printf("\n=== контроль: окно без ноты ===\n");
	{
		proc.getCore().startSoTrace();
		render(proc, 2.5);
		proc.getCore().stopSoTrace();
		const auto w = proc.getCore().takeSoWrites();
		std::printf("  записей: %zu%s\n", w.size(), w.empty() ? "" : "  !!! окно не молчит");
	}

	// ---- which register bit carries the partial pair STRUCTURE --------------------------
	// The structure decides two things at once: which partial is synthetic and which is PCM, and
	// whether they are summed or multiplied by ring modulation. The first is already found - bit 7
	// of byte 0x0D00. There is no point hunting for the second at random: all structure values
	// are stepped through, and what the munt tables say about each is printed next to it. The bit
	// that moves together with ring modulation and in no other way will be the answer.
	if (mode == "struct") {
		// Tables from munt/Part.cpp: what each structure actually is.
		static const uint8_t kPartialStruct[13] = {0, 0, 2, 2, 1, 3, 3, 0, 3, 0, 2, 1, 3};
		static const uint8_t kMixStruct[13] = {0, 1, 0, 1, 1, 0, 1, 3, 3, 2, 2, 2, 2};

		std::printf("\n  структура | ожидается по munt      | флаги 0x0D00 по слотам\n");
		for (int step = 0; step < 13; ++step) {
			press(proc, "Exit", 3);
			press(proc, "Timbre");
			press(proc, "Edit");
			press(proc, "Edit");
			press(proc, "Group+", 1); // common part, structure page 1&2 (tone +10)
			if (step) press(proc, "Number+", 1);
			render(proc, 0.4);
			const auto ram = ramOf(proc);
			const int structure = ram[0x21E4 + 10];
			press(proc, "Exit", 3);

			const Run r = playOne(proc, 60, 100, 0.35, 0.35);
			std::printf("  %9d | %-5s + %-5s, mix %d | ", structure,
			            (structure < 13 && (kPartialStruct[structure] & 2)) ? "PCM" : "синт",
			            (structure < 13 && (kPartialStruct[structure] & 1)) ? "PCM" : "синт",
			            structure < 13 ? kMixStruct[structure] : -1);
			for (int slot : r.slots) {
				uint8_t flag = 0;
				bool got = false;
				for (const auto &w : r.writes)
					if (w.addr == uint16_t(0x0D00 + 2 * slot) && !got) { flag = w.value; got = true; }
				if (got) std::printf("слот%d=%02X ", slot, flag);
			}
			std::printf("\n");
			if (r.slots.empty()) { std::printf("  слоты кончились\n"); break; }
		}
		proc.setPoweredOn(false);
		return 0;
	}

	if (!edits.empty()) {
		for (const auto &e : edits) {
			std::printf("\nправка: Part+ x2, Group+ x%d, Bank+ x%d, Number%s x%d\n", e.group,
			            e.bank, e.presses < 0 ? "-" : "+", std::abs(e.presses));
			press(proc, "Exit", 3);
			press(proc, "Timbre");
			press(proc, "Edit");
			press(proc, "Edit");
			press(proc, "Part+", 2);
			if (e.group) press(proc, "Group+", e.group);
			if (e.bank) press(proc, "Bank+", e.bank);
			const auto before = ramOf(proc);
			press(proc, e.presses < 0 ? "Number-" : "Number+", std::abs(e.presses));
			render(proc, 0.6);
			const auto after = ramOf(proc);
			std::printf("  сдвинулось в тембре:");
			bool any = false;
			for (int i = 0; i < 246; ++i)
				if (before[(size_t)(0x21E4 + i)] != after[(size_t)(0x21E4 + i)]) {
					any = true;
					std::printf(" +%d(%d->%d)", i, before[(size_t)(0x21E4 + i)],
					            after[(size_t)(0x21E4 + i)]);
				}
			std::printf("%s\n", any ? "" : " НИЧЕГО - правка не состоялась");
		}
		press(proc, "Exit", 3);
	}

	constexpr double kHold = 1.5, kTail = 1.2;
	std::printf("\n=== одна нота 60, сила 100: держим %.1f с, потом %.1f с после снятия ===\n",
	            kHold, kTail);
	const Run r = playOne(proc, 60, 100, kHold, kTail);
	const double onMs = r.writes.empty() ? 0.0 : r.writes.front().ms;
	std::printf("  всего записей %zu, потеряно %llu, слоты ноты:", r.writes.size(),
	            (unsigned long long)r.dropped);
	for (int s : r.slots) std::printf(" %d", s);
	std::printf("\n");
	if (r.dropped) std::printf("  !!! захват переполнился - всё ниже нижняя граница\n");
	std::printf("  ответов микросхемы за ноту (LA32 services): %llu\n",
	            (unsigned long long)(r.servicesAfter - r.servicesBefore));
	std::printf("  рамп запущено: %llu, дошло до цели: %llu\n",
	            (unsigned long long)proc.getCore().la32RampStarts(),
	            (unsigned long long)proc.getCore().la32RampLandings());

	if (mode == "order") {
		// Which comes first - the ramp registers or marking the slot busy. The start of the note is
		// printed in full, without thinning: the question is precisely the order of the first few
		// events. The pitch stream is dropped from the log - it alone gives thousands of writes and
		// would drown everything else, and it has no bearing on the question of order.
		std::printf("\n  первые события ноты на одной оси времени\n");
		std::printf("  мс    | ПЗУ  | адрес | знач | что это\n");
		int shown = 0;
		for (const auto &w : r.writes) {
			if ((w.addr & 0xFFC0) == 0x0CC0) continue;
			const char *what = "?";
			if ((w.addr & 0xFFC0) == D110Core::kAmpRampBase) what = "рампа амплитуды";
			else if ((w.addr & 0xFFC0) == D110Core::kFilterRampBase) what = "рампа среза";
			else if ((w.addr & 0xFFC0) == 0x0C40) what = "настройка (ширина/срез)";
			else if ((w.addr & 0xFFC0) == 0x0D00) what = "настройка (волна/резонанс)";
			// The firmware tables are spaced 0x40 apart, like the chip banks, so the slot number is
			// taken from the start of ITS OWN table, not from the start of the whole area.
			else if (w.addr >= 0xEDC0) {
				switch (w.addr & 0xFFC0) {
				case 0xEDC0: what = "СЛОТ: пометка занятости"; break;
				case 0xEE00: what = "слот: контекст"; break;
				case 0xEE40: what = "слот: цепочка"; break;
				case 0xEE80: what = "слот: таблица EE80"; break;
				case 0xEEC0: what = "слот: таблица EEC0"; break;
				case 0xEF00: what = "слот: таблица EF00"; break;
				case 0xEF40: what = "слот: база высоты"; break;
				case 0xEF80: what = "слот: таблица EF80"; break;
				default: what = "слот: прочее"; break;
				}
			}
			const int slot = (w.addr & 0x3F) / 2;
			std::printf("  %6.2f | %04X | %04X  |  %02X  | %s, слот %d\n", w.ms - onMs, w.pc,
			            w.addr, w.value, what, slot);
			if (++shown >= 60) { std::printf("  ... (обрезано)\n"); break; }
		}
	}

	reportBanks(r);

	std::printf("\n  ряды во времени, мс от первой записи (только моменты ИЗМЕНЕНИЯ значения)\n");
	for (int slot : r.slots) {
		std::printf("  --- слот %d ---\n", slot);
		for (uint16_t bank2 : {0x0C00, 0x0C40, 0x0C80, 0x0CC0, 0x0D00})
			for (int b = 0; b < kBytesPerSlot; ++b)
				reportSeries(r, uint16_t(bank2 + slot * kBytesPerSlot + b), onMs);
	}

	proc.setPoweredOn(false);
	std::printf("\nготово\n");
	return 0;
}
