// By what address and what value the firmware sets the reverb TYPE.
//
// The previous probe (plugin/so_trace_probe.cpp) closed the SO latch: the type does not touch it, the same thing
// is written at load for any of the eight types. From that it was concluded that the type
// goes out over the sound board bus 0x0C00-0x0D02. The service notes say that conclusion
// is wrong: the reverb chip IC5 has ITS OWN input from the processor - five data bits D1-D5
// and two strobes STB0/STB1, built from gate array outputs
// (STB0 = NOT(EXIO1 · WL), STB1 = NOT(EXIO2 · WL)). Details and where this comes from -
// docs/service_notes_findings.md.
//
// The EXIO1/EXIO2 addresses inside the array are not named on the schematic either, so they are NOT
// guessed here: the tap takes the whole free range 0x0400-0x0BFF at once
// (D110Core::kExtIoTapBase) and prints everything written there, together with the address
// of the subroutine.
//
// Design of the experiment:
//   * CONTROL BEFORE CONCLUSIONS. "Not a single write" is a negative result, and it cannot be
//     trusted until the same capture shows writes where they certainly exist. So
//     the whole boot is printed first: if the tap is dead, that is visible at once, and does not
//     get turned into a conclusion about the D-110.
//   * CONTROL RUN WITHOUT A TYPE CHANGE. The same key presses and the same note, but the type does not change.
//     Without it "the value changed" means nothing: it could have changed from the key
//     presses themselves or from the note.
//   * TWO STIMULI. After the type is set both a pause and a note are taken: if the chip
//     is programmed not at the moment of the edit but at the next voice allocation, the difference
//     appears only after the note.
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

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

std::vector<uint8_t> g_cgrom;

