// Targeted repro for the hypothesised La32Ramps freeze mechanism: serviceStuckPolicy()'s
// La32Ramps branch used to promote the NEXT queued ramp landing in the same tick the previous
// one was read, skipping the EXTINT falling edge entirely (Mcs96Cpu::setExtIntLine() only
// clears pending_irq's EXTINT bit on a real high-to-low transition - level 7 is deliberately
// excluded from the CPU's own auto-clear-on-take logic, so nothing else ever clears it). Two
// ramps landing close enough together - which single, fully-released notes essentially never
// do, but overlapping/legato notes and chords routinely do - meant the CPU could re-take the
// same never-cleared interrupt forever once PSW.F_I came back up after each RETI: a tight
// ISR-retrigger loop that keeps retiring instructions (so a same-PC hang check misses it) while
// the firmware's own mainline code - note dispatch, LCD refresh - never runs again.
//
// A same-PC check can't catch this (a tight loop visits several addresses, not one), so this
// probe instead watches for the actual user-visible symptom: RAM content going static despite
// continued MIDI input and continued firmware note-on/off completions. rampQueueDepthForTest()
// is also watched for an unbounded backlog, which a permanently-undelivered interrupt would
// also produce (every later landing just piles up in rampLanded_ forever).
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// ROM folder: D110_ROM_DIR (no default - it is a user choice, not tied to a plugin format).
	const char *romEnv = std::getenv("D110_ROM_DIR");
	if (romEnv == nullptr) {
		std::printf("set D110_ROM_DIR to the folder holding the ROM files\n");
		return 2;
	}
	const std::string romDir = romEnv;
	const char *nvramDir = argc > 1 ? argv[1] : "/tmp/native_stress_nvram";

	D110CoreNative core;
	if (!core.start(romDir, nvramDir)) return 1;
	// Control group: pass "stub" as the 2nd arg to run the exact same note pattern under
	// La32Stub instead, to tell "this pattern breaks the firmware regardless of policy" apart
	// from "this pattern specifically breaks La32Ramps".
	const bool useStub = argc > 2 && std::string(argv[2]) == "stub";
	core.setStuckPolicy(useStub ? D110CoreNative::StuckPolicy::La32Stub
	                             : D110CoreNative::StuckPolicy::La32Ramps);
	std::printf("policy: %s\n", useStub ? "La32Stub" : "La32Ramps");
	core.runForSeconds(9.0);

	D110CoreNative::NoteEvent ev;
	auto drain = [&]() { while (core.popNoteEvent(ev)) {} };
	drain();

	// Dense, genuinely overlapping legato: each note's off comes AFTER the next note's on, and
	// several notes/chords are in flight at once - the shape the single-note and
	// hold/release/hold/release probes never produced. 400 rounds x up to 4 simultaneous voices
	// over real emulated time, well past what any earlier probe in this project ran for.
	constexpr int kRounds = 400;
	uint8_t ram[D110CoreNative::kRamSize];
	uint8_t prevRam[D110CoreNative::kRamSize];
	core.getRam(prevRam);
	int staleStreak = 0, maxStaleStreak = 0;
	size_t maxQueueDepth = 0;
	uint64_t lastOnCount = 0, lastOffCount = 0;
	int stalledRounds = 0;

	for (int round = 0; round < kRounds; ++round) {
		const uint8_t base = uint8_t(36 + (round * 5) % 40);
		const int voices = 2 + (round % 3); // 2..4 overlapping voices per round
		uint8_t notes[4];
		for (int v = 0; v < voices; ++v) {
			notes[v] = uint8_t(base + v * 3);
			const uint8_t on[3] = { uint8_t(0x91 + (v & 1)), notes[v], uint8_t(90 + v * 5) };
			core.pushMidi(on, 3);
			core.runForSeconds(0.012); // legato spacing - next note starts well before this chord releases
		}
		// Release the PREVIOUS round's notes now (still-overlapping tail), not this round's -
		// deliberately keeps two rounds' worth of voices sounding at once.
		if (round > 0) {
			const uint8_t prevBase = uint8_t(36 + ((round - 1) * 5) % 40);
			const int prevVoices = 2 + ((round - 1) % 3);
			for (int v = 0; v < prevVoices; ++v) {
				const uint8_t off[3] = { uint8_t(0x81 + (v & 1)), uint8_t(prevBase + v * 3), 0 };
				core.pushMidi(off, 3);
			}
		}
		core.runForSeconds(0.02);
		drain();

		maxQueueDepth = std::max(maxQueueDepth, core.rampQueueDepthForTest());

		core.getRam(ram);
		const bool same = std::memcmp(ram, prevRam, sizeof(ram)) == 0;
		if (same) { ++staleStreak; maxStaleStreak = std::max(maxStaleStreak, staleStreak); }
		else staleStreak = 0;
		std::memcpy(prevRam, ram, sizeof(ram));

		if (core.firmwareNoteOns() == lastOnCount && core.firmwareNoteOffs() == lastOffCount)
			++stalledRounds;
		lastOnCount = core.firmwareNoteOns();
		lastOffCount = core.firmwareNoteOffs();

		if (round % 50 == 0)
			std::printf("round %3d: pc=%04x queueDepth=%zu onCount=%llu offCount=%llu staleStreak=%d\n",
			            round, core.pcForTest(), core.rampQueueDepthForTest(),
			            (unsigned long long)core.firmwareNoteOns(), (unsigned long long)core.firmwareNoteOffs(),
			            staleStreak);
	}

	// Let everything ring out and settle, then confirm the machine is still alive: a real hang
	// stays hung through a further multi-second wait, it doesn't recover on its own.
	core.runForSeconds(3.0);
	drain();
	const uint16_t pcBefore = core.pcForTest();
	core.runForSeconds(1.0);
	const uint16_t pcAfter = core.pcForTest();

	std::printf("\nmax RAM-static streak: %d consecutive rounds (~%.0fms each)\n", maxStaleStreak, 32.0);
	std::printf("rounds with zero note-on/off progress: %d/%d\n", stalledRounds, kRounds);
	std::printf("max rampLanded_ backlog seen: %zu\n", maxQueueDepth);
	std::printf("final: pc %04x -> %04x after 1s idle (%s)\n", pcBefore, pcAfter,
	            pcBefore != pcAfter ? "moved" : "SAME - suspicious");

	// A few consecutive static rounds is normal (nothing changed to mirror that tick); dozens
	// in a row while notes keep going in is the freeze signature.
	const bool suspect = maxStaleStreak > 30 || stalledRounds > kRounds / 2;
	std::printf("\n%s\n", suspect ? "SUSPECT FREEZE" : "no freeze signature over this run");
	return suspect ? 1 : 0;
}
