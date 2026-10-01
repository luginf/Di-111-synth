// Repro for "channel 10 / R makes no sound": plays single rhythm keys on the rhythm channel
// on a freshly booted native core and reports the audio peak and the engine's part-state bits.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

float render(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
	}
	return peak;
}
} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, std::getenv("BOOT") ? std::atof(std::getenv("BOOT")) : 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) { std::printf("did not start\n"); return 1; }
	if (std::getenv("CYCLE")) {
		proc.setPoweredOn(false); render(proc, 0.5); proc.setPoweredOn(true); render(proc, 9.0);
		std::printf("power cycled, running=%d\n", (int)proc.getCore().isRunning());
	}
	proc.setForwardNotesToFirmware(true);
	std::printf("sysexEmitted after boot: %llu\n", (unsigned long long)proc.getCore().sysexEmitted());
	if (std::getenv("RESYNC")) { proc.getCore().resyncMirror(); render(proc, 1.0); std::printf("resynced, emitted now %llu\n", (unsigned long long)proc.getCore().sysexEmitted()); }
	{
		const auto lcd = proc.getLcdSnapshot();
		for (int l = 0; l < 2; ++l) { char b[17] = {}; for (int c = 0; c < 16; ++c) b[c] = char(lcd.text[l][c]); std::printf("LCD[%d] |%s|\n", l, b); }
	}

	{
		std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		for (int n : { 36, 38, 42 }) {
			const int at = D110CoreType::kRamRhythmTemp + (n - D110CoreType::kRhythmFirstKey) * D110CoreType::kRhythmRecord;
			std::printf("key %d rec:", n);
			for (int i = 0; i < D110CoreType::kRhythmRecord; ++i) std::printf(" %d", ram[(size_t)at + i]);
			std::printf("\n");
		}
	}
	{
		std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		std::printf("SYSTEM:");
		for (int i = 0; i < 23; ++i) std::printf(" %d", ram[size_t(D110CoreType::kRamSystem + i)]);
		std::printf("\nunit %d %d, 36E6=%02x\n", ram[0x2DB6], ram[0x2DB7], ram[0x36E6]);
	}
	auto rhythmNonZero = [&]() {
		std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		int nz = 0;
		for (int i = 0; i < 340; ++i) nz += ram[size_t(D110CoreType::kRamRhythmTemp + i)] != 0;
		std::printf("R record:");
		for (int i = 0; i < 16; ++i) std::printf(" %d", ram[size_t(0x2080 + i)]);
		std::printf("\n");
		std::printf("rhythm RAM nonzero bytes: %d, key36 rec %d %d %d %d\n", nz,
		            ram[size_t(D110CoreType::kRamRhythmTemp + 12 * 4)], ram[size_t(D110CoreType::kRamRhythmTemp + 12 * 4 + 1)],
		            ram[size_t(D110CoreType::kRamRhythmTemp + 12 * 4 + 2)], ram[size_t(D110CoreType::kRamRhythmTemp + 12 * 4 + 3)]);
	};
	if (const char *f = std::getenv("MIDI_FILE")) {
		rhythmNonZero();
		std::printf("load: %d\n", (int)proc.getSequencer().loadMidiFile(juce::File(f)));
		render(proc, 3.0);
		rhythmNonZero();
		if (std::getenv("PLAY")) {
			proc.getSequencer().play();
			float pk = 0;
			for (int sec = 0; sec < 12; ++sec) { pk = juce::jmax(pk, render(proc, 1.0)); }
			std::printf("played 12 s, peak %.4f partStates 0x%x\n", pk, proc.enginePartStates());
			rhythmNonZero();
			proc.getSequencer().stop();
			render(proc, 1.0);
		}
	}
	for (int ch : { 10, 2 }) {
		for (int note : { 36, 38, 42, 46, 49, 60 }) {
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(ch, note, (juce::uint8)100), 0);
			const float peak = render(proc, 0.5, &on);
			std::printf("   pending=%zu serialRxReady=%d lamp=%d\n", proc.getCore().midiQueuePendingForTest(), (int)proc.getCore().serialRxReadyForTest(), (int)proc.getCore().midiLampOn());
			std::printf("ch %2d note %3d peak %.4f partStates 0x%x\n", ch, note, peak, proc.enginePartStates());
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(ch, note), 0);
			render(proc, 0.4, &off);
		}
	}
	return 0;
}
