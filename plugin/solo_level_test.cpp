// Reported by ear: on the demo song channels 6 and 7 - which are parts 5 and 6 - sound
// dead, while their indicators light. Note counts cannot settle that: a part can receive
// every note it should and still be inaudible if it is quiet, hard-panned or buried. So
// render the SAME piece once per part with only that part delivered to the engine, and
// measure what comes out.
//
// Also records which song is playing, because the demo advances from one to the next and
// different songs use different parts - comparing a measurement of one against a
// measurement of another is how this investigation went wrong before.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

// The song title is only ever available as dots on the LCD, so reading it back means owning
// the character generator. It is found by content rather than by filename: the ROM set names
// vary between dumps, and a wrong file here would silently turn every title into '?'.
std::vector<uint8_t> g_cgrom;

// Checked against the glyph for 'A' specifically because it is the one character whose shape
// is unmistakable at 5x7 and identical across every revision of this part - a crossbar and
// two uprights. Only the low five bits count; the top three are not part of the cell.
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
		const auto f = entry.getFile();
		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && isCgrom(data)) {
			g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
			               static_cast<const uint8_t *>(data.getData()) + 4096);
			return;
		}
	}
}

// Dots back to a character, by searching the generator for a cell that matches exactly.
// Backwards, but there is no other direction available: what the firmware handed the display
// was a code, and what can be read out of the display afterwards is only the shape it drew.
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

// The SECOND line: the top one carries the running status, which changes constantly, while
// the song's name sits underneath it and holds still for as long as the song plays.
std::string lcdLine2(D110AudioProcessor &proc) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int col = 0; col < D110Core::kCols; ++col)
		s.push_back(decodeCell(rows + ((size_t)D110Core::kCols + col) * D110Core::kRowsPerChar));
	return s;
}

// Takes a LIST of buttons because the demo is started by a combination, and a combination is
// not the same event as its buttons pressed one after another - the firmware reads the panel
// as a whole and only sees a combination if the keys overlap in one of its sweeps.
void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(hold));
	for (int i : idx) proc.getCore().setButton(i, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settle));
}

struct Result {
	float peak = 0;
	double rms = 0;
	int notes = 0;
	std::string song;
	// The completeness of the measurement is printed along with it: the note log can fill up, and the mirror
	// ring can overflow, and then the figures on the left are lower bounds, not a result.
	uint64_t noteDrops = 0, sysexDrops = 0;
};

// Boots a fresh machine, starts the demo from a known state, renders `seconds` with only
// `solo` delivered, and measures. A fresh machine each time is what makes the runs
// comparable - the song must start from the same place, not wherever the last run left it.
Result runSolo(int solo, double seconds) {
	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	// Loading is COUNTED, not slept. The bridge accumulates one DT1 message for each mirrored
	// region that changes at firmware start, and only processBlock drains the ring:
	// sleeping through the load means starting the measurement with an unapplied parameter queue,
	// i.e. measuring not the state the panel shows.
	{
		juce::AudioBuffer<float> warm(2, kBlock);
		const auto begin = Clock::now();
		auto next = begin;
		while (std::chrono::duration<double>(Clock::now() - begin).count() < 9.0) {
			juce::MidiBuffer none;
			warm.clear();
			proc.processBlock(warm, none);
			next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
			std::this_thread::sleep_until(next);
		}
	}

	// Solo is set BEFORE the demo starts, and the counters are zeroed BEFORE the first note: otherwise the figures
	// would include what the firmware was busy with at boot, and the difference between parts would turn out to be
	// the difference between their start-up chores.
	proc.getCore().setSoloPart(solo);
	proc.getCore().resetTallies();
	// Starting the demo from the panel, as on the unit: Edit+Enter together, then Enter separately -
	// confirmation. Both pauses of 200/500 ms are not a safety margin for its own sake: the firmware polls the
	// panel on its own schedule, and a press shorter than a scan pass it simply will not notice.
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);
	proc.getCore().startNoteLog();

	Result r;
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	double sumSq = 0;
	int64_t n = 0;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		for (int ch = 0; ch < 2; ++ch)
			for (int i = 0; i < kBlock; ++i) {
				const float s = block.getSample(ch, i);
				r.peak = juce::jmax(r.peak, std::abs(s));
				sumSq += double(s) * s;
				++n;
			}
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
	r.rms = n ? std::sqrt(sumSq / double(n)) : 0.0;
	r.song = lcdLine2(proc);
	r.noteDrops = proc.getCore().noteLogDropped_();
	r.sysexDrops = proc.getCore().sysexDropped();
	// Counted by the lossless counter, not the log: the log can be truncated, the counter cannot.
	if (solo < 0) {
		for (int p = 0; p < 9; ++p) r.notes += int(proc.getCore().noteOnsForPart(p));
	} else {
		r.notes = int(proc.getCore().noteOnsForPart(solo));
	}
	proc.getCore().takeNoteLog();

	proc.setPoweredOn(false);
	proc.releaseResources();
	return r;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	constexpr double kSeconds = 30.0;
	std::printf("Each row is one fresh boot, the same song from the start, only that part\n"
	            "delivered to the engine. %.0f seconds each.\n\n", kSeconds);
	std::printf("  part | notes | peak      | rms       | dB vs part 1 | drops | song\n");

	double refRms = 0;
	for (int part : {0, 4, 5, 6, 8}) { // part 1 as reference, then 5, 6, 7, rhythm
		const Result r = runSolo(part, kSeconds);
		if (part == 0) refRms = r.rms;
		const double dB = (refRms > 0 && r.rms > 0) ? 20.0 * std::log10(r.rms / refRms) : -999.0;
		std::printf("  %4d | %5d | %.6f | %.6f | %11.1f | %llu/%llu | %s%s\n", part + 1, r.notes,
		            r.peak, r.rms, dB, (unsigned long long)r.noteDrops,
		            (unsigned long long)r.sysexDrops, r.song.c_str(),
		            (r.noteDrops || r.sysexDrops) ? "  <-- LOWER BOUND" : "");
	}

	std::printf("\ndone\n");
	return 0;
}
