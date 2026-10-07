// Whether an EDIT sent as a system exclusive message reaches the firmware itself - and where
// exactly it lands in its memory.
//
// The mirror (D110Core::emitRegionSysex) has so far worked in one direction: the firmware
// edits its own memory, the bridge carries that to the sound engine. The extended editor goes
// the other way - it sends Roland DT1 into the firmware's MIDI IN, exactly as an external
// librarian does with a real unit - and there has been no measurement of this path yet.
// Hence three questions, and each needs an answer capable of showing a failure:
//
//   1. Does the firmware accept DT1 at all and put it in the right bytes? Checked against
//      TEMPORARY areas whose RAM addresses were already measured by other probes: a hit on a
//      known byte is both the result and its control.
//   2. Does it accept writes to patch, timbre and tone memory, or does Mem Protect forbid
//      that (factory value - ON)? The answer decides what the editor may offer at all.
//   3. Where in RAM does TONE memory live? The only area of the Roland map whose location is
//      not measured: the upper 16 KB of RAM in the factory state is all zeros, and it cannot
//      be found by content. But a name can be written there and the moved bytes observed.
//
// And incidentally a fourth, about convenience: which byte the firmware remembers the
// CURRENT PATCH NUMBER in, so the editor can switch to the needed patch with the panel's own
// buttons instead of inventing a patch change itself.
//
// At the end of the run a factory reset is done: the probe writes into the unit's real
// battery memory, shared with the plugin, and its marks must not be left there.
#include "Source/PluginProcessor.h"

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

// The bytes that really changed. Firmware work areas (0x2Dxx above the system area,
// 0x36xx - screen buffer, 0x39xx) move on their own between any two snapshots, so they are
// printed separately from hits on the expected place, not mixed in.
struct Diff {
	std::vector<int> at;
	bool hit = false;
};

Diff reportDiff(const std::vector<uint8_t> &before, const std::vector<uint8_t> &after,
                int expected, int length, const char *what) {
	Diff d;
	for (int i = 0; i < D110Core::kRamSize; ++i)
		if (before[i] != after[i]) d.at.push_back(i);

	int inRange = 0;
	for (int i : d.at)
		if (expected >= 0 && i >= expected && i < expected + length) ++inRange;
	d.hit = (expected < 0) ? !d.at.empty() : (inRange > 0);

	std::printf("    %s: изменившихся байт %d", what, int(d.at.size()));
	if (expected >= 0)
		std::printf(", из них в ожидаемом месте 0x%04X..0x%04X - %d %s", expected,
		            expected + length - 1, inRange, inRange ? "" : "  <-- НЕ ДОШЛО");
	std::printf("\n      ");
	for (size_t i = 0; i < d.at.size() && i < 14; ++i)
		std::printf("0x%04X %02X->%02X  ", d.at[i], before[d.at[i]], after[d.at[i]]);
	if (d.at.size() > 14) std::printf("... ещё %d", int(d.at.size()) - 14);
	std::printf("\n");
	return d;
}

// One edit by exclusive message - exactly as the editor will send it.
void sendDt1(D110AudioProcessor &proc, uint32_t address, int offset, const uint8_t *data,
             int length) {
	uint8_t msg[D110Core::kMaxSysexBytes];
	const int n = D110Core::buildDt1Message(address, offset, data, length, msg);
	if (n <= 0) { std::printf("    !!! сообщение не построено\n"); return; }
	proc.getCore().pushMidi(msg, n);
	render(proc, 1.2);   // bytes arrive at MIDI speed, the firmware needs time to parse them
}

void sendByte(D110AudioProcessor &proc, uint32_t address, int offset, uint8_t value) {
	sendDt1(proc, address, offset, &value, 1);
}

