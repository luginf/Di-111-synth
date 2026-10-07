// Why in the demo song parts 6 and 7 are quieter by 21-24 dB and why part 5 gets no notes.
//
// A lead from the previous session: the firmware holds TIMBRE GROUP 5 for exactly these two parts,
// while all the others hold 0 or 1, and the engine, descended from the MT-32, knows only
// groups 0-3. Here the question is asked in a form a level measurement cannot answer,
// and the answer comes from ONE run, so that nothing is compared between runs:
//
//   Does the engine hold for each part the same timbre that the FIRMWARE holds?
//
// The comparison is by NAME - the first ten bytes of a timbre are its ASCII name - because
// a name is unambiguous and a level is not. Matching parts prove that the method works on
// this very run; a mismatching part directly names the sound that is wrongly playing.
//
// The three earlier tools in this investigation gave confident wrong answers, so:
//   * every record that can overflow prints a loss counter, and counts
//     that must not be truncated are kept in fixed-size counters
//     (D110Core::noteOnsForPart and its neighbours);
//   * the read path from the engine is proven by a CONTROL - write a known value and
//     read it back - before believing a single one of its readings.
//     plugin/part_state_compare.cpp read zeros from the engine, and nobody could say
//     whether the engine or the reader was to blame;
//   * the playing song is printed in every block, because the demo moves from song to
//     song, and different songs use different parts.
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

// Roland writes an address as three separate seven-bit bytes, while the engine addresses the same
// memory with one packed 21-bit number. MT32EMU_MEMADDR is that conversion,
// repeated here so the tool does not have to pull in the engine's internal headers.
constexpr uint32_t packed(uint32_t a) {
	return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
}
constexpr uint32_t kPatchTempSysex = 0x030000; // "Timbre Temporary" in Roland D-110 terms
constexpr uint32_t kToneTempSysex = 0x040000;  // "Tone Temporary" - the timbre proper
constexpr uint16_t kPatchTempRam = 0x2000;
constexpr uint16_t kToneTempRam = 0x21E4;
constexpr int kToneStride = 246;

// ---- LCD readout, so the song and part indicators land in the log together with the numbers ----

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
	const auto dir = D110AudioProcessor::getAutoRomFolder();
	for (const auto &entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles)) {
		juce::MemoryBlock data;
		if (entry.getFile().loadFileAsData(data) && isCgrom(data)) {
			g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
			               static_cast<const uint8_t *>(data.getData()) + 4096);
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

std::string lcdRow(D110AudioProcessor &proc, int row) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int col = 0; col < D110Core::kCols; ++col)
		s.push_back(decodeCell(rows + ((size_t)row * D110Core::kCols + col) * D110Core::kRowsPerChar));
	return s;
}

// Prints the top line of the panel as dots, one per character cell. The part indicators are
// the first nine cells, and whether a cell means "part is playing" or "part is silent"
// cannot be guessed: the whole complaint "part 5 gets no notes although its
// indicator is lit" rests on it. A cell the character generator cannot name is a
// USER-defined character, and Roland's activity block is exactly that, so the picture
// has to be looked at, not decoded.
void dumpIndicatorGlyphs(D110AudioProcessor &proc) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) { std::printf("  (no LCD)\n"); return; }
	for (int r = 0; r < D110Core::kRowsPerChar && r < 7; ++r) {
		std::printf("   ");
		for (int col = 0; col < 10; ++col) {
			const uint8_t bits = rows[(size_t)col * D110Core::kRowsPerChar + r];
			for (int b = 4; b >= 0; --b) std::printf("%c", (bits >> b) & 1 ? '#' : '.');
			std::printf(" ");
		}
		std::printf("\n");
	}
	std::printf("   ");
	for (int col = 0; col < 10; ++col)
		std::printf("  %c   ", decodeCell(rows + (size_t)col * D110Core::kRowsPerChar));
	std::printf("   <- что знакогенератор делает из каждого знакоместа\n");
}