bool isCgrom(const juce::MemoryBlock &data) {
	if (data.getSize() != 4096) return false;
	const auto *p = static_cast<const uint8_t *>(data.getData());
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

// We take the note through the firmware, not directly into the engine: it would program the reverb
// in its own voice allocation, and not on MIDI arriving in someone else's half.
void playNote(D110AudioProcessor &proc, uint8_t note) {
	const uint8_t on[3] = {0x91, note, 100}; // channel 2 is part 1 in the factory layout
	const uint8_t off[3] = {0x81, note, 0};
	proc.getCore().pushMidi(on, 3);
	render(proc, 1.2);
	proc.getCore().pushMidi(off, 3);
	render(proc, 0.6);
}

// Summary of one capture window. Grouping is by the PAIR "address and subroutine", not by
// a single address: TWO different things write to 0x021A - panel scanning drives column strobes there
// hundreds of times a second, while a reverb edit writes once, - and values lumped into one line
// look like a single stream in which nothing can be told apart.
using Key = std::pair<uint16_t, uint16_t>; // port address and the address of the subroutine that wrote

struct Window {
	std::map<Key, std::set<uint8_t>> values;
	std::map<Key, size_t> hits;
	size_t count = 0;
	uint64_t dropped = 0;
};

Window collect(D110AudioProcessor &proc) {
	Window w;
	w.dropped = proc.getCore().soWritesDropped();
	for (const auto &e : proc.getCore().takeSoWrites()) {
		w.values[{e.addr, e.pc}].insert(e.value);
		++w.hits[{e.addr, e.pc}];
		++w.count;
	}
	return w;
}

void printWindow(const Window &w, const char *indent) {
	if (w.values.empty()) { std::printf("%s(ни одной записи)\n", indent); return; }
	for (const auto &[key, values] : w.values) {
		std::printf("%s0x%04X из ПЗУ %04X, %zu раз <-", indent, key.first, key.second,
		            w.hits.at(key));
		int shown = 0;
		for (uint8_t v : values) {
			if (shown++ == 12) { std::printf(" ...(всего %d)", int(values.size())); break; }
			std::printf(" %02X", v);
		}
		std::printf("\n");
	}
	if (w.dropped)
		std::printf("%s!!! захват потерял %llu записей - всё выше это нижняя граница\n",
		            indent, (unsigned long long)w.dropped);
}

// We go into Patch Edit to the Reverb Type page. The route was recorded by the d110_reverb_path probe:
// Patch -> Edit opens Name, then Group+ pages through the parameters (Name, Reverb Type,
// Reverb Time, Reverb Level), and Number+ changes the value.
void toReverbType(D110AudioProcessor &proc) {
	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Edit");
	press(proc, "Group+"); // Name -> Reverb Type
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);

	// Capture is switched on BEFORE power is applied: boot is the only place where the SO latch
	// is certainly written, and it doubles as the tap's health check.
	proc.getCore().startSoTrace();
	proc.setPoweredOn(true);
	render(proc, 10.0);
	std::printf("прошивка: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");

	// ---- CONTROL: is the tap alive at all ---------------------------------------------
	std::printf("\n=== КОНТРОЛЬ: всё, что записано во внешний ввод-вывод при загрузке ===\n");
	std::printf("  (перехват стоит на 0x0200-0x0201, 0x0280-0x0281 и 0x0400-0x0BFF)\n");
	{
		const Window boot = collect(proc);
		std::printf("  записей: %zu\n", boot.count);
		printWindow(boot, "    ");
		if (boot.count == 0) {
			std::printf("\n  Перехват не увидел НИ ОДНОЙ записи, даже защёлки SO, про которую\n"
			            "  известно, что при загрузке она пишется пять раз. Значит сломан\n"
			            "  захват, а не молчит прошивка. Дальше идти незачем.\n");
			proc.setPoweredOn(false);
			proc.releaseResources();
			return 1;
		}
	}

	// ---- type sweep ----------------------------------------------------------------
	// The type lives in RAM at 0x2D95 and changes with Number+ on its page; this has already been measured
	// by the d110_reverb_path probe, and here it is printed again alongside - so that "the RAM value
	// did not change" cannot be confused with "nothing was written to the chip".
	auto reverbTypeByte = [&proc] {
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		return ram[0x2D95];
	};

	std::printf("\n=== развёртка: восемь типов ревербератора ===\n");
	toReverbType(proc);
	std::printf("  страница: \"%s\"\n", screen(proc).c_str());
	press(proc, "Number-", 10); // to the lower stop, so the steps are predictable
	render(proc, 0.5);
	std::printf("  после спуска на упор: \"%s\"  ОЗУ 0x2D95 = %d\n",
	            screen(proc).c_str(), reverbTypeByte());

	std::map<Key, std::set<uint8_t>> perAddrAcrossTypes;
	for (int step = 0; step < 8; ++step) {
		proc.getCore().startSoTrace();
		press(proc, "Number+");
		render(proc, 1.0);
		const Window afterEdit = collect(proc);

		proc.getCore().startSoTrace();
		playNote(proc, 60);
		const Window afterNote = collect(proc);

		std::printf("\n  --- шаг %d: экран \"%s\"  ОЗУ 0x2D95 = %d ---\n",
		            step + 1, screen(proc).c_str(), reverbTypeByte());
		std::printf("    сразу после правки (%zu записей):\n", afterEdit.count);
		printWindow(afterEdit, "      ");
		std::printf("    после ноты (%zu записей):\n", afterNote.count);
		printWindow(afterNote, "      ");

		for (const auto &w : {afterEdit, afterNote})
			for (const auto &[key, values] : w.values)
				perAddrAcrossTypes[key].insert(values.begin(), values.end());
	}

	// ---- CONTROL RUN: the same presses and the same note, but the type does NOT change -------------
	// Without it "the values at this address differ" does not mean "the type sets them": they could have been set by
	// the presses themselves, the note, or just time.
	std::printf("\n=== КОНТРОЛЬ: те же действия, но без смены типа ===\n");
	press(proc, "Exit", 2);
	std::printf("  экран: \"%s\"  ОЗУ 0x2D95 = %d (дальше не меняется)\n",
	            screen(proc).c_str(), reverbTypeByte());
	std::map<Key, std::set<uint8_t>> perAddrControl;
	for (int step = 0; step < 8; ++step) {
		proc.getCore().startSoTrace();
		press(proc, "Group+"); // a press that edits nothing
		render(proc, 1.0);
		playNote(proc, 60);
		const Window w = collect(proc);
		for (const auto &[key, values] : w.values)
			perAddrControl[key].insert(values.begin(), values.end());
	}
	std::printf("  за восемь одинаковых проходов:\n");
	{
		Window c;
		c.values = perAddrControl;
		for (const auto &[key, values] : perAddrControl) c.hits[key] = values.size();
		printWindow(c, "    ");
	}

	// ---- answer --------------------------------------------------------------------------
	std::printf("\n=== ответ ===\n");
	// What decides is not the number of different values but the comparison with the control: an address written to ONLY
	// on a type change names the path unambiguously, however many bits it carries. The type is split across
	// two ports (bits 1-3 at 0x021A, bit 0 at 0x0800), and a requirement of "eight different values
	// at one address" would reject the right answer - the first edition of this probe did exactly that.
	std::printf("  порт  | из ПЗУ | значений при смене типа | при контроле | вывод\n");
	for (const auto &[key, values] : perAddrAcrossTypes) {
		const size_t ctrl = perAddrControl.count(key) ? perAddrControl.at(key).size() : 0;
		const char *verdict = "пишется и без смены типа";
		if (ctrl == 0) verdict = "<== ПИШЕТСЯ ТОЛЬКО ПРИ СМЕНЕ ТИПА";
		else if (values.size() > ctrl) verdict = "пишется всегда, но при смене типа шире";
		std::printf("  0x%04X |   %04X | %23zu | %12zu | %s\n", key.first, key.second,
		            values.size(), ctrl, verdict);
	}
	for (const auto &[key, values] : perAddrControl)
		if (!perAddrAcrossTypes.count(key))
			std::printf("  0x%04X |   %04X | %23d | %12zu | только в контроле\n",
			            key.first, key.second, 0, values.size());

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
