// What the factory patch bank actually does to the instrument.
//
// The user file `D110ORIG.MID` was parsed separately: 32 Roland DT1 messages, all in the
// range 0x060000-0x063E00, 8192 bytes of data - i.e. the whole Patch Memory, 64 patches
// of 128 bytes each. There is no timbre memory (0x080000, 64 x 256) in it. What is checked
// here is not the file contents but something else: does it reach the FIRMWARE, and what exactly does it change in its memory.
//
// Why we cannot just look at the before/after difference: the firmware writes continuously to
// its own work areas, and any RAM snapshot differs from the previous one on its own.
// So a CONTROL run of exactly the same length WITHOUT the import goes first - it gives the
// list of bytes that move by themselves. Everything that changes only in the second run is
// the bank's doing.
//
// Second control - the MIDI counters: the bank goes into the processor byte by byte at real
// MIDI speed (3125 bytes per second), and "nothing changed" with undelivered bytes would mean
// something quite different from the same result with delivered ones.
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

const char *kDefaultBank =
	"C:\\Users\\bd260\\Downloads\\D70\\factorypatches\\D110\\D110ORIG.MID";

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

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

std::vector<bool> changedMask(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	std::vector<bool> m(D110Core::kRamSize, false);
	for (int i = 0; i < D110Core::kRamSize; ++i) m[(size_t)i] = a[(size_t)i] != b[(size_t)i];
	return m;
}

// Contiguous runs are printed on one line: a memory bank is thousands of consecutive
// bytes, and a list of a thousand addresses would say nothing, whereas "0x????..0x????, N bytes"
// says it all.
int reportRuns(const std::vector<bool> &mask, const std::vector<uint8_t> &before,
               const std::vector<uint8_t> &after, const char *indent) {
	int runs = 0, total = 0;
	for (int i = 0; i < D110Core::kRamSize;) {
		if (!mask[(size_t)i]) { ++i; continue; }
		int j = i;
		while (j < D110Core::kRamSize && mask[(size_t)j]) ++j;
		const int len = j - i;
		total += len;
		if (runs < 24) {
			std::printf("%s0x%04X..0x%04X  %5d байт", indent, i, j - 1, len);
			if (len <= 8) {
				std::printf("   ");
				for (int k = i; k < j; ++k) std::printf(" %02X->%02X", before[(size_t)k],
				                                        after[(size_t)k]);
			}
			std::printf("\n");
		}
		++runs;
		i = j;
	}
	if (runs > 24) std::printf("%s... всего участков %d\n", indent, runs);
	std::printf("%sИТОГО изменившихся байт: %d в %d участках\n", indent, total, runs);
	return total;
}

