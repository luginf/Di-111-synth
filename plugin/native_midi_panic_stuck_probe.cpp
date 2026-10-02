// Alan's report, 2026-08-30: notes sometimes stay audibly stuck, the Monitor tab's LA32
// voice-slot grid stays lit, and pressing MIDI PANIC (Utility tab) doesn't reset either -
// the CC64/CC123 messages midiPanic() already sent only ever reach the firmware's NORMAL
// release path, which needs its per-voice envelope-stage counter to count up to 7 before it
// ever writes the release bit (see D110CoreNative::releaseStuckNoteContexts()/
// resetVoiceSlotTable()'s own comments, and docs/la32_interface.md, "solved kept the panel
// alive, but not the polyphony") - pacing this emulation doesn't implement, so a context can
// get stuck sounding forever with no firmware action left to free it.
//
// This reproduces the simplest version of "stuck": hold a note down and never release it -
// engineActivePartials() stays > 0 and at least one LA32 voice slot stays busy, same
// observable symptom Alan describes regardless of exactly how a real release write goes
// missing. Then it calls the real midiPanic() (the actual button's own code path, queued and
// applied from processBlock() exactly as production does - releaseStuckNoteContexts() once,
// resetVoiceSlotTable() repeated for ~1.5s, see PluginProcessor::midiPanic()'s own comment for
// why the second one needs repeating) and checks that BOTH the engine audio and the firmware's
// own voice-slot table actually clear.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map

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

int busySlotCount(D110AudioProcessor &proc, bool verbose = false) {
	std::vector<uint8_t> ram(size_t(D110CoreType::kRamSize), 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int busy = 0;
	for (int s = 0; s < D110CoreType::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110CoreType::kSlotStateTable) + size_t(s) * 2;
		const int state = int(ram[at]);
		if (state == D110CoreType::kSlotBusyValue || state == D110CoreType::kSlotBusyValueAlt) {
			++busy;
			if (verbose) std::printf("    slot %d busy (state=0x%02x)\n", s, state);
		}
	}
	return busy;
}
} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	int failures = 0;
	auto check = [&](bool cond, const char *what) {
		std::printf("%s %s\n", cond ? " ok " : "FAIL", what);
		if (!cond) ++failures;
	};

	// Hold a note down and never release it - the "stuck" condition, engine-side. Checked
	// shortly after triggering, not after a long wait: some patches' own TVA envelope decays
	// to silence within a second or so even while conceptually still "held" (no note-off
	// needed for THAT to happen) - checking early catches the partial while it's still
	// genuinely sounding, which is the actual "son qui reste" symptom being reproduced here.
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kChannel, 60, (juce::uint8)100), 0);
	renderBlocks(proc, 1, &on);
	render(proc, 0.2); // let it actually reach the engine, not decay away on its own

	const int activeBefore = proc.engineActivePartials();
	const int busyBefore = busySlotCount(proc, true);
	check(activeBefore > 0, "note held: engine has an active partial before panic");
	check(busyBefore > 0, "note held: at least one LA32 voice slot reads busy before panic");
	std::printf("  activePartials=%d busySlots=%d\n", activeBefore, busyBefore);

	// The actual production code path: the Utility tab's MIDI PANIC button calls exactly this.
	proc.midiPanicHard();
	render(proc, 0.3); // releaseStuckNoteContexts() itself is one-shot/immediate, but the
	                   // resulting note-off still has to drain through popNoteEvent() into
	                   // synth->playMsgOnPart() and mt32emu's own (near-instant, not literally
	                   // zero-latency) release - a short settle window, not the full repeat
	                   // window resetVoiceSlotTable() separately needs below.
	const int activeAfter = proc.engineActivePartials();
	check(activeAfter == 0, "midiPanic() silences the stuck partial quickly "
	                         "(engineActivePartials == 0)");
	std::printf("  activePartials=%d busySlots=%d (still settling)\n", activeAfter,
	            busySlotCount(proc));

	// resetVoiceSlotTable() repeats for ~1.5s (see its own comment) precisely because the
	// firmware's own delayed response to the panic bytes can overwrite a single early poke -
	// give it the FULL window before judging the slot table, not just one block.
	render(proc, 1.6);
	const int busyAfter = busySlotCount(proc, true);
	check(busyAfter == 0, "midiPanic() clears every LA32 voice slot back to idle within its "
	                       "repeat window (Monitor grid)");
	std::printf("  busySlots=%d after the full repeat window\n", busyAfter);

	// A second panic on an already-quiet instrument must not be destructive/crash.
	proc.midiPanicHard();
	render(proc, 0.3);
	check(proc.getCore().isRunning(), "a second panic on an already-silent instrument is harmless");

	std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURE(S)\n", failures);
	return failures == 0 ? 0 : 1;
}
