// Checks the ported BOSS reverb (BossEmu, munt/mt32emu/src/BossEmu.cpp) not at compile
// time but on actual sound: (1) whether ic6.bin is really found and loaded into the synth, (2)
// whether live audio reacts to a reverb type change through the same panel/memory the owner
// uses, and (3) whether DIFFERENT types (Small Room and Delay 3 - as dissimilar by design as
// possible) give a clearly different decay tail. If BossEmu is wired in but the type never
// reaches it (for example the mirror does not send the byte yet), all eight will sound the
// same - exactly what a successful compile alone cannot reveal.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
	}
}

// Sets the reverb type directly in the System Area (RAM 0x2D95, offset 1) through the same
// path the panel itself uses to send an edit to the firmware - the very one that was extended
// with the mirror today.
void setSystemByte(D110AudioProcessor &proc, int offset, juce::uint8 value) {
	proc.sendAreaData(D110Core::kSysexSystem, offset, &value, 1);
}

// Full volume and a noticeable reverb: note at maximum velocity, reverb level 7 and time
// 7 (the maximum of both). The note is held 0.5s, then released - what matters is the tail
// in silence, where the reverb is audible, not the dry signal.
double measureTail(D110AudioProcessor &proc, int reverbType) {
	setSystemByte(proc, 1, juce::uint8(reverbType)); // type
	setSystemByte(proc, 2, 7);                       // time
	setSystemByte(proc, 3, 7);                       // level
	render(proc, 0.3);

	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(2, 60, 1.0f), 0);
	juce::AudioBuffer<float> buffer(2, kBlock);
	{
		const int blocks = int(0.5 * kSampleRate / kBlock);
		for (int b = 0; b < blocks; ++b) {
			buffer.clear();
			juce::MidiBuffer midi;
			if (b == 0) midi = on;
			proc.processBlock(buffer, midi);
		}
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
	{
		buffer.clear();
		proc.processBlock(buffer, off);
	}

	// Tail: one second of silence RIGHT after the note release, total energy. Different reverb
	// types decay differently - the "Room" comb is shorter, "Delay" stretches differently.
	double energy = 0.0;
	const int tailBlocks = int(1.0 * kSampleRate / kBlock);
	for (int b = 0; b < tailBlocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
		for (int ch = 0; ch < 2; ++ch)
			for (int i = 0; i < buffer.getNumSamples(); ++i) {
				const float s = buffer.getSample(ch, i);
				energy += double(s) * s;
			}
	}
	return energy;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	std::printf("Control ROM: %s\n", proc.getControlRomDescription().toRawUTF8());
	std::printf("PCM ROM    : %s\n", proc.getPcmRomDescription().toRawUTF8());

	static const char *kNames[8] = {
		"Small Room", "Medium Room", "Medium Hall", "Large Hall",
		"Plate", "Delay 1", "Delay 2", "Delay 3"
	};
	std::printf("\nтип | название     | энергия хвоста (1с тишины после снятия ноты)\n");
	double values[8];
	for (int t = 0; t < 8; ++t) {
		values[t] = measureTail(proc, t);
		std::printf("  %d | %-12s | %.6f\n", t, kNames[t], values[t]);
	}

	double minV = values[0], maxV = values[0];
	for (double v : values) { minV = std::min(minV, v); maxV = std::max(maxV, v); }
	std::printf("\nразброс между типами: min=%.6f max=%.6f отношение=%.2fx\n", minV, maxV,
	            minV > 0 ? maxV / minV : 0.0);
	std::printf("%s\n", (maxV / std::max(minV, 1e-12) > 1.3)
	                        ? "*** типы ЗАМЕТНО различаются - тип доходит до чипа ***"
	                        : "ПОДОЗРИТЕЛЬНО ОДИНАКОВО - возможно, тип не доходит до BossEmu");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
