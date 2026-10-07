// LA32 register map - which cell means what, derived from what the firmware writes there.
//
// No such map exists anywhere: nobody emulates the MB87136APF. The service notes only give
// conclusions (docs/service_notes_findings.md): nine address lines A0-A8, i.e. 512 registers,
// an eight-bit data bus, a WR input, an interrupt output. The 0x0C00-0x0DFF window is exactly
// that size, so it is the whole synthesis control interface.
//
// A register cannot be read, only written. So the meaning is found by making it change:
// four stimuli, each differing from the base one in exactly ONE property.
//
//   A  note 60, volume 100, timbre as is         - base
//   B  note 72                                   - difference from A = pitch
//   C  volume 40                                 - difference from A = key velocity
//   D  a different timbre                        - difference from A = timbre
//
// WHAT TO TREAT AS A UNIT. The first revision of this probe compared runs by absolute
// address and declared half the window "pitch" and half "volume" - a pure artifact:
// each note gets its own voice slots, and a cell looks changed simply because the other run
// did not touch it.
//
// The second revision reduced the address to the "smallest used offset in the bank", and on
// the 0x0CC0 envelope bank that lied too: it keeps serving the ALREADY RELEASED voice of the
// previous note, so the minimum belonged to the wrong note.
//
// Here the slot is not guessed at all. The firmware keeps a slot state table (RAM 0x2DC0 + 2n,
// see D110Core::kSlotStateTable): a free slot holds 0x80. A snapshot of this table before and
// after a note directly names the slots allocated to it. The bank spans 0x40 = 64 bytes for 32
// slots, i.e. **two bytes per slot**, and a note takes four because it uses two partials -
// consistent with the earlier measurement where exactly two slots per note went from 0x80
// to 0x40.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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
	{"Exit", 0, 7}, {"Timbre", 0, 5}, {"Number+", 0, 1}, {"Edit", 1, 7},
	{"Number-", 1, 1}, {"Group+", 0, 3}, {"Bank+", 0, 2}, {"Part+", 0, 4},
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
}

std::vector<uint8_t> ramOf(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

struct Capture {
	std::vector<D110Core::SoWrite> writes;
	std::vector<int> slots;        // slots of this note, taken from its own writes
	std::vector<int> slotsByTable; // the same by the state table - for cross-checking only
	std::map<uint16_t, std::vector<uint8_t>> byAddr;
	uint64_t dropped = 0;
};

// (bank, partial number within the note, byte within the slot) -> values
using Key = std::tuple<uint16_t, int, int>;

std::map<Key, std::vector<uint8_t>> normalise(const Capture &c) {
	std::map<Key, std::vector<uint8_t>> out;
	for (const auto &[addr, vals] : c.byAddr) {
		const uint16_t bank = addr & 0xFFC0;
		const int within = addr & 0x3F;
		const int slot = within / kBytesPerSlot;
		const int byteInSlot = within % kBytesPerSlot;
		// Writes to slots that do not belong to this note are dropped: the envelope bank keeps
		// playing out the previous note in parallel, and without this it spoils everything.
		const auto it = std::find(c.slots.begin(), c.slots.end(), slot);
		if (it == c.slots.end()) continue;
		out[{bank, int(it - c.slots.begin()), byteInSlot}] = vals;
	}
	return out;
}

Capture window(D110AudioProcessor &proc, int note, int velocity, double seconds) {
	const auto before = ramOf(proc);
	proc.getCore().startSoTrace();
	const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)}; // channel 2 = part 1
	proc.getCore().pushMidi(on, 3);
	render(proc, seconds);

	// The state table snapshot is TAKEN while the note is still sounding: after release the
	// slots are freed and there would be nothing left to name them by.
	const auto during = ramOf(proc);

	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	render(proc, 0.9);
	proc.getCore().stopSoTrace();

	Capture c;
	c.dropped = proc.getCore().soWritesDropped();
	c.writes = proc.getCore().takeSoWrites();
	for (const auto &w : c.writes) c.byAddr[w.addr].push_back(w.value);

	// Slots are taken FROM THE WRITES THEMSELVES, by bank 0x0C00. It is written only when a
	// voice is issued, unlike the envelope bank 0x0CC0, which keeps playing out earlier notes in
	// parallel - so the slots touched in it during this window are the slots of this note.
	//
	// The earlier criterion - "a state table entry moved away from 0x80" - worked only for slots
	// never used before, because the firmware NEVER returns a slot to 0x80: the ROM release loop
	// at 0x29BB does not touch the edc0 table (la32_interface.md). There are only 32 free slots,
	// the four stimuli took sixteen, and the chromatic run died after the fourth note. Waiting
	// longer did not help and could not.
	for (const auto &[addr, vals] : c.byAddr)
		if ((addr & 0xFFC0) == D110Core::kLa32TapBase)
			c.slots.push_back((addr & 0x3F) / kBytesPerSlot);
	c.slots.erase(std::unique(c.slots.begin(), c.slots.end()), c.slots.end());

	// The state table stays as a cross-check: while slots have not run out, both methods must
	// name the same thing, and a divergence is immediately visible.
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const int off2 = D110Core::kSlotStateTable + 2 * s;
		if (before[(size_t)off2] == D110Core::kSlotIdleValue &&
		    during[(size_t)off2] != D110Core::kSlotIdleValue)
			c.slotsByTable.push_back(s);
	}
	return c;
}

