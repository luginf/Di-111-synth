// By what path the reverb type, time and level reach the BOSS chip.
//
// This has to be known in any case - both to emulate the chip itself someday, and
// to simply carry the settings over to the engine's existing reverb. The question is settled
// by measurement, because reading MAME gives a contradiction: `so_w` allots TWO bits to the reverb
// program number (A13/A14 of the chip's ROM, four programs in all), while the panel, going by
// values read off the screen (docs/factory_defaults.md), offers eight types plus OFF.
// So either the type is not the same as the program, or the rest goes somewhere else - for example to
// the analog board, as the label of bit 3 "R. SW. to analog board" hints.
//
// The probe assumes nothing about the menu layout: it presses buttons and PRINTS THE SCREEN after
// every step, and then on the page it finds it pages through the value and shows three things
// side by side - what is on screen, which RAM bytes moved by exactly the number of presses, and what was
// written to the SO register meanwhile.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

// Bytes that moved by exactly the number of presses are the signature of the parameter being edited. All other
// changed bytes are printed too, but separately: the firmware's screen buffer changes along with
// the parameter, and mistaking one for the other is easy.
void reportRamDelta(const std::vector<uint8_t> &before, const std::vector<uint8_t> &after,
                    int presses) {
	std::vector<int> exact, other;
	for (int i = 0; i < D110Core::kRamSize; ++i) {
		if (before[i] == after[i]) continue;
		const int d = int(after[i]) - int(before[i]);
		(d == presses ? exact : other).push_back(i);
	}
	std::printf("    ОЗУ: сдвиг ровно на %d в %d байтах", presses, int(exact.size()));
	for (size_t i = 0; i < exact.size() && i < 8; ++i)
		std::printf("  0x%04X %d->%d", exact[i], before[exact[i]], after[exact[i]]);
	std::printf("\n    ОЗУ: прочих изменившихся байт %d", int(other.size()));
	for (size_t i = 0; i < other.size() && i < 8; ++i)
		std::printf("  0x%04X %d->%d", other[i], before[other[i]], after[other[i]]);
	std::printf("\n");
}

