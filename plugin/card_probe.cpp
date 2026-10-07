// How the D-110 firmware finds out whether a memory card is inserted - and whether it really does so this way.
//
// The ROM analysis (routine 0x770A, written out in full in the comment on D110Core::kCardSize)
// says: the machine has no separate "card present" line at all. The firmware WRITES to the card the
// complement of what it read there, reads the same place back and checks whether it changed.
// Unchanged and read as 0xFF - the slot is empty.
//
// The analysis is a hypothesis, and here it is tested by what the firmware itself says. FOUR
// different cards are fed to it, each differing in exactly one property, and what it said about
// each is captured from the screen:
//
//   empty slot (whole card 0xFF)        expected "Card Not Ready"
//   card of zeros (as MAME loads it)    expected "Illegal Card" - the write goes through,
//                                       but there is no signature
//   formatted card                      expected to work with no error message
//   the same, write-protected           expected "Memory Card Write Protected"
//
// Four different answers to four fed cards is the control: "Card Not Ready" alone on an empty
// slot would prove nothing, because a menu that simply does not work would look the same.
//
// The first argument is the mode:
//   explore <button> ...  press the listed buttons, printing the screen after each. This is how
//                         the "Save to Card" page itself is found, without guessing the menu layout.
//   cards                 the four-part experiment above.
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

bool press(D110AudioProcessor &proc, const std::string &name, int times = 1) {
	for (const auto &b : kButtons)
		if (name == b.name) {
			const int idx = D110Core::buttonIndex(b.port, b.bit);
			for (int i = 0; i < times; ++i) {
				proc.getCore().setButton(idx, true);
				render(proc, 0.13);
				proc.getCore().setButton(idx, false);
				render(proc, 0.30);
			}
			return true;
		}
	std::printf("  !!! нет такой кнопки: %s\n", name.c_str());
	return false;
}

// Signature of a formatted card - twelve bytes from ROM 0x7804, followed by the card type and its
// complement (the firmware requires exactly an X / ~X pair, routine 0x7785).
void fillFormatted(std::vector<uint8_t> &card) {
	card.assign(D110Core::kCardSize, 0x00);
	static const char kSig[] = "Roland D-10 ";
	std::memcpy(card.data(), kSig, 12);
	card[0x0c] = 'D';
	card[0x0d] = uint8_t(~'D');
	card[0x0e] = 'X';
	card[0x0f] = uint8_t(~'X');
	// Bit 0 of the last byte is write protection; zero means "protected".
	card[D110Core::kCardSize - 1] = 0xff;
}

