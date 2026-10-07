// Verifies the ported six individual outputs not by compilation but by sound routing:
// enables all 6 extra buses, sets part 1's Output Assign to different values (MIX, then
// INDIVIDUAL 1, then INDIVIDUAL 3) and measures the energy in EACH of the eight channels
// (MIX L/R + 6 mono) - the part must sound in EXACTLY one place, not smear across or be
// silent everywhere.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, juce::AudioBuffer<float> &buffer, double seconds,
            juce::MidiBuffer *first = nullptr) {
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	bool firstBlock = true;
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (firstBlock && first != nullptr) ? *first : none);
		firstBlock = false;
	}
}

void setSystemByte(D110AudioProcessor &proc, int offset, juce::uint8 value) {
	proc.sendAreaData(D110Core::kSysexSystem, offset, &value, 1);
}

double energyOf(const juce::AudioBuffer<float> &buffer, int channel) {
	if (channel >= buffer.getNumChannels()) return -1.0; // the bus is absent from the buffer entirely
	double e = 0.0;
	const float *d = buffer.getReadPointer(channel);
	for (int i = 0; i < buffer.getNumSamples(); ++i) e += double(d[i]) * d[i];
	return e;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;

			// Enable all six extra buses - they are off by default (createBuses()), and without
			// this the plugin takes the ordinary, few-output path, which there is nothing to check.
	for (int i = 1; i <= 6; ++i) {
		auto *bus = proc.getBus(false, i);
		if (bus != nullptr) bus->setCurrentLayout(juce::AudioChannelSet::mono());
	}

	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	{
		juce::AudioBuffer<float> warm(8, kBlock);
		render(proc, warm, 9.0);
		if (!proc.getCore().isRunning()) {
			std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
			return 1;
		}
	}
	proc.setForwardNotesToFirmware(true);

	std::printf("шин на выходе (ожидается 8 - MIX L/R + 6 моно): %d\n",
	            proc.getTotalNumOutputChannels());

	static const struct { int assign; const char *name; } kCases[] = {
		{ 1, "MIX (заводское значение)" },
		{ 2, "INDIVIDUAL 1" },
		{ 4, "INDIVIDUAL 3" },
	};

	for (const auto &c : kCases) {
		std::printf("\n=== Output Assign = %d (%s) ===\n", c.assign, c.name);
		setSystemByte(proc, 1, 0); // reverb off - so INDIVIDUAL 5/6 are not silenced by the rule
		juce::AudioBuffer<float> settle(8, kBlock);
		render(proc, settle, 0.3);

		proc.sendTimbreTempParam(0, 6, juce::uint8(c.assign)); // part 1, byte 6 = Output Assign
		render(proc, settle, 0.2);

		juce::AudioBuffer<float> buffer(8, kBlock);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(2, 60, 1.0f), 0);
		render(proc, buffer, 0.6, &on);

		static const char *kChannelNames[8] = {
			"MIX L", "MIX R", "IND 1", "IND 2", "IND 3", "IND 4", "IND 5", "IND 6"
		};
		for (int ch = 0; ch < 8; ++ch)
			std::printf("  %-6s: %.6f\n", kChannelNames[ch], energyOf(buffer, ch));

		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
		render(proc, buffer, 0.3, &off);
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
