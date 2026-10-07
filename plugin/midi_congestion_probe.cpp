// How far apart the times really are at which the firmware LEARNS of a note when several
// parts strike at once - what used to be explained in words (a shared MIDI queue to the
// firmware at real cable speed, 3125 bytes/s) is measured here for real.
//
// D110Core already carries a ready-made tool for this: startNoteLog()/takeNoteLog() stamps
// a REAL-time mark (steady_clock, milliseconds since recording started) on every note
// the firmware itself registered in its tables. This is not the time the MIDI went into the
// plugin but the time the firmware ACTUALLY learned of it - exactly the link where the
// delay of the shared serial queue lives.
//
// Experiment: nine parts (eight voice + rhythm) strike EXACTLY simultaneously - all in one
// MidiBuffer at position zero inside one processBlock, the way a host would deliver them on a
// dense beat - for eight beats in a row in REAL time (120 bpm, not sped up). The beat number
// is baked into the velocity, so a NoteLog entry can be unambiguously attributed to its beat,
// rather than guessed from how close the marks are.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kBeats = 8;
constexpr double kBeatMs = 500.0; // 120 bpm
// Parts 1-8 answer on channels 2-9, rhythm on 10 (factory map).
constexpr int kChannels[9] = { 2, 3, 4, 5, 6, 7, 8, 9, 10 };

