// Byte-by-byte comparison of what the FIRMWARE holds for each part and what the ENGINE
// holds, right during the demo song. Whatever the bridge failed to carry over shows up
// here as a specific diverging byte and is visible from neither side alone.
//
// Taken DURING the demo, because the demo loads its own patch: how the parts look at
// load time says nothing about how they look during play.
//
// The normal result on "Macho Memory" - exactly byte 0 diverges for parts 6 and 7: the
// firmware holds timbre group 5, the engine 3. This is not a loss in the bridge but a
// limit declared by the D-110 itself: the maximums table in the control ROM gives the
// timbre group a maximum of 3, and the write is clamped to it. What this means for the
// sound and why the right timbre still arrives - see docs/timbre_group_5.md.
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

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);

	for (int round = 0; round < 4; ++round) {
		render(proc, 8.0);

		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());

		// Byte for byte, firmware against engine. Roland's PatchTemp is
		// group, timbre, keyShift, fineTune, benderRange, assignMode, reverbSwitch, dummy,
		// then outputLevel and panpot - so a level or a timbre that failed to cross shows up
		// as a specific differing byte rather than as a vague discrepancy.
		std::printf("\n--- after %d s of the demo ---\n", (round + 1) * 8);
		std::printf("  part | grp tmbr kSh fine bnd asg rev  -  LVL PAN | engine same?\n");
		for (int p = 0; p < 8; ++p) {
			const uint8_t *fw = &ram[0x2000 + 16 * p];
									// The engine addresses its memory in PACKED form, not in Roland's "three
									// seven-bit bytes" form: MT32EMU_MEMADDR from Structures.h folds 0x030000 into
									// 0x00C000. At the Roland address the region is not found at all, readMemory
									// returns without touching the buffer, and everything reads as zeros. So the
									// buffer is first filled with a marker: a silent no-op call must not look like
									// data.
			auto packed = [](uint32_t a) {
				return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
			};
			uint8_t eng[16];
			std::memset(eng, 0xAA, sizeof eng);
			proc.engineReadMemory(packed(0x030000u) + 16u * uint32_t(p), 10, eng);

			std::printf("  %4d |", p + 1);
			for (int i = 0; i < 10; ++i) std::printf(" %3d", fw[i]);
			bool same = true;
			for (int i = 0; i < 10; ++i)
				if (eng[i] != fw[i]) { same = false; break; }
			if (same) {
				std::printf(" | match\n");
			} else {
				std::printf(" | DIFFERS, engine:");
				for (int i = 0; i < 10; ++i)
					std::printf(" %s%d%s", eng[i] != fw[i] ? "[" : "", eng[i],
					            eng[i] != fw[i] ? "]" : "");
				std::printf("\n");
			}
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
