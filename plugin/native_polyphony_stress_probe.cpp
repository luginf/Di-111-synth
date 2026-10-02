// The user's report: playing fast, many notes in succession, and some don't sound. Compares
// StuckPolicy::La32Stub (answers "voice dispatched" but never "voice finished", so all 32
// hardware slots fill up and stay busy forever) against StuckPolicy::La32Ramps (the real
// envelope model - a voice's slot genuinely frees once its ramp lands, exactly as the real
// chip would report it) under a fast run of many short notes - la32Stub should start losing
// notes once the firmware's own 32-slot table fills; la32Ramps should keep taking them
// because slots actually come back.
#include "Source/native/D110CoreNative.h"

#include <cstdio>

namespace {

// Plays `count` short notes back to back (each held briefly, then released, minimal gap -
// "playing fast") and returns how many were registered by the firmware (popNoteEvent ON)
// versus how many were sent.
int runBurst(D110CoreNative &core, int count, const char *label) {
	int registered = 0;
	D110CoreNative::NoteEvent ev;
	for (int i = 0; i < count; ++i) {
		const uint8_t note = uint8_t(36 + (i % 48)); // spread across the keyboard
		const uint8_t on[3] = { 0x91, note, 100 };
		core.pushMidi(on, 3);
		core.runForSeconds(0.05); // "fast": 50ms per note, on+off
		const uint8_t off[3] = { 0x81, note, 0 };
		core.pushMidi(off, 3);
		core.runForSeconds(0.03);

		bool sawThisOne = false;
		while (core.popNoteEvent(ev))
			if (ev.on && ev.note == note) sawThisOne = true;
		if (sawThisOne) ++registered;
	}
	core.runForSeconds(0.5);
	while (core.popNoteEvent(ev)) {} // drain trailing offs, not counted

	// The more direct measurement: does the firmware's OWN hardware-voice slot table
	// actually show slots coming free, or are all 32 stuck "busy" regardless of note-off?
	// This is what actually gates whether a LATER note gets a voice at all - noteWatch()
	// completing (registered, above) only proves the firmware logged the note in its RAM
	// bookkeeping, not that a hardware voice was successfully behind it.
	uint8_t ram[D110CoreNative::kRamSize];
	core.getRam(ram);
	int busy = 0;
	for (int n = 0; n < D110CoreNative::kNumHardwareVoices; ++n) {
		const uint8_t state = ram[D110CoreNative::kSlotStateTable + 2 * n];
		if (state == D110CoreNative::kSlotBusyValue || state == D110CoreNative::kSlotBusyValueAlt) ++busy;
	}
	std::printf("%-12s: %d/%d notes registered by the firmware, %d/%d hardware slots still busy after settling\n",
	            label, registered, count, busy, D110CoreNative::kNumHardwareVoices);
	return busy;
}

} // namespace

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
	constexpr int kNotes = 60;

	int stubBusy = 0, rampsBusy = 0;

	{
		D110CoreNative core;
		if (!core.start(romDir, nvramDir)) return 1;
		core.factoryReset();
		core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);
		core.runForSeconds(9.0);
		stubBusy = runBurst(core, kNotes, "La32Stub");
		core.stop();
	}
	{
		D110CoreNative core;
		if (!core.start(romDir, nvramDir)) return 1;
		core.factoryReset();
		core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Ramps);
		core.runForSeconds(9.0);
		rampsBusy = runBurst(core, kNotes, "La32Ramps");
		core.stop();
	}

	std::printf("\n%s\n", (rampsBusy < stubBusy)
	                          ? "La32Ramps frees slots back up - the fix helps"
	                          : "no improvement - needs more investigation");
	return 0;
}