void reportSo(D110AudioProcessor &proc) {
	std::printf("    SO: ");
	bool any = false;
	for (int v = 0; v < 256; ++v) {
		const uint64_t n = proc.getCore().soWrites(uint8_t(v));
		if (!n) continue;
		any = true;
		std::printf("%02X(прог %d, RSW %d)x%llu  ", v, (v >> 1) & 3, (v >> 3) & 1,
		            (unsigned long long)n);
	}
	if (!any) std::printf("ни одной записи");
	std::printf("\n");
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");

	// Factory reset at the START of the run. Otherwise the probe inherits memory from the previous
	// run, the parameter may already sit at its stop, and "the value did not change" means
	// nothing. It also repairs the patch name if the previous run wrote something into it.
	std::printf("\nзаводской сброс...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);
	press(proc, "Exit", 2);
	std::printf("исходный экран: \"%s\"\n", screen(proc).c_str());

	// CONTROL on the SO register. "Not a single write" is a negative result, and it cannot be trusted
	// until the same counter shows writes where they certainly exist. The MIDI
	// lamp is bit 0 of the same register, and it works, so the writes exist.
	std::printf("\n=== контроль: пишется ли SO вообще ===\n");
	reportSo(proc);

	std::printf("\n=== вход в Patch Edit ===\n");
	press(proc, "Patch");
	std::printf("  после Patch: \"%s\"\n", screen(proc).c_str());
	press(proc, "Edit");
	std::printf("  после Edit : \"%s\"\n", screen(proc).c_str());

	// Group+ moves from parameter to parameter (Name, Reverb Type, Reverb Time,
	// Reverb Level - read off the screen by the previous run), and Number+ changes the value.
	// Bank+ here moves the cursor inside the name, and does not page through pages: the first version of
	// this probe thought otherwise and edited the patch name instead of the parameters.
	constexpr int kPresses = 3;
	static const char *kParamName[] = {"Name", "Reverb Type", "Reverb Time", "Reverb Level"};
	for (int page = 0; page < 4; ++page) {
		const std::string before = screen(proc);
		const auto ramBefore = snapshot(proc);
		proc.getCore().resetTallies();
		press(proc, "Number+", kPresses);
		render(proc, 0.5);
		const auto ramAfter = snapshot(proc);
		std::printf("\n  ожидается %s\n    экран до   : \"%s\"\n    экран после: \"%s\"\n",
		            kParamName[page], before.c_str(), screen(proc).c_str());
		reportRamDelta(ramBefore, ramAfter, kPresses);
		reportSo(proc);
		press(proc, "Group+");
	}

	// Second control, and also the real question: is the chip reprogrammed on a
	// PATCH CHANGE, when the whole set of reverb settings changes at once.
	std::printf("\n=== смена патча целиком ===\n");
	press(proc, "Exit", 2);
	press(proc, "Patch");
	proc.getCore().resetTallies();
	const auto ramBefore = snapshot(proc);
	press(proc, "Number+", 4);
	render(proc, 1.0);
	const auto ramAfter = snapshot(proc);
	std::printf("  экран: \"%s\"\n", screen(proc).c_str());
	reportRamDelta(ramBefore, ramAfter, 4);
	reportSo(proc);

	// Did time and level reach the engine. The read path is the same as the one proven by the control
	// "wrote and read back" in d110_tone_clobber.
	std::printf("\n=== доехали ли время и уровень до движка ===\n");
	{
		const auto ram = snapshot(proc);
		uint8_t eng[8];
		std::memset(eng, 0xAA, sizeof eng);
		const uint32_t sysPacked = ((0x100000u & 0x7f0000u) >> 2)
		                         | ((0x100000u & 0x7f00u) >> 1) | (0x100000u & 0x7fu);
		proc.engineReadMemory(sysPacked, 4, eng);
		std::printf("    прошивка: тип %d  время %d  уровень %d\n",
		            ram[0x2D95], ram[0x2D96], ram[0x2D97]);
		std::printf("    движок  : режим %d  время %d  уровень %d\n", eng[1], eng[2], eng[3]);
		std::printf("    время %s, уровень %s\n",
		            eng[2] == ram[0x2D96] ? "СОВПАЛО" : "РАСХОДИТСЯ",
		            eng[3] == ram[0x2D97] ? "СОВПАЛО" : "РАСХОДИТСЯ");
	}

	// And is it audible. The level is set from the panel to 0 and to 7, and on each the TAIL
	// after note-off is measured - which is what the reverb adds. The overall level will not do:
	// the note itself would mask the difference.
	std::printf("\n=== слышна ли разница по хвосту ===\n");
	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Edit");
	press(proc, "Group+", 3); // Name -> Reverb Type -> Reverb Time -> Reverb Level
	for (int want : {0, 7}) {
		press(proc, "Number-", 8);
		press(proc, "Number+", want);
		render(proc, 0.5);
		const auto ram = snapshot(proc);

		proc.playNoteOnPartForTest(0, 60, 100);
		render(proc, 1.0);
		proc.playNoteOffOnPartForTest(0, 60);
		render(proc, 0.35);

		juce::AudioBuffer<float> block(2, kBlock);
		double sumSq = 0;
		int64_t n = 0;
		const auto begin = Clock::now();
		auto next = begin;
		while (std::chrono::duration<double>(Clock::now() - begin).count() < 1.2) {
			juce::MidiBuffer none;
			block.clear();
			proc.processBlock(block, none);
			for (int ch = 0; ch < 2; ++ch)
				for (int i = 0; i < kBlock; ++i) {
					const float v = block.getSample(ch, i);
					sumSq += double(v) * v;
					++n;
				}
			next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
			std::this_thread::sleep_until(next);
		}
		std::printf("    экран \"%s\"  ОЗУ %d  ->  СКЗ хвоста %.6f\n", screen(proc).c_str(),
		            ram[0x2D97], n ? std::sqrt(sumSq / double(n)) : 0.0);
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