void renderBlocks(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
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
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	std::printf("прошивка работает, начинаю плотный паттерн - %d долей по %.0f мс "
	            "(120 уд/мин), девять партий разом на каждой\n\n",
	            kBeats, kBeatMs);

	proc.getCore().startNoteLog();
	const auto t0 = std::chrono::steady_clock::now();

	for (int beat = 0; beat < kBeats; ++beat) {
		// Wait REAL time until its beat - not sped up, otherwise the measurement would not show
		// what a player at a real tempo runs into.
		const double targetMs = beat * kBeatMs;
		while (std::chrono::duration<double, std::milli>(
		           std::chrono::steady_clock::now() - t0)
		           .count() < targetMs)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));

		juce::MidiBuffer hit;
		// The beat number is baked into the velocity (10 + beat), so a NoteLog entry can be
		// unambiguously attributed to its beat - by value, not by how close the time marks are.
		for (int ch : kChannels)
			hit.addEvent(juce::MidiMessage::noteOn(ch, 48, juce::uint8(10 + beat)), 0);
		juce::AudioBuffer<float> audio(2, kBlock);
		audio.clear();
		proc.processBlock(audio, hit);

		// Release after 80 ms so voices do not pile up needlessly; this does not affect measuring
		// the note-on registration.
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		juce::MidiBuffer off;
		for (int ch : kChannels) off.addEvent(juce::MidiMessage::noteOff(ch, 48), 0);
		juce::AudioBuffer<float> audio2(2, kBlock);
		audio2.clear();
		proc.processBlock(audio2, off);
	}

	render(proc, 1.5); // let the tail of the queue reach the firmware

	const auto log = proc.getCore().takeNoteLog();
	const auto dropped = proc.getCore().noteLogDropped_();
	std::printf("записей в журнале: %d, потеряно: %llu\n\n", int(log.size()),
	            (unsigned long long)dropped);

	std::printf("=== разброс регистрации ВНУТРИ каждой доли (девять партий, ударили разом) ===\n");
	double worstSpread = 0.0, worstDriftMs = 0.0;
	int worstDriftBeat = -1;
	double firstBeatFirstMs = -1.0;
	for (int beat = 0; beat < kBeats; ++beat) {
		std::vector<double> ms;
		for (const auto &e : log)
			if (e.on && e.velocity == 10 + beat) ms.push_back(e.ms);
		if (ms.empty()) {
			std::printf("  доля %d: НИ ОДНА нота не зарегистрирована\n", beat + 1);
			continue;
		}
		std::sort(ms.begin(), ms.end());
		const double spread = ms.back() - ms.front();
		worstSpread = std::max(worstSpread, spread);
		if (beat == 0) firstBeatFirstMs = ms.front();
		// Lag of the first note of a beat behind where it would sit if every
		// beat started from a clean slate - i.e. from the ideal grid counted from the first beat.
		const double intendedMs = firstBeatFirstMs + beat * kBeatMs;
		const double driftMs = ms.front() - intendedMs;
		if (std::abs(driftMs) > std::abs(worstDriftMs)) { worstDriftMs = driftMs; worstDriftBeat = beat; }
		std::printf("  доля %d: нот зарегистрировано %2d/9   разброс внутри доли %.2f мс   "
		            "отставание первой ноты от сетки %+.2f мс\n",
		            beat + 1, int(ms.size()), spread, driftMs);
	}

	std::printf("\nсамый большой разброс внутри одной доли: %.2f мс "
	            "(теоретический потолок для девяти трёхбайтных нот подряд: %.2f мс)\n",
	            worstSpread, (9 * 3 - 1) * 1000.0 / D110Core::kMidiBytesPerSecond);
	std::printf("самое большое отставание от сетки: %.2f мс, на доле %d%s\n", worstDriftMs,
	            worstDriftBeat + 1,
	            std::abs(worstDriftMs) > kBeatMs * 0.05
	                ? "  <-- растёт от доли к доле, а не только разброс внутри удара"
	                : "  (в пределах разброса одного удара, не накапливается)");

	// --- stress: where exactly the average density starts to exceed the channel ---------
	//
	// A moderate strike (above) accumulates no lag at all - the average load there is absurdly
	// small (27 bytes every 500 ms = 54 bytes/s against a 3125 bytes/s channel). Here the density
	// is raised with a real stream - sixteenth notes across three drum parts at once - and the queue
	// is watched DIRECTLY through the byte counters (midiForwarded/midiDelivered), not indirectly
	// through the note log: the difference between them is exactly what still sits in the queue and
	// has not reached the firmware.
	std::printf("\n=== СТРЕСС: сплошной плотный поток, слежение за очередью напрямую ===\n");
	{
		// Chosen so that the AVERAGE load exceeds the channel (3125 bytes/s) rather than staying
		// close to it: 9 parts, note+release each (6 bytes) every 10 ms - that is 5400 bytes/s,
		// 173% of the channel. This is no longer a "fast drum fill" but a deliberate excess - the question is
		// not "does this happen in music" but "what happens to the queue when it does".
		constexpr double kStepMs = 10.0;
		constexpr int kVoices = 9;
		constexpr int kSteps = 200; // 2 seconds of stream
		const uint64_t beforeSent = proc.getCore().midiForwarded();
		const auto t1 = std::chrono::steady_clock::now();
		double peakBacklogBytes = 0.0;
		double peakBacklogMs = 0.0;

		for (int step = 0; step < kSteps; ++step) {
			const double targetMs = step * kStepMs;
			while (std::chrono::duration<double, std::milli>(
			           std::chrono::steady_clock::now() - t1)
			           .count() < targetMs)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));

			juce::MidiBuffer hit;
			for (int v = 0; v < kVoices; ++v)
				hit.addEvent(juce::MidiMessage::noteOn(kChannels[v], 36 + v, juce::uint8(100)), 0);
			for (int v = 0; v < kVoices; ++v)
				hit.addEvent(juce::MidiMessage::noteOff(kChannels[v], 36 + v), 0);
			juce::AudioBuffer<float> audio(2, kBlock);
			audio.clear();
			proc.processBlock(audio, hit);

			const uint64_t sent = proc.getCore().midiForwarded();
			const uint64_t got = proc.getCore().midiDelivered();
			const double backlogBytes = double(sent - got);
			const double backlogMs = backlogBytes * 1000.0 / D110Core::kMidiBytesPerSecond;
			peakBacklogBytes = std::max(peakBacklogBytes, backlogBytes);
			peakBacklogMs = std::max(peakBacklogMs, backlogMs);
		}

		render(proc, 2.0); // let the queue drain everything that accumulated
		const uint64_t afterSent = proc.getCore().midiForwarded();
		const uint64_t afterGot = proc.getCore().midiDelivered();
		const double avgBytesPerSec = double(afterSent - beforeSent)
		                            / (kSteps * kStepMs / 1000.0);

		std::printf("  шаг %.1f мс (16-е при 240 уд/мин), %d партий разом, %d шагов\n",
		            kStepMs, kVoices, kSteps);
		std::printf("  средняя нагрузка потока: %.0f байт/с из %.0f байт/с канала (%.0f%%)\n",
		            avgBytesPerSec, D110Core::kMidiBytesPerSecond,
		            100.0 * avgBytesPerSec / D110Core::kMidiBytesPerSecond);
		std::printf("  пик очереди во время потока: %.0f байт (~%.0f мс отставания)\n",
		            peakBacklogBytes, peakBacklogMs);
		std::printf("  очередь после потока и 2с ожидания: %llu байт (%s)\n",
		            (unsigned long long)(afterSent - afterGot),
		            (afterSent - afterGot) == 0 ? "полностью слита" : "ОСТАЛОСЬ ВИСЕТЬ");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
