// "Ableton sync drifts, plays off the grid, even though the demo song keeps the rhythm
// tightly" - and that is exactly one architectural step of difference: the demo song is
// played by the firmware ITSELF, without a single byte of MIDI, while a host note goes
// processBlock -> MIDI queue -> emulated serial cable (31250 baud, real tempo) -> the
// firmware recognises it in its tables. That is a ROUND TRIP with real latency, not the
// same thing as the demo.
// setLatencySamples() is never called in the plugin (checked with grep) - so the host is
// sure the latency is zero and compensates nothing. If the round trip gives a stable, not
// jittery, delay - it is not jitter, it is uncompensated latency, and it is fixed by
// exactly one line in prepareToPlay.
//
// Measured with the tool the core already has: startNoteLog()/takeNoteLog() stamps REAL
// time on every note the firmware itself registered - which is the moment it became
// audible. Compared against the moment WE fed the note to processBlock, with the same
// steady_clock.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

// A flat 11ms sleep per block, used by earlier probes for ordinary boot waits, is 0.6ms
// short of the true block period (512/44100 = 11.61ms) - close enough to not matter when
// waiting seconds for firmware boot, but this probe is measuring single-digit milliseconds,
// and that shortfall accumulates into exactly the kind of sawtooth drift a real, honest
// latency measurement must not carry. Paced against an absolute deadline instead, the same
// way la32_ctx_probe.cpp and bridge_probe.cpp already do it for real-time-accurate MIDI.
void renderBlocks(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	auto next = Clock::now();
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
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
	render(proc, 1.0); // let the panel settle after boot, do not confuse it with notes

			// Isolated single notes, ONE part, with slack between them - no crowding in the
			// queue, so that we measure the round-trip latency of the path itself, not a jam.
	constexpr int kRounds = 15;
	std::vector<double> offsets;
	proc.getCore().startNoteLog();
	const auto t0 = Clock::now();

	for (int i = 0; i < kRounds; ++i) {
		const double sendMs =
			std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(2, 60 + (i % 12), 0.9f), 0);
		renderBlocks(proc, 1, &on);
		render(proc, 0.4);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60 + (i % 12)), 0);
		renderBlocks(proc, 1, &off);
		render(proc, 0.3);

		const auto events = proc.getCore().takeNoteLog();
		for (const auto &e : events)
			if (e.on && e.ms >= sendMs) { offsets.push_back(e.ms - sendMs); break; }
	}

	std::printf("круг | подано, мс | задержка до регистрации прошивкой, мс\n");
	double sum = 0, minV = 1e9, maxV = -1e9;
	for (size_t i = 0; i < offsets.size(); ++i) {
		std::printf("  %2d | %10.1f | %8.1f\n", int(i) + 1, 0.0, offsets[i]);
		sum += offsets[i];
		minV = std::min(minV, offsets[i]);
		maxV = std::max(maxV, offsets[i]);
	}
	const double mean = offsets.empty() ? 0.0 : sum / double(offsets.size());
	double variance = 0;
	for (double v : offsets) variance += (v - mean) * (v - mean);
	const double stddev = offsets.empty() ? 0.0 : std::sqrt(variance / double(offsets.size()));

	std::printf("\nизмерений: %d, среднее %.1f мс, разброс [%.1f, %.1f], стд.откл. %.1f мс\n",
	            int(offsets.size()), mean, minV, maxV, stddev);
	std::printf("%s\n",
	            stddev < 3.0
	                ? "СТАБИЛЬНО - это не джиттер, а некомпенсированная задержка (setLatencySamples не вызывается)."
	                : "НЕСТАБИЛЬНО - разброс велик, дело не только в фиксированной задержке.");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
