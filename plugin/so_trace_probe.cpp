// When the firmware touches the SO register, and which subroutine does it.
//
// SO is the only path seen from the processor to the BOSS reverb chip: bits 1-2
// are A13/A14 of its ROM, i.e. the program number, bit 3 is "R. SW." to the analog board
// (roland_d10.cpp, so_w). The previous probe (d110_reverb_path) showed that when editing
// Reverb Type the register is not written AT ALL, and that over the whole boot there are only four writes.
// So the moment of reprogramming is some other one, and it has to be found: without it there is nothing
// to feed to a future emulation of the chip, and it is not even known whether
// the program number accounts for the reverb type at all.
//
// The instrument is run through states in each of which the chip can reasonably be expected to be
// reprogrammed, and for each the writes to SO are printed together with the address they
// were made from. The address matters more than the value: it gives an entry point for the disassembler.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

std::vector<uint8_t> g_cgrom;

bool isCgrom(const juce::MemoryBlock &d) {
	if (d.getSize() != 4096) return false;
	const auto *p = static_cast<const uint8_t *>(d.getData());
	static const uint8_t kA[7] = {0x0e, 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11};
	for (int r = 0; r < 7; ++r)
		if ((p[16 * 0x41 + r] & 0x1f) != kA[r]) return false;
	return true;
}

void loadCgrom() {
	for (const auto &e : juce::RangedDirectoryIterator(D110AudioProcessor::getAutoRomFolder(),
	                                                   true, "*", juce::File::findFiles)) {
		juce::MemoryBlock d;
		if (e.getFile().loadFileAsData(d) && isCgrom(d)) {
			g_cgrom.assign(static_cast<const uint8_t *>(d.getData()),
			               static_cast<const uint8_t *>(d.getData()) + 4096);
			return;
		}
	}
}

char decodeCell(const uint8_t *rows) {
	if (g_cgrom.empty()) return '?';
	for (int code = 0x20; code < 0x80; ++code) {
		bool same = true;
		for (int r = 0; r < 7; ++r)
			if ((g_cgrom[(size_t)16 * code + r] & 0x1f) != (rows[r] & 0x1f)) { same = false; break; }
		if (same) return char(code);
	}
	return '?';
}

std::string screen(D110AudioProcessor &proc) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return "(нет экрана)";
	std::string s;
	for (int row = 0; row < 2; ++row) {
		if (row) s += " / ";
		for (int col = 0; col < D110Core::kCols; ++col)
			s.push_back(decodeCell(rows + ((size_t)row * D110Core::kCols + col)
			                       * D110Core::kRowsPerChar));
	}
	return s;
}

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer midi = {}) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	bool first = true;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer m;
		if (first) { m = midi; first = false; }
		block.clear();
		proc.processBlock(block, m);
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

// Writes over the elapsed interval, collapsed by the pair "address + value". Single lines
// would drown in repeats: the MIDI lamp sits in bit 0 of the same register and blinks often.
void report(D110AudioProcessor &proc, const char *what) {
	const auto writes = proc.getCore().takeSoWrites();
	const uint64_t dropped = proc.getCore().soWritesDropped();
	std::printf("\n--- %s ---\n", what);
	if (writes.empty()) {
		std::printf("    записей в SO нет (потеряно %llu)\n", (unsigned long long)dropped);
		return;
	}
	std::map<std::tuple<uint16_t, uint16_t, uint8_t>, int> tally;
	for (const auto &w : writes) ++tally[{w.addr, w.pc, w.value}];
	std::printf("    записей %zu, потеряно %llu, первая на %.0f мс\n", writes.size(),
	            (unsigned long long)dropped, writes.front().ms);
	for (const auto &kv : tally) {
		const uint16_t addr = std::get<0>(kv.first);
		const uint16_t pc = std::get<1>(kv.first);
		const uint8_t v = std::get<2>(kv.first);
		std::printf("      по 0x%04X из 0x%04X  значение %02X"
		            "  (программа %d, R.SW %d, лампа %d)  x%d\n",
		            addr, pc, v, (v >> 1) & 3, (v >> 3) & 1, v & 1, kv.second);
	}
}