// Checking one area: snapshot, send, snapshot, report.
bool checkWrite(D110AudioProcessor &proc, const char *what, uint32_t address, int offset,
                const uint8_t *data, int length, int expectedRam) {
	std::printf("\n  %s   адрес %02X %02X %02X + %d\n", what, (address >> 16) & 0x7f,
	            (address >> 8) & 0x7f, address & 0x7f, offset);
	const auto before = snapshot(proc);
	sendDt1(proc, address, offset, data, length);
	const auto after = snapshot(proc);
	const Diff d = reportDiff(before, after, expectedRam, length, "ОЗУ");
	return d.hit;
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
	if (!proc.getCore().isRunning()) return 1;

	press(proc, "Exit", 2);
	std::printf("экран: \"%s\"\n", screen(proc).c_str());

	// --- 1. temporary areas: addresses are known, so this is the control ------------
	std::printf("\n=== 1. ВРЕМЕННЫЕ ОБЛАСТИ (адреса в ОЗУ уже измерены) ===\n");

	int passed = 0, total = 0;

	// Timbre Temporary, part 3, volume. 0x2000 + 2*16 + 8.
	{
		const uint8_t v = 0x55;
		++total;
		if (checkWrite(proc, "Timbre Temporary, партия 3, Output Level = 85",
		               D110Core::kSysexTimbreTemp, 2 * D110Core::kTimbreTempRecord + 8, &v, 1,
		               D110Core::kRamTimbreTemp + 2 * D110Core::kTimbreTempRecord + 8))
			++passed;
	}

	// Tone Temporary, part 2, name. 0x21E4 + 246.
	{
		const uint8_t name[10] = { 'P','R','O','B','E','T','O','N','E','2' };
		++total;
		if (checkWrite(proc, "Tone Temporary, партия 2, имя = PROBETONE2",
		               D110Core::kSysexToneTemp, D110Core::kToneRecord, name, 10,
		               D110Core::kRamToneTemp + D110Core::kToneRecord))
			++passed;
	}

	// Rhythm Setup, sixteenth entry, volume. 0x2090 + 16*4 + 1.
	{
		const uint8_t v = 0x40;
		++total;
		if (checkWrite(proc, "Rhythm Setup, запись 17, Output Level = 64",
		               D110Core::kSysexRhythmTemp, 16 * D110Core::kRhythmRecord + 1, &v, 1,
		               D110Core::kRamRhythmTemp + 16 * D110Core::kRhythmRecord + 1))
			++passed;
	}

	// System Area, partial reserve of part 1. 0x2D94 + 4.
	{
		const uint8_t v = 3;
		++total;
		if (checkWrite(proc, "System, Partial Reserve партии 1 = 3",
		               D110Core::kSysexSystem, 4, &v, 1, D110Core::kRamSystem + 4))
			++passed;
	}

	// --- 2. memory: it may be forbidden by Mem Protect ---------------------------
	std::printf("\n=== 2. ПАМЯТЬ ПАТЧЕЙ И ТЕМБРОВ (Mem Protect заводски ON) ===\n");

	// Timbre Memory, cell 6, Key Shift. 0x2994 + 5*8 + 2. The place was measured by content:
	// 128 records of 8 bytes, exactly between Tone Temporary and the system area.
	{
		const uint8_t v = 30;
		++total;
		if (checkWrite(proc, "Timbre Memory I-A6, Key Shift = 30",
		               D110Core::kSysexTimbres, 5 * D110Core::kTimbreRecord + 2, &v, 1,
		               D110Core::kRamTimbres + 5 * D110Core::kTimbreRecord + 2))
			++passed;
	}

	// Patch Memory, patch 4, name. 0x0000 + 3*128.
	{
		const uint8_t name[10] = { 'P','R','O','B','E',' ',' ',' ','0','4' };
		++total;
		if (checkWrite(proc, "Patch Memory 4, имя = PROBE   04",
		               D110Core::kSysexPatches, 3 * D110Core::kPatchRecord, name, 10,
		               D110Core::kRamPatches + 3 * D110Core::kPatchRecord))
			++passed;
	}

	// --- 3. tone memory: the place is NOT known, that is what is searched for ----------
	//
	// The only area of the Roland map that cannot be found by content: in a factory unit
	// it is empty, and the upper 16 KB of RAM are solid zeros. So it is searched for by writing:
	// two tones, two records apart, and the place of both is measured, not assumed.
	//
	// ALL occurrences must be searched, not the first: the incoming message also lies in the
	// firmware's receive buffer (0x39xx-0x3Axx), and the first match is always that. The first
	// version of this probe fell into exactly that and declared the buffer address the base.
	std::printf("\n=== 3. ПАМЯТЬ ТОНОВ: куда она ляжет? ===\n");
	{
		auto findAll = [](const std::vector<uint8_t> &ram, const uint8_t *pat, int len) {
			std::vector<int> hits;
			for (int i = 0; i + len <= D110Core::kRamSize; ++i)
				if (std::memcmp(ram.data() + i, pat, (size_t)len) == 0) hits.push_back(i);
			return hits;
		};

		const uint8_t name1[10] = { 'P','R','O','B','E','T','O','N','E','1' };
		std::printf("\n  Tone Memory 1, имя = PROBETONE1   адрес 08 00 00\n");
		const auto before = snapshot(proc);
		sendDt1(proc, D110Core::kSysexTones, 0, name1, 10);
		const auto after = snapshot(proc);
		reportDiff(before, after, -1, 10, "ОЗУ");
		const auto hits1 = findAll(after, name1, 10);
		for (int h : hits1)
			std::printf("    PROBETONE1 в ОЗУ по 0x%04X%s\n", h,
			            (h >= 0x3900 && h < 0x3C00) ? "   (приёмный буфер прошивки)" : "");

		// Second write, via a tone: if both differ by exactly 512 bytes, it is an array with a
		// stride of 256, i.e. tone memory, not a coincidence.
		const uint8_t name2[10] = { 'P','R','O','B','E','T','O','N','E','3' };
		std::printf("\n  Tone Memory 3, имя = PROBETONE3   адрес 08 04 00\n");
		sendDt1(proc, D110Core::kSysexTones, 2 * 256, name2, 10);
		render(proc, 0.5);
		const auto after2 = snapshot(proc);
		const auto hits2 = findAll(after2, name2, 10);
		for (int h : hits2)
			std::printf("    PROBETONE3 в ОЗУ по 0x%04X%s\n", h,
			            (h >= 0x3900 && h < 0x3C00) ? "   (приёмный буфер прошивки)" : "");

		bool measured = false;
		for (int a : hits1)
			for (int b : hits2)
				if (b - a == 2 * 256) {
					std::printf("    ИЗМЕРЕНО: RAM 0x%04X == SysEx 08 00 00, шаг 256 байт\n", a);
					measured = true;
				}
		if (!measured)
			std::printf("    пары с шагом 512 нет: память тонов либо не в этих 32 КБ, либо "
			            "запись в неё не принимается\n");
	}

	// --- 5. what the unit CALLS the four tone groups --------------------------
	//
	// In a timbre record the group is a number 0..3, and the sound engine understands them as
	// its own four banks (A, B, Memory, Rhythm). But the labels in the editor must be those the
	// unit itself shows, not borrowed from the MT-32, - so they are not guessed but read off
	// the display: the group of part 1 is set by exclusive message, the screen is read.
	std::printf("\n=== 5. ИМЕНА ЧЕТЫРЁХ ГРУПП ТОНОВ, снятые с индикатора ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "Timbre");   // screen showing the group and timbre number of part 1
		render(proc, 0.6);
		std::printf("  экран после Timbre: \"%s\"\n", screen(proc).c_str());
		for (int group = 0; group < 4; ++group) {
			sendByte(proc, D110Core::kSysexTimbreTemp, 0, uint8_t(group));   // part 1, group
			sendByte(proc, D110Core::kSysexTimbreTemp, 1, 0);                // and number 1
			render(proc, 0.8);
			std::printf("    группа %d -> \"%s\"\n", group, screen(proc).c_str());
		}
	}

	// --- 4. which byte the firmware remembers the current patch number in -------------
	std::printf("\n=== 4. НОМЕР ТЕКУЩЕГО ПАТЧА: каким байтом? ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "Patch");
		render(proc, 0.5);
		std::printf("  экран после Patch: \"%s\"\n", screen(proc).c_str());

		constexpr int kPresses = 3;
		const auto before = snapshot(proc);
		press(proc, "Number+", kPresses);
		render(proc, 0.8);
		const auto after = snapshot(proc);
		std::printf("  экран после Number+ x%d: \"%s\"\n", kPresses, screen(proc).c_str());
		std::vector<int> exact;
		for (int i = 0; i < D110Core::kRamSize; ++i)
			if (int(after[i]) - int(before[i]) == kPresses) exact.push_back(i);
		std::printf("  байтов, сдвинувшихся ровно на %d: %d", kPresses, int(exact.size()));
		for (size_t i = 0; i < exact.size() && i < 10; ++i)
			std::printf("   0x%04X %d->%d", exact[i], before[exact[i]], after[exact[i]]);
		std::printf("\n");

		// Bank+ on the D-110 pages through patches by eights - if so, the same byte moves by 8,
		// and then any of the 64 patches is at most eight presses away.
		const auto beforeBank = snapshot(proc);
		press(proc, "Bank+", 1);
		render(proc, 0.8);
		const auto afterBank = snapshot(proc);
		std::printf("  экран после Bank+: \"%s\"\n", screen(proc).c_str());
		for (int i : exact)
			std::printf("    0x%04X: %d -> %d (шаг %d)\n", i, beforeBank[i], afterBank[i],
			            int(afterBank[i]) - int(beforeBank[i]));
	}

	// --- 6. what the unit WRITES ON THE SCREEN about master tune -------------------
	//
	// The factory tune byte is 0x4A = 74, while the unit's screen shows 442. The scale
	// documented by Roland, 0..127 -> 432.1..457.6 Hz, gives about 447 for 74, i.e. disagrees
	// with the unit (which is why this value is not carried into the sound engine). So the scale
	// must not be computed but READ OFF: tune is set by exclusive message, the screen is read.
	std::printf("\n=== 6. ШКАЛА ОБЩЕЙ ПОДСТРОЙКИ, снятая с индикатора ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "System");
		render(proc, 0.8);
		std::printf("  экран после System: \"%s\"\n", screen(proc).c_str());
		for (int v : { 0, 32, 64, 74, 100, 127 }) {
			sendByte(proc, D110Core::kSysexSystem, 0, uint8_t(v));
			render(proc, 1.0);
			std::printf("    байт %3d -> \"%s\"\n", v, screen(proc).c_str());
		}
		sendByte(proc, D110Core::kSysexSystem, 0, 0x4A);   // restore the factory value
		render(proc, 0.8);
	}

	// Two tone memory cells that the probe labelled with its own names are returned to their
	// original state: a factory reset does NOT touch them - visible from the fact that after the
	// reset they stayed labelled - and the probe's marks must not be left in the unit's memory.
	{
		const uint8_t blank[10] = {};
		sendDt1(proc, D110Core::kSysexTones, 0, blank, 10);
		sendDt1(proc, D110Core::kSysexTones, 2 * 256, blank, 10);
	}

	// --- 7. what byte the unit calls Output Assign -------------------------
	//
	// On the MT-32 the sixth byte of a part record is Reverb Switch, and the editor's labels
	// were taken from there. But on the laminated D-110 card (Play Mode, Timbre Edit page) it is
	// not that but Output Assign - assignment to the individual outputs, which the MT-32 does not
	// have at all. Factory values do not settle the dispute: byte 6 is 1 (both "reverb on" and
	// "output 1"), byte 7 is 0.
	//
	// So we ask the unit: reach the Output Assign page and page the value, watching which byte
	// moves and what limit it reaches. The limit decides - a switch has two positions, an output
	// assignment nine.
	std::printf("\n=== 7. OUTPUT ASSIGN: КАКОЙ ЭТО БАЙТ И КАКОВ ЕГО ПРЕДЕЛ ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "Timbre");
		render(proc, 0.6);
		press(proc, "Edit");
		render(proc, 0.8);
		std::printf("  Timbre Edit: \"%s\"\n", screen(proc).c_str());

		// Pages are paged by Group+, and their order is from the card: Tone Select, Key Shift, Fine
		// Tune, Bender Range, Assign Mode, Output Assign. The order is not taken on trust - the
		// screen is printed at each step.
		for (int page = 1; page <= 5; ++page) {
			press(proc, "Group+");
			render(proc, 0.5);
			std::printf("    Group+ x%d: \"%s\"\n", page, screen(proc).c_str());
		}

		constexpr int kPresses = 3;
		const auto before = snapshot(proc);
		press(proc, "Number+", kPresses);
		render(proc, 0.8);
		const auto after = snapshot(proc);
		std::printf("  после Number+ x%d: \"%s\"\n", kPresses, screen(proc).c_str());
		std::printf("  байт 6 партии 1 (0x%04X): %d -> %d\n",
		            D110Core::kRamTimbreTemp + 6, before[D110Core::kRamTimbreTemp + 6],
		            after[D110Core::kRamTimbreTemp + 6]);
		std::printf("  байт 7 партии 1 (0x%04X): %d -> %d\n",
		            D110Core::kRamTimbreTemp + 7, before[D110Core::kRamTimbreTemp + 7],
		            after[D110Core::kRamTimbreTemp + 7]);
		for (int i = 0; i < D110Core::kRamSize; ++i)
			if (int(after[i]) - int(before[i]) == kPresses && i < 0x2D94)
				std::printf("    сдвиг ровно на %d: 0x%04X  %d -> %d\n", kPresses, i,
				            before[i], after[i]);

		// To the stop: how many positions this parameter has in all. Twenty presses is certainly
		// more than either of the two supposed limits.
		press(proc, "Number+", 20);
		render(proc, 1.0);
		const auto atTop = snapshot(proc);
		std::printf("  на упоре: \"%s\"   байт 6 = %d, байт 7 = %d\n", screen(proc).c_str(),
		            atTop[D110Core::kRamTimbreTemp + 6], atTop[D110Core::kRamTimbreTemp + 7]);
	}

	std::printf("\n=== ИТОГ: %d из %d записей дошли ===\n", passed, total);

	// The probe wrote into the unit's real battery memory, shared with the plugin. Cleaning up.
	std::printf("\nзаводской сброс, чтобы не оставлять свои метки в памяти прибора...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 10.0);
	press(proc, "Exit", 2);
	std::printf("экран после сброса: \"%s\"\n", screen(proc).c_str());

	proc.setPoweredOn(false);
	std::printf("готово\n");
	return 0;
}
