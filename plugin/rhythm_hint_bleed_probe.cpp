// Reproduces exactly what was noticed by ear: you play a working drum (kick, key
// 35), then Closed Hi-Hat (key 42, timbre 64 - one of the three broken ones) - and the hi-hat
// sounds like the NEIGHBOURING sound, as if the kick had "imprinted". The cause was that the hint was queued
// for ANY rhythm note, but is only consumed by the three broken keys - the kick clogged the
// queue, and the hi-hat got somebody else's key.
//
// Checked through the NoteLog: it carries ev.note AFTER the substitution - the very value that actually
// went into the engine. If the fix works, for key 42 it is always 42, regardless of what was played
// before it.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	bool first = true;
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (first && midi) ? *midi : none);
		first = false;
	}
}

void hit(D110AudioProcessor &proc, int note) {
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(10, note, 0.9f), 0);
	render(proc, 0.4, &on);
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(10, note), 0);
	render(proc, 0.3, &off);
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

	int failures = 0;
	// Scenario from the complaint: a series of "foreign" strikes (kick, then a snare too), then a hi-hat -
	// and so ten times in a row, to catch even a rarely manifesting anomaly.
	for (int round = 1; round <= 10; ++round) {
		proc.getCore().takeNoteLog();
		proc.getCore().startNoteLog();

		hit(proc, 35); // kick - a working key, used to clog the queue
		hit(proc, 38); // snare - also working
		hit(proc, 42); // Closed Hi-Hat - broken, needs substitution

		const auto events = proc.getCore().takeNoteLog();
		int hatNote = -1;
		for (const auto &e : events)
			if (e.on && e.part == 8) hatNote = int(e.note); // the last one in order is the hi-hat
		const bool ok = (hatNote == 42);
		std::printf("круг %2d: клавиша 42 дошла до движка как %3d  %s\n", round, hatNote,
		            ok ? "верно" : "*** ОШИБКА ***");
		if (!ok) ++failures;
	}

	std::printf("\nитого: %d ошибок из 10\n", failures);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return failures > 0 ? 1 : 0;
}
