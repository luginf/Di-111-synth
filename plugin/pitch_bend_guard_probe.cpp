// Headless test for dropRogueInitialPitchBend() (GitHub issue #8 / Carla parameter reset).
// No firmware or ROMs needed.
#include "Source/PitchBendGuard.h"

#include <cstdio>

static int count(const juce::MidiBuffer &b) {
	int n = 0;
	for (const auto m : b) { (void) m; ++n; }
	return n;
}

int main() {
	int fails = 0;
	auto check = [&](bool ok, const char *what) {
		std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
		if (!ok) ++fails;
	};
	juce::MidiBuffer scratch;
	scratch.ensureSize(1024);
	std::array<bool, 17> seen{};

	juce::MidiBuffer b;
	b.addEvent(juce::MidiMessage::pitchWheel(1, 0), 0);
	b.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 1);
	dropRogueInitialPitchBend(b, scratch, seen);
	check(count(b) == 1, "initial bend 0 dropped, note kept");

	b.clear();
	b.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
	dropRogueInitialPitchBend(b, scratch, seen);
	check(count(b) == 1, "centre bend passes");

	b.clear();
	b.addEvent(juce::MidiMessage::pitchWheel(1, 0), 0);
	dropRogueInitialPitchBend(b, scratch, seen);
	check(count(b) == 1, "bend 0 passes once a real bend was seen");

	b.clear();
	b.addEvent(juce::MidiMessage::pitchWheel(2, 0), 0);
	dropRogueInitialPitchBend(b, scratch, seen);
	check(count(b) == 0, "other channel is tracked independently");

	return fails;
}