// One fed card: load it, get to the card page, press ENTER and capture the firmware's answer
// from the screen.
void tryCard(D110AudioProcessor &proc, const char *what, const std::vector<uint8_t> &card,
             bool inserted, const std::vector<std::string> &path) {
	proc.getCore().setCardImage(card.data());
	proc.getCore().setCardInserted(inserted);
	render(proc, 0.6);

	press(proc, "Exit", 3);
	for (const auto &b : path) press(proc, b);
	const std::string before = screen(proc);
	press(proc, "Enter");
	render(proc, 1.0);
	// ENTER only asks "Sure?", and WRITE/COPY executes - the confirmation here is its, and this
	// was found by trying buttons, not by guessing the menu.
	const std::string asked = screen(proc);
	press(proc, "Write");

	std::printf("  %s\n    до ENTER      : \"%s\"\n    после ENTER   : \"%s\"\n",
	            what, before.c_str(), asked.c_str());
	// The firmware clears the error message itself after a couple of seconds, so the screen is
	// recorded as a series: a single snapshot "after so many seconds" simply misses it.
	std::string last = asked;
	for (int t = 0; t < 20; ++t) {
		const std::string s = screen(proc);
		if (s != last) { std::printf("    %4.1f с ОТВЕТ: \"%s\"\n", t * 0.15, s.c_str()); last = s; }
		render(proc, 0.15);
	}
	press(proc, "Exit", 3);
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	const std::string mode = argc > 1 ? argv[1] : "explore";

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");
	press(proc, "Exit", 3);
	std::printf("исходный экран: \"%s\"\n", screen(proc).c_str());

	if (mode == "explore") {
		// The screen is captured not once but as a series: the firmware holds card error messages for
		// a couple of seconds and clears them itself, and a single snapshot "after so many seconds"
		// simply misses them.
		for (int i = 2; i < argc; ++i) {
			// A card is a user action just like a button press, and in the step breakdown it belongs
			// in the same list.
			// How many bytes of battery RAM differ from the previous mark. "Complete" on the screen only
			// says the operation ran to the end; whether memory changed because of it is a separate
			// question, answered by the byte count, not by the message.
			if (std::strcmp(argv[i], "RAMDIFF") == 0) {
				static std::vector<uint8_t> markRam;
				std::vector<uint8_t> now(D110Core::kRamSize, 0);
				proc.getCore().getRam(now.data());
				if (markRam.empty()) std::printf("  RAMDIFF  : отметка поставлена\n");
				else {
					size_t diff = 0;
					for (size_t k = 0; k < now.size(); ++k) if (now[k] != markRam[k]) ++diff;
					std::printf("  RAMDIFF  : разошлось байт: %zu\n", diff);
				}
				markRam = now;
				continue;
			}
			if (std::strcmp(argv[i], "EJECT") == 0 || std::strcmp(argv[i], "INSERT") == 0) {
				proc.getCore().setCardInserted(std::strcmp(argv[i], "INSERT") == 0);
				render(proc, 0.6);
				std::printf("  %-8s : \"%s\"\n", argv[i], screen(proc).c_str());
				continue;
			}
			press(proc, argv[i]);
			std::printf("  после %-8s :\n", argv[i]);
			std::string last;
			for (int t = 0; t < 24; ++t) {
				const std::string s = screen(proc);
				if (s != last) { std::printf("      %5.1f с  \"%s\"\n", t * 0.15, s.c_str()); last = s; }
				render(proc, 0.15);
			}
		}
		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == "roundtrip") {
		// A full circle, as the owner would go through it: clean card - format - write -
		// eject - put back - read. Each step is confirmed by what the firmware itself said, and by a
		// snapshot of the card taken from the plugin.
		auto watch = [&](const char *what, double seconds) {
			std::string last;
			const int steps = int(seconds / 0.15);
			for (int t = 0; t < steps; ++t) {
				const std::string s = screen(proc);
				if (s != last) { std::printf("    %-18s %4.1f с \"%s\"\n", what, t * 0.15, s.c_str()); last = s; }
				render(proc, 0.15);
			}
		};
		std::vector<uint8_t> card(D110Core::kCardSize, 0xff);
		std::vector<uint8_t> seen(D110Core::kCardSize, 0);

		std::printf("\n=== 1. чистая карта в гнездо, Save to Card ===\n");
		proc.getCore().setCardImage(card.data());
		proc.getCore().setCardInserted(true);
		render(proc, 0.6);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch("после WRITE", 2.5);

		std::printf("\n=== 2. согласиться на форматирование ===\n");
		press(proc, "Enter"); // ENTER confirms here, not WRITE - found by trying buttons
		watch("формат", 6.0);
		proc.getCore().getCardImage(seen.data());
		std::printf("    подпись на карте: \"%.12s\"  тип %02X/%02X %02X/%02X  байт 0x7FFF=%02X\n",
		            reinterpret_cast<char *>(seen.data()), seen[0x0c], seen[0x0d], seen[0x0e],
		            seen[0x0f], seen[D110Core::kCardSize - 1]);

		std::printf("\n=== 3. записать звуки на карту ===\n");
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch("запись", 8.0);
		proc.getCore().getCardImage(seen.data());
		size_t nonzero = 0;
		for (size_t i = 0x10; i < seen.size(); ++i) if (seen[i] != 0x00 && seen[i] != 0xff) ++nonzero;
		std::printf("    байт с данными на карте: %zu\n", nonzero);

		std::printf("\n=== 4. вынуть карту и попробовать читать без неё ===\n");
		proc.getCore().setCardInserted(false);
		render(proc, 0.6);
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Group+"); press(proc, "Enter"); press(proc, "Write");
		// Reading from the card writes to internal memory, which is protected, so the firmware first
		// asks "MemProtected / Turn off once ?" - agree and carry on.
		render(proc, 0.6);
		press(proc, "Enter"); press(proc, "Write");
		watch("без карты", 3.0);

		std::printf("\n=== 5. вернуть её и прочитать ===\n");
		proc.getCore().setCardInserted(true);
		render(proc, 0.6);
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Group+"); press(proc, "Enter"); press(proc, "Write");
		render(proc, 0.6);
		press(proc, "Enter"); press(proc, "Write");
		watch("чтение", 8.0);

		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == "persist") {
		// The card is a medium, so it must survive power-off of the unit, and the slot must remember
		// whether the card was taken out or not. The marker is put in a place the firmware does not
		// care about, so it cannot be confused with what the unit wrote.
		std::vector<uint8_t> card(D110Core::kCardSize, 0x00);
		fillFormatted(card);
		static const char kMark[] = "MARK-2026";
		std::memcpy(card.data() + 0x100, kMark, sizeof kMark);
		proc.getCore().setCardImage(card.data());
		proc.getCore().setCardInserted(true);
		render(proc, 1.0);

		std::vector<uint8_t> seen(D110Core::kCardSize, 0);
		auto report = [&](const char *when) {
			proc.getCore().getCardImage(seen.data());
			std::printf("  %-26s подпись \"%.12s\"  метка \"%.9s\"  гнездо: %s\n", when,
			            reinterpret_cast<char *>(seen.data()),
			            reinterpret_cast<char *>(seen.data() + 0x100),
			            proc.getCore().cardInserted() ? "карта на месте" : "пусто");
		};
		report("карта вставлена:");

		std::printf("\n=== выключить и включить ===\n");
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.setPoweredOn(true);
		render(proc, 9.0);
		report("после включения:");

		std::printf("\n=== вынуть карту, выключить и включить ===\n");
		proc.getCore().setCardInserted(false);
		render(proc, 1.0);
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.setPoweredOn(true);
		render(proc, 9.0);
		report("после включения:");
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		std::string last;
		for (int t = 0; t < 20; ++t) {
			const std::string s = screen(proc);
			if (s != last) { std::printf("    Save to Card: \"%s\"\n", s.c_str()); last = s; }
			render(proc, 0.15);
		}

		std::printf("\n=== вернуть карту ===\n");
		proc.getCore().setCardInserted(true);
		render(proc, 1.0);
		report("вставлена обратно:");

		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == "loadverify") {
		// "Complete" on the screen only says the operation ran to the end. That reading from the
		// card really RETURNS the memory is shown only by comparing three snapshots: right after
		// writing to the card, after an edit in the unit, and after reading back. The third must
		// match the first, and the second must differ from it. Without the second the experiment would
		// mean nothing: the snapshots would match for a read that does nothing.
		auto snap = [&] {
			std::vector<uint8_t> v(D110Core::kRamSize, 0);
			proc.getCore().getRam(v.data());
			return v;
		};
		// The count of differing bytes says nothing by itself: the firmware's screen buffer lives
		// in the same RAM and changes from one snapshot to the next for no reason. So the addresses
		// are printed too - patch memory is 0x0000-0x1FFF (measured by factory_bank_probe), and a
		// reverb type edit must land exactly there.
		auto diff = [](const char *what, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
			size_t n = 0, inPatches = 0;
			std::string where;
			for (size_t i = 0; i < a.size(); ++i)
				if (a[i] != b[i]) {
					++n;
					if (i < 0x2000) ++inPatches;
					if (n <= 12) {
						char buf[32];
						std::snprintf(buf, sizeof buf, " %04zX:%02X>%02X", i, a[i], b[i]);
						where += buf;
					}
				}
			std::printf("  %-34s всего %3zu, в памяти патчей %3zu %s\n", what, n, inPatches,
			            where.c_str());
			return n;
		};
		auto watch = [&](double seconds) {
			std::string last;
			for (int t = 0; t < int(seconds / 0.15); ++t) {
				const std::string s = screen(proc);
				if (s != last) { std::printf("      %4.1f с \"%s\"\n", t * 0.15, s.c_str()); last = s; }
				render(proc, 0.15);
			}
		};

		std::printf("\n=== снять защиту внутренней памяти ===\n");
		press(proc, "System"); press(proc, "Group+"); press(proc, "Number-");
		std::printf("  %s\n", screen(proc).c_str());
		press(proc, "Exit", 3);

		std::printf("\n=== чистая карта, форматирование, запись ===\n");
		std::vector<uint8_t> blank(D110Core::kCardSize, 0xff);
		proc.getCore().setCardImage(blank.data());
		proc.getCore().setCardInserted(true);
		render(proc, 0.6);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch(2.5);
		press(proc, "Enter"); // agree to formatting
		watch(4.0);
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch(5.0);
		press(proc, "Exit", 3);
		const auto afterSave = snap();

		// The stimulus must change STORED memory, not temporary memory. An edit in Patch Edit will not
		// do: it lands in the working copy, and the RAM snapshot showed zero changes in patch memory.
		// A factory reset rebuilds timbre and rhythm memory from the ROM presets - that is definitely
		// stored memory, and it restarts the machine, which also checks whether the card survives a
		// restart.
		std::printf("\n=== заводской сброс: память заведомо другая ===\n");
		proc.getCore().factoryReset();
		render(proc, 3.0);
		while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
		render(proc, 9.0);
		press(proc, "Exit", 3);
		const auto afterEdit = snap();
		diff("после сброса против записанного:", afterSave, afterEdit);

		std::printf("\n=== чтение с карты ===\n");
		press(proc, "System"); press(proc, "Group+"); press(proc, "Number-");
		std::printf("  %s\n", screen(proc).c_str());
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Group+"); press(proc, "Enter"); press(proc, "Write");
		watch(5.0);
		press(proc, "Exit", 3);
		const auto afterLoad = snap();
		diff("после чтения против записанного:", afterSave, afterLoad);
		diff("после чтения против правки:", afterEdit, afterLoad);

		proc.setPoweredOn(false);
		return 0;
	}

	// Path to the card page. Passed on the command line, because it was found by the explore
	// mode, not derived from the menu layout.
	std::vector<std::string> path;
	for (int i = 2; i < argc; ++i) path.emplace_back(argv[i]);
	if (path.empty()) { std::printf("нужен путь кнопок, найденный режимом explore\n"); return 1; }

	std::printf("\n=== четыре карты, четыре ответа ===\n");
	std::vector<uint8_t> card(D110Core::kCardSize, 0xff);
	tryCard(proc, "пустое гнездо (0xFF)", card, false, path);

	card.assign(D110Core::kCardSize, 0x00);
	tryCard(proc, "карта из нулей", card, true, path);

	fillFormatted(card);
	tryCard(proc, "отформатированная", card, true, path);

	// Write protection is the engine ON THE CARD, not a byte in its memory: the firmware reads it
	// as bit 0 of the IC21 matrix status port. While this address was memory, formatting itself
	// protected the card it had just formatted.
	proc.getCore().setCardWriteProtect(true);
	tryCard(proc, "она же, защита записи", card, true, path);
	proc.getCore().setCardWriteProtect(false);

	// Persistence control: what the firmware wrote to the card must survive ejection and
	// return. Otherwise the "card" is a picture, not a medium.
	std::printf("\n=== контроль: содержимое переживает извлечение ===\n");
	fillFormatted(card);
	proc.getCore().setCardImage(card.data());
	proc.getCore().setCardInserted(true);
	render(proc, 0.6);
	std::vector<uint8_t> seen(D110Core::kCardSize, 0);
	proc.getCore().getCardImage(seen.data());
	std::printf("  вставлена: подпись \"%.12s\"\n", reinterpret_cast<char *>(seen.data()));
	proc.getCore().setCardInserted(false);
	render(proc, 0.6);
	proc.getCore().getCardImage(seen.data());
	std::printf("  извлечена: подпись \"%.12s\" (буфер плагина)\n",
	            reinterpret_cast<char *>(seen.data()));
	proc.getCore().setCardInserted(true);
	render(proc, 0.6);
	proc.getCore().getCardImage(seen.data());
	std::printf("  вставлена снова: подпись \"%.12s\"\n", reinterpret_cast<char *>(seen.data()));

	proc.setPoweredOn(false);
	return 0;
}
