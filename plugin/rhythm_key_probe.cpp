// "Closed hat on channel ten does not sound, maybe something else too."
//
// The Rhythm Setup map turned out NOT to be the cause: key 42 (Closed Hi-Hat per GM) carries timbre 64
// both in the unit's current memory and in the factory dump D110-ALL.SYX (compared byte by byte) - so
// the assignment is right, and keys 24-34 are empty LEGITIMATELY, that is the factory state of the rhythm section,
// which on the D-110 starts at note 35, not a broken reset.
//
// So the question is not "what is in the table" but "what happens BELOW when the engine receives this
// assignment". The probe plays in turn all keys whose Rhythm Setup currently carries a REAL
// timbre (not OFF), and measures each one's peak - the list of silent ones comes from direct measurement, not
// from a single suspect.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kRhythmChannel = 10; // channel 10, part 8 on a factory unit

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (b == 0 && midi) ? *midi : none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

float peakOf(D110AudioProcessor &proc, int note) {
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kRhythmChannel, note, 0.95f), 0);
	juce::AudioBuffer<float> buffer(2, kBlock);
	float peak = 0.0f;
	const int blocks = int(0.5 * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, b == 0 ? on : none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
		peak = juce::jmax(peak, buffer.getMagnitude(0, 0, buffer.getNumSamples()));
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(kRhythmChannel, note), 0);
	render(proc, 0.6, &off);
	return peak;
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

	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());

	std::printf("клавиша | тембр | пик       | \n");
	int silent = 0, sounded = 0;
	for (int keyIdx = 0; keyIdx < D110Core::kNumRhythmKeys; ++keyIdx) {
		const int note = D110Core::kRhythmFirstKey + keyIdx;
		const int at = D110Core::kRamRhythmTemp + keyIdx * D110Core::kRhythmRecord;
		const int tembr = ram[(size_t)at];
		if (tembr == 127) continue; // OFF per the map - it should not sound here

		const float peak = peakOf(proc, note);
		const char *note_name = nullptr;
		switch (note) {
			case 35: note_name = "Acoustic Bass Drum"; break;
			case 36: note_name = "Bass Drum 1"; break;
			case 38: note_name = "Acoustic Snare"; break;
			case 42: note_name = "Closed Hi-Hat"; break;
			case 44: note_name = "Pedal Hi-Hat"; break;
			case 46: note_name = "Open Hi-Hat"; break;
			case 49: note_name = "Crash Cymbal 1"; break;
			case 51: note_name = "Ride Cymbal 1"; break;
			default: note_name = ""; break;
		}
		std::printf("  %5d | %5d | %.6f  %s%s\n", note, tembr, peak,
		            peak < 1e-5f ? "*** ТИШИНА ***  " : "", note_name);
		if (peak < 1e-5f) ++silent; else ++sounded;
	}
	std::printf("\nитого: звучит %d, тишина %d\n", sounded, silent);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
