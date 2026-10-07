#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

// Drops a Pitch Bend of exactly 0 on a channel until a real one (!= 0) has been seen there.
// Real-time safe given a pre-sized scratch buffer. seen is indexed by MIDI channel (1-16).
inline void dropRogueInitialPitchBend(juce::MidiBuffer &midi, juce::MidiBuffer &scratch,
                                      std::array<bool, 17> &seen) {
	bool rogue = false;
	for (const auto meta : midi) {
		const auto m = meta.getMessage();
		if (m.isPitchWheel() && m.getPitchWheelValue() == 0 && !seen[(size_t) m.getChannel()]) {
			rogue = true;
			break;
		}
	}
	if (!rogue) {
		for (const auto meta : midi) {
			const auto m = meta.getMessage();
			if (m.isPitchWheel()) seen[(size_t) m.getChannel()] = true;
		}
		return;
	}
	scratch.clear();
	for (const auto meta : midi) {
		const auto m = meta.getMessage();
		if (m.isPitchWheel()) {
			if (m.getPitchWheelValue() == 0 && !seen[(size_t) m.getChannel()]) continue;
			seen[(size_t) m.getChannel()] = true;
		}
		scratch.addEvent(m, meta.samplePosition);
	}
	midi.swapWith(scratch);
}
