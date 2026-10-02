// Repro for "channel 10 / R makes no sound": plays single rhythm keys on the rhythm channel
// on a freshly booted native core and reports the audio peak and the engine's part-state bits.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <string>
#include <algorithm>
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
	if (std::getenv("DEBUGLOG")) proc.setDebugModeEnabled(true);
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
			const int secs = std::getenv("SECS") ? std::atoi(std::getenv("SECS")) : 12;
			uint64_t lastHits = proc.getCore().stuckLoopHitsForTest();
			int wedgedRun = 0, wedgedMax = 0;
			for (int sec = 0; sec < secs; ++sec) {
				pk = juce::jmax(pk, render(proc, 1.0));
				const uint64_t hits = proc.getCore().stuckLoopHitsForTest();
				// a firmware parked at the dispatch wait loop spins there thousands of times a second
				const bool spinning = hits - lastHits > 20000;
				wedgedRun = spinning ? wedgedRun + 1 : 0;
				wedgedMax = std::max(wedgedMax, wedgedRun);
				if (spinning && (wedgedRun == 1 || wedgedRun % 10 == 0))
					std::printf("  t=%3ds: firmware parked at the dispatch wait (%llu hits/s), run=%d s\n", sec,
					            (unsigned long long)(hits - lastHits), wedgedRun);
				lastHits = hits;
			}
			std::printf("longest stuck-wait run: %d s\n", wedgedMax);
			std::printf("played 12 s, peak %.4f partStates 0x%x\n", pk, proc.enginePartStates());
			rhythmNonZero();
			proc.getSequencer().stop();
			render(proc, 1.0);
		}
	}
	if (std::getenv("SYSEX")) {
		// Messages the D-110 has no use for (they are in the garvalf file): GM On, GS Reset, and a
		// SysEx that never terminates properly would be worse, but these two are real.
		static const juce::uint8 gmOn[] = { 0x7e, 0x7f, 0x09, 0x01 };
		static const juce::uint8 gsReset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41 };
		proc.injectMidiMessage(juce::MidiMessage::createSysExMessage(gmOn, sizeof(gmOn)));
		render(proc, 0.5);
		proc.injectMidiMessage(juce::MidiMessage::createSysExMessage(gsReset, sizeof(gsReset)));
		render(proc, 0.5);
		std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		const int before = ram[size_t(D110CoreType::kRamPatchNumber)];
		proc.getCore().setButton(D110CoreType::buttonIndex(0, 6), true); render(proc, 0.15);
		proc.getCore().setButton(D110CoreType::buttonIndex(0, 6), false); render(proc, 0.5);
		proc.getCore().setButton(D110CoreType::buttonIndex(0, 1), true); render(proc, 0.15);
		proc.getCore().setButton(D110CoreType::buttonIndex(0, 1), false); render(proc, 0.8);
		proc.getCore().getRam(ram.data());
		std::printf("after GM On + GS Reset: patch number %d -> %d (Number+ %s)\n", before,
		            ram[size_t(D110CoreType::kRamPatchNumber)],
		            ram[size_t(D110CoreType::kRamPatchNumber)] != before ? "answered" : "IGNORED");
	}
	if (std::getenv("CYCLES")) {
		// play / stop / play ... on the loaded song, sampling the firmware every 100 ms: parked at the dispatch
		// wait (pc 0x29E9/0x29EE) with no slot for its context, for over 2 s, is the wedge seen in the DEBUG log.
		const int cycles = std::atoi(std::getenv("CYCLES"));
		int wedges = 0;
		for (int c = 0; c < cycles; ++c) {
			proc.getSequencer().play();
			int parked = 0, longest = 0;
			const int playTenths = std::getenv("PLAYSECS") ? int(10 * std::atof(std::getenv("PLAYSECS"))) : 150;
			for (int i = 0; i < playTenths; ++i) { // PLAYSECS of play (default 15)
				render(proc, 0.1);
				const auto fw = proc.getCore().firmwareDiag();
				const bool inWait = (fw.pc == 0x29E9 || fw.pc == 0x29EE) && fw.slotForContext < 0;
				parked = inWait ? parked + 1 : 0;
				longest = std::max(longest, parked);
			}
			std::printf("cycle %d: longest parked-at-wait run %.1f s, forced releases so far %llu\n", c, longest * 0.1,
			            (unsigned long long)proc.getCore().firmwareDiag().unmatchedWaitReleases);
			wedges += longest > 20;
			proc.getSequencer().stop();
			// The panel's STOP button also calls midiPanic(), which keeps resetting the voice-slot table for 1.5 s;
			// PANIC_GAP is how long the user waits before pressing play again.
			if (std::getenv("PANIC_GAP")) { proc.midiPanic(); render(proc, std::atof(std::getenv("PANIC_GAP"))); }
			else render(proc, 1.0);
		}
		std::printf("cycles with a wedge: %d/%d\n", wedges, cycles);
	}
	if (std::getenv("HOLD_PANIC")) {
		// A held note, then the sequencer's STOP panic (midiPanic) vs the explicit one (midiPanicHard).
		for (int hard = 0; hard < 2; ++hard) {
			for (int ch : { 2, 3, 10 }) {
				juce::MidiBuffer on;
				on.addEvent(juce::MidiMessage::noteOn(ch, ch == 10 ? 49 : 60, (juce::uint8)110), 0);
				render(proc, 0.01, &on);
			}
			render(proc, 1.0);
			const int before = proc.engineActivePartials();
			if (hard) proc.midiPanicHard(); else proc.midiPanic();
			std::printf("%s: partials before panic %d", hard ? "midiPanicHard" : "midiPanic    ", before);
			for (double t : { 0.3, 1.0, 2.5 }) {
				render(proc, t == 0.3 ? 0.3 : (t == 1.0 ? 0.7 : 1.5));
				std::printf(" | after %.1fs: %d", t, proc.engineActivePartials());
			}
			std::printf("\n");
		}
	}
	if (std::getenv("NO_MATCH")) {
		// Every wait looks unmatched: the last-resort release has to keep the firmware moving.
		std::printf("stuck policy = %d (4 = La32Ramps)\n", int(proc.getCore().stuckPolicy()));
		proc.getCore().setIgnoreSlotMatchForTest(true);
		const uint64_t before = proc.getCore().firmwareDiag().midiDelivered;
		int parked = 0, longest = 0;
		proc.getSequencer().play();
		int activeBlocks = 0;
		float peakAll = 0;
		for (int i = 0; i < 300; ++i) {
			peakAll = juce::jmax(peakAll, render(proc, 0.1));
			activeBlocks += proc.enginePartStates() != 0;
			const auto fw = proc.getCore().firmwareDiag();
			parked = ((fw.pc == 0x29E9 || fw.pc == 0x29EE) && fw.slotForContext < 0) ? parked + 1 : 0;
			longest = std::max(longest, parked);
		}
		const auto fw = proc.getCore().firmwareDiag();
		std::printf("NO_MATCH: engine parts active in %d/300 steps, peak %.3f\n", activeBlocks, peakAll);
		std::printf("NO_MATCH 30 s: longest parked run %.1f s, forced releases %llu, MIDI bytes delivered %llu\n", longest * 0.1,
		            (unsigned long long)fw.unmatchedWaitReleases, (unsigned long long)(fw.midiDelivered - before));
		proc.getSequencer().stop();
		proc.getCore().setIgnoreSlotMatchForTest(false);
	}
	if (std::getenv("PANIC_BURST")) {
		// midiPanic() resets the voice-slot table every block for 1.5 s. Play dense chords through that
		// whole window and see whether the firmware ends up parked at the dispatch wait.
		int wedges = 0;
		const int rounds = std::atoi(std::getenv("PANIC_BURST"));
		for (int r = 0; r < rounds; ++r) {
			proc.midiPanic();
			int parked = 0, longest = 0;
			const bool keepPanicking = std::getenv("PANIC_CONTINUOUS") != nullptr;
			for (int step = 0; step < 150; ++step) { // 15 s: 1.5 s of window, then the rest as the aftermath
				if (keepPanicking && step % 10 == 0) proc.midiPanic();
				juce::MidiBuffer m;
				if (step < (keepPanicking ? 150 : 40)) {
					for (int k = 0; k < 4; ++k) {
						m.addEvent(juce::MidiMessage::noteOn(2 + (step + k) % 8, 48 + ((step * 5 + k * 4) % 30), (juce::uint8)100), 0);
						m.addEvent(juce::MidiMessage::noteOff(2 + (step + k + 3) % 8, 48 + (((step - 3) * 5 + k * 4) % 30)), 8);
					}
				}
				render(proc, 0.1, &m);
				const auto fw = proc.getCore().firmwareDiag();
				const bool inWait = (fw.pc == 0x29E9 || fw.pc == 0x29EE) && fw.slotForContext < 0;
				parked = inWait ? parked + 1 : 0;
				longest = std::max(longest, parked);
			}
			std::printf("burst %d: longest parked-at-wait run %.1f s, forced releases so far %llu\n", r, longest * 0.1,
			            (unsigned long long)proc.getCore().firmwareDiag().unmatchedWaitReleases);
			wedges += longest > 20;
		}
		std::printf("bursts with a wedge: %d/%d\n", wedges, rounds);
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