// The first bytes of a patch record are its ASCII name, as with a timbre. Non-printable bytes
// are printed as dots so that an empty or garbage slot is visible and not mistaken for a string.
void dumpNames(const std::vector<uint8_t> &ram, int base, int stride, int count, int nameLen,
               const char *label) {
	std::printf("  %s (ОЗУ 0x%04X, шаг %d):\n", label, base, stride);
	for (int n = 0; n < count; ++n) {
		if (n % 4 == 0) std::printf("   ");
		std::printf(" %2d:\"", n + 1);
		for (int k = 0; k < nameLen; ++k) {
			const int off = base + stride * n + k;
			const uint8_t c = (off < D110Core::kRamSize) ? ram[(size_t)off] : 0;
			std::printf("%c", (c >= 0x20 && c < 0x7f) ? char(c) : '.');
		}
		std::printf("\"");
		if (n % 4 == 3) std::printf("\n");
	}
	std::printf("\n");
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	const juce::File bank(argc > 1 ? argv[1] : kDefaultBank);
	std::printf("банк: %s\n", bank.getFullPathName().toRawUTF8());
	if (!bank.existsAsFile()) {
		std::printf("файла нет - дальше идти незачем\n");
		return 1;
	}
	std::printf("размер файла: %lld байт\n\n", (long long)bank.getSize());

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	std::printf("прошивка: %s   движок: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.engineIsOpen() ? "открыт" : "НЕ ОТКРЫТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");

	// Cold start - the very state Roland calls "clear the memory" and after which the
	// factory data is supposed to be loaded over MIDI.
	std::printf("\nхолодный старт (WRITE/COPY при включении)...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);
	std::printf("  экран: \"%s\"\n", screen(proc).c_str());

	constexpr double kWindow = 20.0;

	// ---- CONTROL: what moves by itself, with no import at all -----------------------
	std::printf("\n=== КОНТРОЛЬ: окно %.0f с БЕЗ импорта ===\n", kWindow);
	const auto quietBefore = snapshot(proc);
	render(proc, kWindow);
	const auto quietAfter = snapshot(proc);
	const auto noise = changedMask(quietBefore, quietAfter);
	reportRuns(noise, quietBefore, quietAfter, "    ");

	// ---- import ------------------------------------------------------------------------
	std::printf("\n=== импорт банка ===\n");
	const uint64_t midiInBefore = proc.getCore().midiForwarded();
	const uint64_t midiOutBefore = proc.getCore().midiDelivered();
	const auto before = snapshot(proc);

	proc.importSysexBank(bank);
	// The bytes go into the processor at real MIDI speed - 3125 bytes per second - so the
	// window is taken with a margin relative to the file size, and delivery is still confirmed
	// by the counters below, not by a time calculation.
	render(proc, kWindow);

	const auto after = snapshot(proc);
	const uint64_t inN = proc.getCore().midiForwarded() - midiInBefore;
	const uint64_t outN = proc.getCore().midiDelivered() - midiOutBefore;
	std::printf("  байт принято в очередь: %llu, вдвинуто в процессор: %llu, потеряно: %llu\n",
	            (unsigned long long)inN, (unsigned long long)outN,
	            (unsigned long long)proc.getCore().midiDropped());
	if (inN == 0) {
		std::printf("  В очередь не попало НИ ОДНОГО байта - импорт не состоялся, и всё\n"
		            "  дальнейшее сравнение бессмысленно.\n");
		proc.setPoweredOn(false);
		proc.releaseResources();
		return 1;
	}
	if (outN < inN)
		std::printf("  (часть байтов ещё в очереди - окно короче, чем нужно этому файлу)\n");

	std::printf("  экран после импорта: \"%s\"\n", screen(proc).c_str());

	// ---- what changed ON TOP of the firmware's own noise --------------------------------
	std::printf("\n=== изменения от импорта (за вычетом того, что шевелится само) ===\n");
	auto changed = changedMask(before, after);
	for (int i = 0; i < D110Core::kRamSize; ++i)
		if (noise[(size_t)i]) changed[(size_t)i] = false;
	const int total = reportRuns(changed, before, after, "    ");

	std::printf("\n  Ожидание, если файл - это Patch Memory: один сплошной участок примерно\n"
	            "  в 8192 байта (64 патча по 128). Смещение этого участка в ОЗУ и есть\n"
	            "  ответ на вопрос, где прошивка держит память патчей - оно нигде не\n"
	            "  записано и до сих пор в этом проекте не измерялось.\n");

	(void)total;
	{
		int lo = -1, hi = -1;
		for (int i = 0; i < D110Core::kRamSize; ++i)
			if (changed[(size_t)i]) { if (lo < 0) lo = i; hi = i; }
		if (lo >= 0) std::printf("    (все изменения лежат в 0x%04X..0x%04X)\n", lo, hi);
	}

	// ---- where the bank actually ended up ------------------------------------------------
	// The "before and after" difference only answers WHAT changed, not WHERE the bank lies:
	// if the memory already held almost the same patches, the matching bytes will not show up
	// in the difference, and a whole region looks like a handful of small runs. So a direct
	// search follows: take the start of each patch record from the file and look for it in RAM.
	// The offsets found will name both the base and the stride themselves.
	std::printf("\n=== прямой поиск записей банка в памяти прошивки ===\n");
	juce::MemoryBlock raw;
	std::vector<std::vector<uint8_t>> records; // 128 bytes per patch
	if (bank.loadFileAsData(raw)) {
		const auto *p = static_cast<const uint8_t *>(raw.getData());
		const size_t n = raw.getSize();
		std::vector<uint8_t> blob;
		// Look for the signature "Roland, device 17, model D-110, DT1" and take the data without
		// the header, checksum and F7. A raw scan also works on .MID, because the sysex bytes lie
		// in the track consecutively.
		for (size_t i = 0; i + 8 < n; ++i) {
			if (p[i] == 0x41 && p[i + 1] == 0x10 && p[i + 2] == 0x16 && p[i + 3] == 0x12) {
				const size_t data = i + 7;
				if (data + 256 <= n) blob.insert(blob.end(), p + data, p + data + 256);
				i = data + 256;
			}
		}
		std::printf("  из файла извлечено %zu байт данных\n", blob.size());
		for (size_t o = 0; o + 128 <= blob.size(); o += 128)
			records.emplace_back(blob.begin() + o, blob.begin() + o + 128);
	}
	if (records.empty()) {
		std::printf("  не удалось разобрать файл - поиск невозможен\n");
	} else {
		// Twenty-four bytes are enough for a match not to be accidental: the name plus the
		// start of the parameters. Searching by the full 128 bytes is not possible - the firmware may store the
		// record tail differently, and then nothing would be found.
		constexpr int kNeedle = 24;
		int found = 0, firstAt = -1, prevAt = -1, stride = -1;
		bool strideStable = true;
		for (size_t r = 0; r < records.size(); ++r) {
			int at = -1;
			for (int i = 0; i + kNeedle <= D110Core::kRamSize; ++i)
				if (std::memcmp(&after[(size_t)i], records[r].data(), kNeedle) == 0) { at = i; break; }
			if (at < 0) continue;
			++found;
			if (firstAt < 0) firstAt = at;
			if (prevAt >= 0) {
				const int d = at - prevAt;
				if (stride < 0) stride = d;
				else if (stride != d) strideStable = false;
			}
			prevAt = at;
			if (r < 6 || r == records.size() - 1)
				std::printf("  патч %2zu найден по 0x%04X\n", r + 1, at);
		}
		std::printf("  найдено записей: %d из %zu\n", found, records.size());
		if (found == 0)
			std::printf("  => банк в память прошивки НЕ попал, хотя байты до процессора дошли.\n"
			            "     Значит прошивка его отвергла - смотреть надо на то, что она\n"
			            "     проверяет в сообщении, а не на доставку.\n");
		else
			std::printf("  => база 0x%04X, шаг %d%s\n", firstAt, stride,
			            strideStable ? "" : " (ШАГ НЕ ПОСТОЯНЕН - смотреть смещения выше)");

		if (found > 0) {
			std::printf("\n  имена патчей в памяти ДО импорта:\n");
			dumpNames(before, firstAt, stride > 0 ? stride : 128, 8, 10, "первые 8");
			std::printf("  имена патчей в памяти ПОСЛЕ импорта:\n");
			dumpNames(after, firstAt, stride > 0 ? stride : 128, 8, 10, "первые 8");
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