// Accesses to addresses the memory map does not cover, collapsed by address. This is how the LA32
// interface was found in its day: what the MAME model lacks is visible only here. If the reverb
// type goes to the chip not through SO but through a port the driver does not know,
// it will show up as an address appearing exactly on the type edit and not appearing at rest.
void reportUnmapped(D110AudioProcessor &proc, const char *what) {
	const auto lines = proc.getCore().takeLogLines();
	std::map<std::string, int> byAddr;
	for (const auto &l : lines) {
		// MAME format: "...unmapped program memory write to 1234 = 56 & FF"
		const auto to = l.find(" to ");
		if (to == std::string::npos) continue;
		const bool write = l.find("write") != std::string::npos;
		std::string addr = l.substr(to + 4, 4);
		byAddr[(write ? "W " : "R ") + addr] += 1;
	}
	std::printf("    неотображённые обращения: строк %zu, различных адресов %zu\n",
	            lines.size(), byAddr.size());
	int shown = 0;
	for (const auto &kv : byAddr) {
		std::printf("      %s x%d\n", kv.first.c_str(), kv.second);
		if (++shown >= 14) { std::printf("      ...\n"); break; }
	}
	(void)what;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().startSoTrace();
	// Set BEFORE the machine starts: the unmapped-access hook is installed once
	// when the devices are parsed and will not appear later.
	proc.getCore().setLogUnmapped(true);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s\n", proc.getCore().isRunning() ? "работает" : "НЕТ");
	report(proc, "загрузка");

	render(proc, 3.0);
	report(proc, "простой 3 с");
	reportUnmapped(proc, "простой 3 с");

	// The MIDI MESSAGE lamp. BOTH directions are checked: that it lights on a byte stream
	// and that it goes out afterwards. A check only for lighting is not enough - a lamp stuck on forever
	// would pass it just as well as a healthy one.
	{
		std::printf("\n--- лампа MIDI MESSAGE ---\n");
		std::printf("    до подачи MIDI: %s\n", proc.getCore().midiLampOn() ? "ГОРИТ" : "погашена");
		// Samples are taken OFTEN and inside the stream, not one per note: the lamp's hold time
		// is comparable to the gap between notes, and one sample per note always lands in
		// the same phase - the first version of this check measured 1 of 12 on a
		// healthy lamp.
		int litDuring = 0, samplesDuring = 0;
		for (int i = 0; i < 12; ++i) {
			juce::MidiBuffer m;
			m.addEvent(juce::MidiMessage::noteOn(2, 60 + (i % 5), 0.8f), 0);
			m.addEvent(juce::MidiMessage::noteOff(2, 60 + (i % 5)), 200);
			for (int k = 0; k < 10; ++k) {
				render(proc, 0.01, k == 0 ? m : juce::MidiBuffer{});
				if (proc.getCore().midiLampOn()) ++litDuring;
				++samplesDuring;
			}
		}
		std::printf("    во время потока MIDI: горела в %d из %d замеров\n", litDuring,
		            samplesDuring);
		render(proc, 0.5);
		std::printf("    через 0.5 с тишины: %s\n",
		            proc.getCore().midiLampOn() ? "ГОРИТ - не гаснет" : "погашена");
	}

	// A note from the host. The MIDI lamp is bit 0 of the same register, so writes here MUST
	// exist; this is a health check of the capture, not only a measurement.
	{
		juce::MidiBuffer m;
		m.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
		render(proc, 1.5, m);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
		render(proc, 1.5, off);
	}
	report(proc, "нота с хоста (контроль: лампа сидит в бите 0)");

	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Number+", 3);
	render(proc, 1.5);
	std::printf("\nэкран после смены патча: \"%s\"\n", screen(proc).c_str());
	report(proc, "смена патча");
	reportUnmapped(proc, "смена патча");

	// The reverb type is changed, and then a note is played: if the chip is
	// reprogrammed lazily, at the first sound after the edit, we will see it here.
	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Edit");
	press(proc, "Group+");
	press(proc, "Number+", 3);
	render(proc, 1.0);
	std::printf("\nэкран после правки типа: \"%s\"\n", screen(proc).c_str());
	report(proc, "правка Reverb Type");
	reportUnmapped(proc, "правка Reverb Type");
	{
		juce::MidiBuffer m;
		m.addEvent(juce::MidiMessage::noteOn(2, 64, 0.9f), 0);
		render(proc, 1.5, m);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 64), 0);
		render(proc, 1.5, off);
	}
	report(proc, "первая нота ПОСЛЕ правки типа");

	// Demo song: the densest stream of notes and setting changes the instrument produces on its own.
	press(proc, "Exit", 2);
	press(proc, "Edit");
	press(proc, "Enter");
	press(proc, "Enter");
	render(proc, 12.0);
	std::printf("\nэкран демо: \"%s\"\n", screen(proc).c_str());
	report(proc, "демо-песня, 12 с");

	// The only write to SO after boot came from 0x2D28, and its value between two
	// runs turned out different - 04 and 00 - with a different stored reverb type.
	// If this is not a coincidence, then the type DOES select the ROM program after all, it is just applied
	// once at power-on. Checked directly: set the type, power off, power on and
	// see what was written. The type lives in battery-backed RAM and survives power-off.
	std::printf("\n\n=== тип ревербератора -> номер программы, через выключение ===\n");
	for (int type = 1; type <= 8; ++type) {
		press(proc, "Exit", 2);
		press(proc, "Patch");
		press(proc, "Edit");
		press(proc, "Group+");           // Name -> Reverb Type
		press(proc, "Number-", 10);      // to the lower stop
		press(proc, "Number+", type - 1);
		render(proc, 0.6);
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		const std::string shown = screen(proc);
		const uint8_t stored = ram[0x2D95];

		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		proc.getCore().startSoTrace();
		proc.setPoweredOn(true);
		render(proc, 9.0);

		const auto writes = proc.getCore().takeSoWrites();
		std::printf("\n  тип на экране \"%s\"  ОЗУ 0x2D95 = %d\n", shown.c_str(), stored);
		if (writes.empty()) {
			std::printf("    при загрузке записей в SO нет\n");
		} else {
			for (const auto &w : writes)
				std::printf("    по 0x%04X из 0x%04X значение %02X -> программа %d, R.SW %d\n",
				            w.addr, w.pc, w.value, (w.value >> 1) & 3, (w.value >> 3) & 1);
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
