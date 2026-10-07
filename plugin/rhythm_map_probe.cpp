// Where the two "Attempted to play unmapped key 25/27" at the start of the demo come from.
//
// The engine refuses to play a rhythm key whose Rhythm Setup holds timbre 127 (OFF)
// - Part.cpp, RhythmPart::noteOn. There are exactly two possible explanations, and they need different
// fixes, so they must be told apart, not picked between:
//
//   1. Race. The firmware loads the rhythm map at song start, while the mirror snapshots RAM once
//      per frame, and the first strikes get in before the region is sent.
//   2. Map. The firmware itself holds OFF for these keys and plays them anyway - then either
//      the region base or the interpretation of the field diverges.
//
// One measurement tells them apart: what lies in these entries in the FIRMWARE and in the ENGINE before the
// song starts and after, and at what moment relative to the first strikes the region is sent. The region
// send counter is sampled by the same loop as the sound, so the times are comparable.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

constexpr uint32_t packed(uint32_t a) {
	return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
}
constexpr uint32_t kRhythmSysex = 0x030110;
constexpr uint16_t kRhythmRam = 0x2090;
constexpr int kFirstRhythmKey = 24; // entry N describes key 24 + N

// Origin for all time marks in this tool: it is also the start of the note log, so
// region sends and rhythm strikes are measured on one ruler.
Clock::time_point g_zero;
bool g_watching = false;
struct Emit { double ms; uint64_t c0, c1, c2; };
std::vector<Emit> g_emits;
uint64_t g_prev0 = 0, g_prev1 = 0, g_prev2 = 0;

double elapsedMs() {
	return std::chrono::duration<double, std::milli>(Clock::now() - g_zero).count();
}

// The only way to advance time in this tool: it renders sound in blocks and on each
// block samples the Rhythm Setup send counters (regions 1..3 in kMirrorRegions).
//
// Polling runs during the key presses too. While it started only after them, the answer to the main
// question - which came first, the rhythm map or the first strikes - was unreachable: the counter was first
// read with an already accumulated value, and all it showed was "sometime up to now".
void pump(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		if (g_watching) {
			const uint64_t c0 = proc.getCore().regionEmitCount(1);
			const uint64_t c1 = proc.getCore().regionEmitCount(2);
			const uint64_t c2 = proc.getCore().regionEmitCount(3);
			if (c0 != g_prev0 || c1 != g_prev1 || c2 != g_prev2) {
				g_emits.push_back({elapsedMs(), c0, c1, c2});
				g_prev0 = c0; g_prev1 = c1; g_prev2 = c2;
			}
		}
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

// The button is held and released UNDER sound rendering, not under a sleep. In a host the plugin renders
// sound continuously, and the window between a parameter appearing and being applied is one block;
// a pause without rendering piles up both queues for the whole press and stretches that window to
// a second and a half - i.e. it measures the tool itself, not the machine.
void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	pump(proc, hold / 1000.0);
	for (int i : idx) proc.getCore().setButton(i, false);
	pump(proc, settle / 1000.0);
}

// Rhythm Setup entries for keys `from`..`to`, from firmware RAM and from the engine, side by side.
// Timbre field: 127 is OFF, anything else is the timbre number in the rhythm bank.
void dumpMap(D110AudioProcessor &proc, const std::vector<uint8_t> &ram, int from, int to,
             const char *when) {
	std::printf("\n  Rhythm Setup, %s\n", when);
	std::printf("   клавиша | прошивка: тембр ур. пан вых | движок: тембр ур. пан вых\n");
	for (int key = from; key <= to; ++key) {
		const int entry = key - kFirstRhythmKey;
		const uint8_t *fw = &ram[kRhythmRam + 4 * entry];
		uint8_t eng[4];
		std::memset(eng, 0xAA, sizeof eng);
		proc.engineReadMemory(packed(kRhythmSysex) + 4u * uint32_t(entry), 4, eng);
		const bool differ = std::memcmp(fw, eng, 4) != 0;
		std::printf("   %7d | %14s%3d %3d %3d %3d | %8s%3d %3d %3d %3d%s\n", key,
		            fw[0] == 127 ? "OFF " : "", fw[0], fw[1], fw[2], fw[3],
		            eng[0] == 127 ? "OFF " : "", eng[0], eng[1], eng[2], eng[3],
		            differ ? "   <-- РАСХОДЯТСЯ" : "");
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	pump(proc, 9.0); // render, do not sleep: otherwise the mirror backlog would be applied later
	std::printf("прошивка работает: %s   движок открыт: %s\n",
	            proc.getCore().isRunning() ? "да" : "НЕТ",
	            proc.engineIsOpen() ? "да" : "НЕТ");
	if (!proc.engineIsOpen()) return 1;

	// Keys 24..31 - where the skipped 25 and 27 sit.
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
	dumpMap(proc, ram, 24, 31, "ДО запуска песни");

	proc.getCore().resetTallies();
	proc.getCore().startNoteLog();
	g_zero = Clock::now();
	g_watching = true;
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);
	pump(proc, 12.0);

	proc.getCore().getRam(ram.data());
	dumpMap(proc, ram, 24, 31, "ПОСЛЕ 12 секунд песни");

	std::printf("\n  Отправки Rhythm Setup, мс от startNoteLog (нажатия входят в отсчёт):\n");
	if (g_emits.empty()) std::printf("   ни одной - карта ритма не менялась\n");
	for (const auto &e : g_emits)
		std::printf("   %8.1f мс   куски: %llu / %llu / %llu\n", e.ms,
		            (unsigned long long)e.c0, (unsigned long long)e.c1, (unsigned long long)e.c2);

	std::printf("\n  Первые удары ритм-партии (партия 8 = ритм), мс от startNoteLog:\n");
	int shown = 0;
	const auto log = proc.getCore().takeNoteLog();
	for (const auto &e : log) {
		if (!e.on || e.part != 8) continue;
		std::printf("   %8.1f мс   клавиша %3d  velocity %3d\n", e.ms, e.note, e.velocity);
		if (++shown >= 12) break;
	}
	std::printf("   журнал: %zu событий, потеряно %llu; кольцо зеркала потеряло %llu\n",
	            log.size(), (unsigned long long)proc.getCore().noteLogDropped_(),
	            (unsigned long long)proc.getCore().sysexDropped());

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