std::string show(const std::map<Key, std::vector<uint8_t>> &m, const Key &k, int maxShown = 5) {
	const auto it = m.find(k);
	if (it == m.end()) return "-";
	std::string s;
	int shown = 0;
	for (uint8_t v : it->second) {
		if (shown++ == maxShown) { s += " ..."; break; }
		char buf[8];
		std::snprintf(buf, sizeof buf, "%s%02X", s.empty() ? "" : " ", v);
		s += buf;
	}
	return s;
}

// How a register moves PER SEMITONE.
//
// Comparing whole streams declared bank 0x0CC0 dependent "on everything", and that is an
// artifact of length: runs have different numbers of updates, so the vectors differ even when
// their BEGINNINGS match element by element. And the beginnings do match. A set of values
// cannot settle such a question - a law is needed, and a series of consecutive notes gives it.
//
// Taken FIRST, before all other experiments, and that is not presentation order but a
// necessity: a slot is recognized by what the firmware issued, and it issues the never-used
// ones first. There are 32 of them, and each note takes two to four. If the chromatic run is
// taken after the four stimuli, the free ones run out at the fourth note - this happened three
// runs in a row, and neither a pause between notes nor a filter on the routine address helped,
// because they were not the cause.
void chromaticSweep(D110AudioProcessor &proc) {
	std::printf("\n=== хроматика: первое значение каждого регистра, ноты 60..72 ===\n");

	auto firstReal = [](const std::vector<uint8_t> &v) -> int {
		for (uint8_t x : v)
			if (x != 0xFF) return x;
		return -1;
	};

	std::vector<Key> watch;
	for (uint16_t bank : {0x0C00, 0x0C40, 0x0C80, 0x0CC0, 0x0D00})
		for (int p = 0; p < 2; ++p)
			for (int b = 0; b < kBytesPerSlot; ++b) watch.push_back({bank, p, b});

	std::printf("  нота |");
	for (const auto &k : watch)
		std::printf(" %04X.%d.%d", std::get<0>(k), std::get<1>(k), std::get<2>(k));
	std::printf("\n");

	std::map<Key, std::vector<int>> series;
	for (int note = 60; note <= 72; ++note) {
		const Capture cap = window(proc, note, 100, 0.35);
		const auto n = normalise(cap);
		std::printf("  %4d |", note);
		for (const auto &k : watch) {
			const auto it = n.find(k);
			const int v = (it == n.end()) ? -1 : firstReal(it->second);
			series[k].push_back(v);
			if (v < 0) std::printf("       -");
			else std::printf("      %02X", v);
		}
		std::printf("   слоты:");
		for (int s : cap.slots) std::printf(" %d", s);
		std::printf("\n");
	}

	std::printf("\n  шаг на полутон (разности подряд идущих нот):\n");
	for (const auto &k : watch) {
		const auto &s = series[k];
		bool anyMissing = false;
		for (int v : s) if (v < 0) anyMissing = true;
		if (anyMissing) continue;
		std::printf("    %04X.%d.%d :", std::get<0>(k), std::get<1>(k), std::get<2>(k));
		int total = 0;
		for (size_t i = 1; i < s.size(); ++i) {
			std::printf(" %+d", s[i] - s[i - 1]);
			total += s[i] - s[i - 1];
		}
		std::printf("   всего за октаву %+d\n", total);
	}
	std::printf("  Регистр, у которого сумма за октаву не ноль, следует за клавишей.\n");
}

} // namespace