void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(hold));
	for (int i : idx) proc.getCore().setButton(i, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settle));
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

// The first ten bytes of a timbre are its name. Non-printable bytes are shown as '.', so that
// a block of zeros or garbage looks like an obvious non-name and not like an empty string.
std::string toneName(const uint8_t *p) {
	std::string s;
	for (int i = 0; i < 10; ++i) s.push_back((p[i] >= 0x20 && p[i] < 0x7f) ? char(p[i]) : '.');
	return s;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	// We COUNT sound, not sleep. The bridge queues a DT1 for every mirrored region
	// that changes while the firmware loads, and this ring is drained only by
	// processBlock. Sleeping through the load means leaving the whole jam in the queue, and the very first
	// subsequent sound render plays it back: that is how this tool's own control write was silently
	// overwritten, and a healthy read path looked broken.
	render(proc, 9.0);
	std::printf("firmware running: %s   sound engine open: %s   LCD font: %s\n",
	            proc.getCore().isRunning() ? "yes" : "NO",
	            proc.engineIsOpen() ? "yes" : "NO",
	            g_cgrom.empty() ? "NOT FOUND (screens will read as ?)" : "loaded");
	if (!proc.engineIsOpen()) {
		std::printf("\nThe engine never opened, so every reading below would be the buffer\n"
		            "this tool filled in itself. Stopping instead of printing them.\n");
		return 1;
	}

	// ---- CONTROL: does the read path from the engine work at all? ---------------------
	// Write, then read back. Until this passes, nothing read from the engine
	// means anything - exactly the state part_state_compare.cpp was left in.
	//
	// This control caught TWO things on the very first attempt, and both would have been read as findings
	// about the D-110 itself:
	//  1. Synth::playSysex puts the message IN A QUEUE; it is applied during sound
	//     rendering. A write and read-back with no rendering between them read the value from BEFORE the write.
	//  2. The bridge resends the whole Timbre Temporary region every time the firmware's RAM
	//     changes, and that overwrites everything written here. So the counter of
	//     sends of this region is printed alongside: a failed read with a non-zero counter means the firmware
	//     is taking its memory back, not that the reader is broken.
	{
		proc.getCore().resetTallies();
		// A whole 16-byte Timbre Temporary record for the rhythm part (index 8), which the demo
		// does not edit along this path. The values lie inside the D-110's own table of maxima,
		// so nothing is clamped and the read can be compared exactly.
		const uint8_t want[16] = {2, 17, 24, 50, 12, 1, 0, 0, 77, 9, 0, 0, 0, 0, 0, 0};
		uint8_t msg[32];
		int n = 0;
		msg[n++] = 0xF0; msg[n++] = 0x41; msg[n++] = 0x10; msg[n++] = 0x16; msg[n++] = 0x12;
		const uint32_t addr = kPatchTempSysex + 0x100; // rhythm part, per Roland's own map
		const uint8_t a1 = uint8_t((addr >> 16) & 0x7f), a2 = uint8_t((addr >> 8) & 0x7f),
		              a3 = uint8_t(addr & 0x7f);
		msg[n++] = a1; msg[n++] = a2; msg[n++] = a3;
		uint32_t sum = a1 + a2 + a3;
		for (int i = 0; i < 16; ++i) { msg[n++] = want[i]; sum += want[i]; }
		msg[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
		msg[n++] = 0xF7;
		proc.engineWriteSysexForTest(msg, n);
		render(proc, 0.3); // the engine applies the sysex queue while it renders sound

		uint8_t got[16];
		std::memset(got, 0xAA, sizeof got);
		proc.engineReadMemory(packed(kPatchTempSysex) + 16 * 8, 16, got);
		const bool ok = std::memcmp(want, got, 10) == 0;
		std::printf("\nCONTROL - write a known Timbre Temporary entry, read it back:\n  wrote:");
		for (int i = 0; i < 10; ++i) std::printf(" %3d", want[i]);
		std::printf("\n  read :");
		for (int i = 0; i < 10; ++i) std::printf(" %3d", got[i]);
		std::printf("\n  (the bridge re-sent this region %llu times meanwhile)\n",
		            (unsigned long long)proc.getCore().regionEmitCount(0));
		std::printf("  => engine read path %s\n", ok ? "WORKS" : "IS BROKEN - stop here");
		if (!ok) return 1;

		// Put the firmware's own state back in place before any measurements.
		proc.getCore().resyncMirror();
		render(proc, 0.5);
	}

	// ---- start the demo -----------------------------------------------------------------
	proc.getCore().resetTallies();
	proc.getCore().startNoteLog();
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);

	for (int round = 0; round < 4; ++round) {
		render(proc, 7.5);

		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());

		std::printf("\n=== %2d s into the demo | screen: \"%s\" / \"%s\" ===\n",
		            (round + 1) * 8, lcdRow(proc, 0).c_str(), lcdRow(proc, 1).c_str());
		std::printf(" part | firmware grp/tone lvl pan | tone the FIRMWARE holds"
		            " | tone the ENGINE holds\n");
		for (int p = 0; p < 8; ++p) {
			const uint8_t *fwPatch = &ram[kPatchTempRam + 16 * p];
			const uint8_t *fwTone = &ram[kToneTempRam + kToneStride * p];

			uint8_t engTone[16];
			std::memset(engTone, 0xAA, sizeof engTone);
			proc.engineReadMemory(packed(kToneTempSysex) + uint32_t(kToneStride) * uint32_t(p),
			                      10, engTone);

			const std::string fwName = toneName(fwTone), engName = toneName(engTone);
			std::printf("  %3d | grp %d  tone %2d  %3d %2d | \"%s\" | \"%s\" %s\n",
			            p + 1, fwPatch[0], fwPatch[1], fwPatch[8], fwPatch[9],
			            fwName.c_str(), engName.c_str(),
			            fwName == engName ? "" : "  <-- MISMATCH");
		}
		std::printf("  the nine part indicators, as dots (parts 1-8 then rhythm):\n");
		dumpIndicatorGlyphs(proc);
	}

	// ---- counts that could not have lost anything -------------------------------
	const auto log = proc.getCore().takeNoteLog();
	std::printf("\nNotes the firmware started, per part (fixed counters - nothing can be lost):\n");
	std::printf("  part | note-ons | writes to the part byte naming this part\n");
	for (int p = 0; p < 9; ++p)
		std::printf("  %4s | %8llu | %llu\n", p == 8 ? "rhy" : std::to_string(p + 1).c_str(),
		            (unsigned long long)proc.getCore().noteOnsForPart(p),
		            (unsigned long long)proc.getCore().partByteWrites(p));
	std::printf("  part-byte writes naming something that is not a part (9-15):");
	for (int v = 9; v < 16; ++v)
		std::printf(" %llu", (unsigned long long)proc.getCore().partByteWrites(v));
	std::printf("\n  note log held %zu events, dropped %llu\n", log.size(),
	            (unsigned long long)proc.getCore().noteLogDropped_());

	std::printf("\nDT1 messages sent per mirrored region - a region that fires alone can\n"
	            "undo what another region set up:\n");
	for (int i = 0; i < D110Core::kNumMirrorRegions; ++i)
		std::printf("  %-28s %llu\n", D110Core::kMirrorRegions[i].name,
		            (unsigned long long)proc.getCore().regionEmitCount(i));
	std::printf("  mirror messages the ring had no room for: %llu%s\n",
	            (unsigned long long)proc.getCore().sysexDropped(),
	            proc.getCore().sysexDropped() ? "   <-- READINGS ABOVE ARE LOWER BOUNDS" : "");

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
