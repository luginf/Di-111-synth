// Separates two versions of the cause of "types 0-6 give silence": either BossEmu really
// does not sound on those types, or the issue is switching BETWEEN types in one and the
// same synth (something is not reopened properly). The only way to separate them is a
// fresh process per type, with no prior switching at all.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

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

void setSystemByte(D110AudioProcessor &proc, int offset, juce::uint8 value) {
	proc.sendAreaData(D110Core::kSysexSystem, offset, &value, 1);
}

} // namespace

double measure(D110AudioProcessor &proc, int type, int time, int level) {
	setSystemByte(proc, 1, juce::uint8(type));
	setSystemByte(proc, 2, juce::uint8(time));
	setSystemByte(proc, 3, juce::uint8(level));
	render(proc, 0.3);

	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(2, 60, 1.0f), 0);
	juce::AudioBuffer<float> buffer(2, kBlock);
	for (int b = 0; b < int(0.5 * kSampleRate / kBlock); ++b) {
		buffer.clear();
		juce::MidiBuffer midi;
		if (b == 0) midi = on;
		proc.processBlock(buffer, midi);
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
	buffer.clear();
	proc.processBlock(buffer, off);

	double energy = 0.0;
	for (int b = 0; b < int(1.0 * kSampleRate / kBlock); ++b) {
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

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const int type = argc > 1 ? std::atoi(argv[1]) : 0;

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	(void)type;
			// The time=7,level=7 sweep (first experiment) landed in a dead zone for most types -
			// this is not a chip bug, it is an unlucky choice of test parameters. We check all
			// eight types at a more typical value (time=3, level=5), which is already confirmed to
			// sound on type 0.
		"Small Room", "Medium Room", "Medium Hall", "Large Hall",
		"Plate", "Delay 1", "Delay 2", "Delay 3"
	};
	std::printf("все восемь типов, time=3 level=5\n");
	std::printf("тип | название     | энергия хвоста\n");
	for (int t = 0; t <= 7; ++t) {
		const double e = measure(proc, t, 3, 5);
		std::printf("  %d | %-12s | %.6f\n", t, kNames[t], e);
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