// Modes. Exactly one experiment per launch, and that is forced: the firmware issues the
// never-used slots first, there are only 32 of them, and once they run out, resets on reuse
// start going into the same bank - indistinguishable here from issuing. Two experiments do
// not fit in one run, and trying to combine them spoiled the measurement three times.
enum class Mode { Sweep, Tone, Find, Grid };

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	Mode mode = Mode::Sweep;
	if (argc > 1 && std::strcmp(argv[1], "tone") == 0) mode = Mode::Tone;
	if (argc > 1 && std::strcmp(argv[1], "find") == 0) mode = Mode::Find;
	if (argc > 1 && std::strcmp(argv[1], "grid") == 0) mode = Mode::Grid;
	std::printf("режим: %s\n",
	            mode == Mode::Sweep ? "хроматика и четыре раздражителя"
	            : mode == Mode::Tone ? "точечные правки тембра"
	            : mode == Mode::Grid ? "сетка параметров партиала: Group+ и Bank+"
	                                 : "разведка страниц правки тембра");

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	std::printf("прошивка: %s\n", proc.getCore().isRunning() ? "работает" : "НЕТ");
	if (!proc.getCore().isRunning()) return 1;

	// The capture is narrowed to the LA32 window: at 0x021A the panel polling runs in parallel,
	// thousands of writes per second, and without the filter the ring would be filled by it alone.
	proc.getCore().setTraceFilter(D110Core::kLa32TapBase, D110Core::kLa32TapEnd);

	// The window is short on purpose: while the note is held, bank 0x0CC0 is updated without
	// stopping, and at two seconds the ring overflowed, losing thousands of writes.
	constexpr double kWindow = 0.5;

	std::printf("\n=== КОНТРОЛЬ: окно %.1f с БЕЗ ноты ===\n", kWindow);
	{
		proc.getCore().startSoTrace();
		render(proc, kWindow);
		proc.getCore().stopSoTrace();
		const auto w = proc.getCore().takeSoWrites();
		std::printf("  записей: %zu\n", w.size());
		if (!w.empty())
			std::printf("  !!! окно не молчит - всё ниже надо читать с поправкой на это\n");
	}

	// ---- survey of timbre edit pages ---------------------------------------------------
	// Looks for how PARTIAL parameters are reached at all. Assumes nothing about what the buttons
	// are for: presses a value and watches which timbre byte moved. The byte names the parameter
	// itself. A D-110 timbre is a 10-byte name, four more bytes of common part (two structures,
	// partial muting, envelope mode), and from byte 14 on come the four partials proper, 58
	// bytes each.
	//
	// No notes are played here, and that matters: the survey does not spend voice slots, so the
	// whole grid can be stepped through in one run, whereas register measurement runs into the 32
	// slots and requires one experiment per launch.
	// ---- whole partial parameter grid: Group+ selects the GROUP, Bank+ the parameter in it ------
	// The earlier survey only walked Group+ and found offsets +0, +8, +20, +23, +28, +41,
	// +47. These are not "seven partial parameters", as was recorded, but the FIRST parameters of
	// seven groups: checking against the timbre layout (munt, Structures.h) gives WG, P-ENV,
	// P-LFO, TVF, TVF-ENV, TVA, TVA-ENV - and exactly these names are stored as strings in the
	// firmware ROM. So of the thirty partial parameters seven had been measured, while waveform,
	// pulse width, ROM wave number and resonance - the ones that MUST reach the chip, since it
	// generates the wave itself - had never been checked.
	//
	// No notes are played here, so voice slots are not spent and the whole grid is captured in
	// one run. Within a group the page is not re-entered: Bank+ moves to the next parameter, and
	// each cell is compared with its own snapshot.
	if (mode == Mode::Grid) {
		std::printf("заводской сброс, чтобы отсчёт был от известного тембра...\n");
		proc.getCore().factoryReset();
		render(proc, 3.0);
		while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
		render(proc, 9.0);

		constexpr int kPartialBase = 14; // partial 1; bases are 58 apart, already measured
		std::printf("\n  группа | Bank+ | байт тембра | смещение в партиале | значение\n");
		for (int group = 0; group <= 6; ++group) {
			press(proc, "Exit", 2);
			press(proc, "Timbre");
			press(proc, "Edit");
			press(proc, "Edit");
			press(proc, "Part+", 1);
			if (group) press(proc, "Group+", group);
			render(proc, 0.4);

			for (int bank = 0; bank <= 8; ++bank) {
				const auto before = ramOf(proc);
				press(proc, "Number+", 3);
				render(proc, 0.4);
				auto after = ramOf(proc);
				bool moved = std::memcmp(&before[0x21E4], &after[0x21E4], 246) != 0;
				const char *dir = "+3";
				if (!moved) {
					// The value may have been at the upper stop - then "did not move" means
					// "nothing to add", not "no such parameter". Try the other direction.
					press(proc, "Number-", 3);
					render(proc, 0.4);
					after = ramOf(proc);
					moved = std::memcmp(&before[0x21E4], &after[0x21E4], 246) != 0;
					dir = "-3";
				}
				std::printf("  %6d | %5d | %s |", group, bank, dir);
				if (!moved) std::printf(" (ничего ни вверх, ни вниз)");
				for (int i = 0; i < 246; ++i) {
					const int off = 0x21E4 + i;
					if (before[(size_t)off] == after[(size_t)off]) continue;
					std::printf(" байт%d", i);
					if (i >= kPartialBase && i < kPartialBase + 58)
						std::printf(" = партиал1 +%d", i - kPartialBase);
					std::printf(" (%d->%d)", before[(size_t)off], after[(size_t)off]);
				}
				std::printf("\n");
				press(proc, "Bank+");
				render(proc, 0.3);
			}
		}
		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == Mode::Find) {
		std::printf("  общая часть тембра - байты 0..13, партиалы - с 14-го\n");
		std::printf("  Part+ | Group+ | сдвинувшийся байт тембра\n");
		for (int part = 0; part <= 4; ++part) {
			for (int group = 0; group <= 6; ++group) {
				press(proc, "Exit", 2);
				press(proc, "Timbre");
				press(proc, "Edit");
				press(proc, "Edit");
				if (part) press(proc, "Part+", part);
				if (group) press(proc, "Group+", group);
				render(proc, 0.4);
				const auto before = ramOf(proc);
				press(proc, "Number+", 3);
				render(proc, 0.5);
				auto after = ramOf(proc);
				// The value may have been at the upper stop - and then "nothing moved" would mean not
				// "the page is empty" but "nothing to add". The firmware memory lives between runs, and
				// earlier runs did add, so by now a stop is commonplace. Try the other direction.
				bool moved = std::memcmp(&before[0x21E4], &after[0x21E4], 246) != 0;
				const char *dir = "+";
				if (!moved) {
					press(proc, "Number-", 3);
					render(proc, 0.5);
					after = ramOf(proc);
					moved = std::memcmp(&before[0x21E4], &after[0x21E4], 246) != 0;
					dir = "-";
				}
				std::printf("  %5d | %6d | %s |", part, group, dir);
				int moves = 0;
				for (int i = 0; i < 246; ++i) {
					const int off = 0x21E4 + i;
					if (before[(size_t)off] == after[(size_t)off]) continue;
					if (moves++ < 4)
						std::printf(" байт%d(%d->%d)%s", i, before[(size_t)off],
						            after[(size_t)off], i >= 14 ? " <== ПАРТИАЛ" : "");
				}
				if (!moves) std::printf(" (ничего ни вверх, ни вниз)");
				std::printf("\n");
			}
		}
		proc.setPoweredOn(false);
		proc.releaseResources();
		std::printf("\nготово\n");
		return 0;
	}

	// ---- single timbre edit mode ---------------------------------------------------------
	// The "different timbre" stimulus is too coarse: it moves nine registers at once and does
	// not say which is for what. Here the timbre is NOT changed - one parameter inside it is
	// changed at a time, and compared with a measurement taken by the same note right before
	// the edit. The way into timbre editing was found earlier (plugin/audio_test.cpp): Exit, Exit ->
	// Timbre -> Edit opens the part parameters on the "Tone =" page, one more Edit drops into
	// editing the timbre itself, Group+ pages through it.
	if (mode == Mode::Tone) {
		// Pages are given as arguments: three or four fit in a run. A page costs two measurements,
		// a measurement costs four slots with a four-partial timbre, and there are 32 slots - after
		// that reuse begins, and there is nothing to measure with.
		// Inside timbre editing the PARTIAL is selected by Part+, and Group+ pages through the
		// parameters inside it; found by the survey (find mode, table in docs/la32_register_map.md).
		// Bank+ does not fit - it moves the cursor along the timbre name, which cost one run spent
		// on someone else's assumption instead of a measurement.
		//
		// Part+ 0 leaves the common part of the timbre: the name and two structures.
		// Edit address: partial (Part+), group (Group+) and parameter within the group (Bank+).
		// Bank+ appeared here after grid mode showed that Group+ leads to the FIRST parameter of
		// a group and does not step through parameters. Without it waveform, ROM wave number,
		// pulse width and resonance were unreachable, and these are exactly the parameters that
		// must reach the chip.
		const int partSteps = (argc > 2) ? std::atoi(argv[2]) : 0;
		const int groupSteps = (argc > 3) ? std::atoi(argv[3]) : 0;
		const int firstBank = (argc > 4) ? std::atoi(argv[4]) : 0;
		const int bankCount = (argc > 5) ? std::atoi(argv[5]) : 1;
		const int presses = (argc > 6) ? std::atoi(argv[6]) : 3;
		std::printf("партиал: Part+ x%d; группа: Group+ x%d; параметры: Bank+ с %d, числом %d;"
		            " шаг значения %d\n", partSteps, groupSteps, firstBank, bankCount, presses);

		// A factory reset is MANDATORY, and this was learned the hard way. The timbre lives in
		// firmware memory between runs, and there were many runs with Number+: structures went to
		// their stops, partials were switched on and off, and the timbre ended up in a state nobody
		// chose. In such a run "the register did not move" means nothing - maybe the parameter
		// really does not reach the chip, or maybe a partial is being edited that takes no part in
		// the current structure. They cannot be told apart if the starting point is unknown.
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

		struct Point { std::string name; int bankSteps; int presses; };
		// What exactly is on each page the probe does not assume: it shows which timbre bytes in
		// RAM moved, and the byte names the parameter itself.
		std::vector<Point> points;
		for (int p = 0; p < bankCount; ++p)
			points.push_back({"группа " + std::to_string(groupSteps) + ", параметр "
			                  + std::to_string(firstBank + p), firstBank + p, presses});
		for (const auto &pt : points) {
			press(proc, "Exit", 2);
			press(proc, "Timbre");
			press(proc, "Edit");
			press(proc, "Edit");
			if (partSteps) press(proc, "Part+", partSteps);
			if (groupSteps) press(proc, "Group+", groupSteps);
			if (pt.bankSteps) press(proc, "Bank+", pt.bankSteps);
			render(proc, 0.6);
			const auto ramBefore = ramOf(proc);

			const Capture base = window(proc, 60, 100, kWindow);

			press(proc, "Number+", pt.presses);
			render(proc, 0.8);
			const auto ramAfter = ramOf(proc);
			const Capture moved = window(proc, 60, 100, kWindow);

			std::printf("\n=== %s ===\n", pt.name.c_str());
			// Which bytes of the TIMBRE itself moved - that is the signature of the edit. The timbre of
			// part 1 lies in RAM at 0x21E4, 246 bytes long.
			std::printf("  сдвинулось в тембре (ОЗУ 0x21E4+):");
			int moves = 0;
			for (int i = 0; i < 246; ++i)
				if (ramBefore[(size_t)(0x21E4 + i)] != ramAfter[(size_t)(0x21E4 + i)]) {
					if (moves++ < 6)
						std::printf(" +%d(%d->%d)", i, ramBefore[(size_t)(0x21E4 + i)],
						            ramAfter[(size_t)(0x21E4 + i)]);
				}
			std::printf("%s\n", moves ? "" : "  НИЧЕГО - страница ничего не правит");
			std::printf("  слоты до: ");
			for (int s : base.slots) std::printf("%d ", s);
			std::printf("| после: ");
			for (int s : moved.slots) std::printf("%d ", s);
			std::printf("| потеряно %llu/%llu\n", (unsigned long long)base.dropped,
			            (unsigned long long)moved.dropped);

			if (base.slots.empty() || moved.slots.empty()) {
				std::printf("  слоты кончились - дальше в этом прогоне мерить нечего\n");
				break;
			}
			const auto nb = normalise(base), nm = normalise(moved);
			std::set<Key> keys;
			for (const auto *m : {&nb, &nm})
				for (const auto &[k, v] : *m) keys.insert(k);
			std::printf("  регистры, сдвинувшиеся ОТ ЭТОЙ правки:\n");
			int changed = 0;
			// Only the FIRST values are compared, not whole vectors. The envelope bank is updated while
			// the note sounds, and the number of updates differs from run to run - comparing whole
			// vectors declares it changed always, even when the beginnings match byte for byte. This trap
			// has already been fallen into twice here.
			auto head = [](const std::vector<uint8_t> &v) {
				return std::vector<uint8_t>(v.begin(),
				                            v.begin() + std::min<size_t>(v.size(), 4));
			};
			for (const auto &k : keys) {
				const auto ib = nb.find(k), im = nm.find(k);
				if (ib != nb.end() && im != nm.end() && head(ib->second) == head(im->second))
					continue;
				++changed;
				std::printf("    %04X.%d.%d : %-14s -> %s\n", std::get<0>(k), std::get<1>(k),
				            std::get<2>(k), show(nb, k).c_str(), show(nm, k).c_str());
			}
			if (!changed) std::printf("    (ни одного)\n");
		}
		proc.setPoweredOn(false);
		proc.releaseResources();
		std::printf("\nготово\n");
		return 0;
	}

	chromaticSweep(proc);

	struct Run { const char *label; Capture cap; };
	std::vector<Run> runs;

	std::printf("\n=== A: нота 60, громкость 100, тембр по умолчанию ===\n");
	runs.push_back({"A 60/100", window(proc, 60, 100, kWindow)});

	std::printf("=== B: нота 72, громкость 100 ===\n");
	runs.push_back({"B 72/100", window(proc, 72, 100, kWindow)});

	std::printf("=== C: нота 60, громкость 40 ===\n");
	runs.push_back({"C 60/40 ", window(proc, 60, 40, kWindow)});

	// Fourth stimulus: a DIFFERENT TIMBRE with the same note and volume. The way was found
	// earlier (plugin/audio_test.cpp): Exit, Exit -> Timbre -> Edit opens on the "Tone =" page,
	// and Number+ selects a different sound.
	std::printf("=== D: нота 60, громкость 100, ДРУГОЙ тембр ===\n");
	// The timbre is selected from the STOP, not as "plus seven from what it was". Firmware memory
	// lives between runs, so a relative selection drifts: in one run it was timbre 31, in the
	// next 38 - and that one takes four partials instead of two, and there was nothing to
	// compare with. From the lower stop the number is always the same.
	press(proc, "Exit", 2);
	press(proc, "Timbre");
	press(proc, "Edit");
	press(proc, "Number-", 40);
	press(proc, "Number+", 7);
	render(proc, 1.0);
	{
		const auto ram = ramOf(proc);
		std::printf("  тембр партии 1: группа %d, номер %d (было 0/17 у заводского)\n",
		            ram[0x2000], ram[0x2001]);
	}
	runs.push_back({"D 60/100 другой тембр", window(proc, 60, 100, kWindow)});

	for (const auto &r : runs) {
		std::printf("  %-22s: записей %5zu, потеряно %llu, слоты:", r.label,
		            r.cap.writes.size(), (unsigned long long)r.cap.dropped);
		for (int s : r.cap.slots) std::printf(" %d", s);
		if (r.cap.slots.empty()) std::printf(" (НИ ОДНОГО - слот не определился)");
		std::printf("   | по таблице состояний:");
		for (int s : r.cap.slotsByTable) std::printf(" %d", s);
		if (r.cap.slotsByTable.empty()) std::printf(" (пусто - свободные слоты кончились)");
		std::printf("%s\n", r.cap.slots == r.cap.slotsByTable ? "   СОВПАЛО" : "");
	}

	bool usable = true;
	for (const auto &r : runs)
		if (r.cap.slots.empty() || r.cap.dropped) usable = false;
	if (!usable)
		std::printf("\nНе у всех прогонов определились слоты или захват переполнялся -\n"
		            "таблица сравнения пропускается, чтобы её не приняли за результат.\n"
		            "Хроматика ниже от этого не зависит и всё равно снимается.\n");

	std::vector<std::map<Key, std::vector<uint8_t>>> norm;
	for (const auto &r : runs) norm.push_back(normalise(r.cap));

	std::set<Key> all;
	if (usable)
		for (const auto &m : norm)
			for (const auto &[k, v] : m) all.insert(k);

	if (usable)
	std::printf("\n=== что говорит каждый регистр (банк, партиал ноты, байт в слоте) ===\n");
	std::printf("  банк   пар байт | A              | B (высота)     | C (сила)       "
	            "| D (тембр)      | вывод\n");
	std::map<std::string, int> tally;
	for (const auto &k : all) {
		const bool have0 = norm[0].count(k) != 0;
		std::string verdict;
		if (!have0) verdict = "нет в основном прогоне";
		else {
			const auto &base = norm[0].at(k);
			const bool dPitch = !norm[1].count(k) || norm[1].at(k) != base;
			const bool dVel = !norm[2].count(k) || norm[2].at(k) != base;
			const bool dTone = !norm[3].count(k) || norm[3].at(k) != base;
			if (!dPitch && !dVel && !dTone) verdict = "не меняется ни от чего";
			else {
				if (dPitch) verdict += "ВЫСОТА ";
				if (dVel) verdict += "СИЛА ";
				if (dTone) verdict += "ТЕМБР ";
			}
		}
		tally[verdict]++;
		std::printf("  0x%04X  %d   %d  | %-14s | %-14s | %-14s | %-14s | %s\n",
		            std::get<0>(k), std::get<1>(k), std::get<2>(k),
		            show(norm[0], k).c_str(), show(norm[1], k).c_str(),
		            show(norm[2], k).c_str(), show(norm[3], k).c_str(), verdict.c_str());
	}

	if (usable) {
		std::printf("\n  сводка:\n");
		for (const auto &[v, n] : tally) std::printf("    %-28s %d\n", v.c_str(), n);
		std::printf("\n  Регистр, помеченный ровно одним свойством, назван этим свойством и\n"
		            "  ничем другим - остальные три раздражителя его не сдвинули. Помеченный\n"
		            "  несколькими требует ещё одного опыта, а не толкования.\n");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
