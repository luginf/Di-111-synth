// D110Core: runs MAME's real D-110 machine - the actual Roland firmware, its menu
// system, its MSM6222B LCD and its 16 front-panel buttons - inside the plugin, on its
// own thread, with a headless OSD. It produces NO audio: MAME has no LA32 emulation
// for any Roland LA machine, so the sound continues to come from mt32emu. This half
// supplies everything mt32emu lacks, and mt32emu supplies the one thing this half
// lacks. See docs/sysex_address_map.md for how the two are joined.
//
// Architecture mirrors the proven MameWrap in the MU-100R and MU-128 plugins (same
// cli_frontend + custom osd_common_t technique), minus the audio ring and plus a
// window onto the firmware's battery-backed RAM, which is what carries panel edits
// across to mt32emu.
//
// Nothing here needs a patched MAME: the LCD's render(), the "rams" memory share and
// the SC0/SC1 ioports are all reachable through public interfaces.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class D110Core {
public:
	D110Core();
	~D110Core();

	// Boots the machine. `romPath` is a MAME rompath (the d110 romset); `nvramDir` is
	// where the firmware's battery RAM and memory card persist between runs.
	//
	// Returns false, having done nothing, if another machine is already running anywhere
	// in this process. ONLY ONE IS POSSIBLE, and the reason is structural rather than
	// something this code can arrange around: MAME reaches its machine through
	// mame_machine_manager::instance(), which caches a process-wide
	// `static mame_machine_manager *s_manager` and hands the same one to every later
	// caller, so a second cli_frontend attaches to the first machine's OSD and options.
	// Measured, not assumed - plugin/two_instance_test.cpp starts a second core while the
	// first is up and the process dies of heap corruption within seconds. Refusing is
	// therefore the only safe answer until MAME's singleton is dealt with.
	bool start(const std::string &romPath, const std::string &nvramDir);
	void stop();
	bool isRunning() const { return running.load(std::memory_order_acquire); }
	// Whether some other instance in this process already holds the machine.
	static bool machineHeldElsewhere() { return sMachineLive.load(std::memory_order_acquire); }

	// --- LCD -----------------------------------------------------------------
	// The real MSM6222B's rendered dot matrix, straight out of the chip: 2 lines of
	// 16 characters, 8 dot rows per character, and within a row byte bit 4 is the
	// LEFTMOST dot (msm6222b.h's own convention). The glyphs therefore come from the
	// genuine mask CGROM that MAME loads, and the cursor and blink are the
	// controller's own - there is no font table on this path at all.
	static constexpr int kCols = 16;
	static constexpr int kLines = 2;
	static constexpr int kRowsPerChar = 8;
	static constexpr int kLcdBytes = kLines * kCols * kRowsPerChar;
	// Fills `out` (kLcdBytes) with out[(line * kCols + col) * kRowsPerChar + row].
	// Returns false if the machine has not produced a frame yet.
	bool getLcd(uint8_t *out) const;

	// --- front panel ---------------------------------------------------------
	// Two 8-bit scan ports exactly as INPUT_PORTS_START(d110) declares them:
	// port 0 = "SC0" (top row), port 1 = "SC1" (bottom row). Bit masks per button
	// are in docs/panel_reference_notes.md.
	static constexpr int kNumPorts = 2;
	static constexpr int kNumBits = 8;
	static constexpr int kNumButtons = kNumPorts * kNumBits;
	static constexpr int buttonIndex(int port, int bit) { return port * kNumBits + bit; }

	// Buttons are held as a DESIRED STATE, not as a queue of press/release events, and
	// the machine re-applies that state every frame. This is what makes a stuck button
	// impossible: there is no release event that can be dropped or arrive out of order,
	// and re-asserting a state that is already correct costs nothing. (A queue was tried
	// first and could strand a switch closed whenever its release was lost.)
	void setButton(int index, bool down);
	// Opens every switch at once - used when the panel loses track, e.g. on power off.
	void releaseAllButtons() { wantButtons.store(0, std::memory_order_release); }
	uint32_t buttonMask() const { return wantButtons.load(std::memory_order_acquire); }

	// The factory reset the driver's own header documents: hold Write/Copy across a
	// reset, then confirm with Enter. That is what makes the firmware rebuild its patch
	// and timbre memory from the preset ROM - a fresh NVRAM is all zeroes and shows an
	// empty patch until this is done, and it is also the way back from a memory that
	// has been edited into a mess. Restarts the machine, so it returns immediately and
	// finishes on its own; `isRunning()` goes false and true again as it does.
	void factoryReset();
	// Set while a factory reset is in progress, so the UI can say so.
	bool isResetting() const { return resetting.load(std::memory_order_acquire); }

	// --- the firmware's battery-backed RAM ------------------------------------
	// 32 KB at 0x40000 in the D-110's bank map, shared as "rams". This is the whole
	// patch/timbre memory plus the working areas, and its layout is Roland's own
	// SysEx address map with a constant offset per region - RAM 0x2000 is SysEx
	// 0x030000, confirmed both against the manual and by measurement. Mirroring it
	// into mt32emu is what makes a panel edit audible.
	static constexpr int kRamSize = 0x8000;
	// Copies the current RAM into `out` (kRamSize bytes). Returns false before the
	// first snapshot exists.
	bool getRam(uint8_t *out) const;
	// Monotonic counter bumped whenever the snapshot changed, so a caller can skip
	// work when the firmware has been idle.
	uint64_t ramGeneration() const { return ramGen.load(std::memory_order_acquire); }

	// --- the bridge to the LA engine -----------------------------------------
	// Whenever a mirrored region of that RAM changes, the core turns it into a Roland
	// DT1 exclusive message addressed exactly as the hardware would address it, and
	// queues it here. The audio thread drains the queue into mt32emu, which speaks
	// this map natively - so selecting a timbre or retuning a part on the panel
	// reaches the sound engine without anything having to understand the parameters
	// one by one. See docs/sysex_address_map.md.
	static constexpr int kMaxSysexBytes = 256;
	// Copies one pending message into `out` (at least kMaxSysexBytes) and returns its
	// length, or 0 if the queue is empty. Safe to call from the audio thread.
	int popSysex(uint8_t *out);
	// Sends every mirrored region on the next snapshot, whether or not it changed. Used
	// wherever the engine's idea of the state cannot be trusted to match the firmware's:
	// once the machine has finished booting (the engine came up on its own ROM defaults
	// while the firmware came up on the user's saved memory) and after a factory reset
	// (the firmware has just rebuilt that memory from scratch). The firmware is the
	// master here, so it is always the engine that gets brought into line.
	void resyncMirror() { mirrorResync.store(true, std::memory_order_release); }
	// How many mirror messages have been produced since boot - diagnostics, so a
	// "nothing changed" result can be told apart from "the bridge never fired".
	uint64_t sysexEmitted() const { return sysexCount.load(std::memory_order_acquire); }
	// Mirror messages that found no room in the ring. Anything but zero means the engine missed
	// parameter updates, and any reading taken from it is a lower bound.
	uint64_t sysexDropped() const { return sysexDropCount.load(std::memory_order_acquire); }

	// --- Roland's map, as this instrument lays it out -------------------------
	// The exclusive addresses are the owner's manual's own (docs/sysex_address_map.md);
	// the RAM offsets beside them were MEASURED, and between them they account for the
	// whole of the battery RAM below the firmware's working areas:
	//
	//   0x0000  Patch Memory        64 x 128   plugin/factory_bank_probe.cpp
	//   0x2000  Timbre Temporary     9 x 16    plugin/bridge_probe.cpp
	//   0x2090  Rhythm Setup        85 x 4     follows Timbre Temporary
	//   0x21E4  Tone Temporary       8 x 246   plugin/tone_probe.cpp, 246/246 bytes
	//   0x2994  Timbre Memory      128 x 8     plugin/editor_write_probe.cpp
	//   0x2D94  System Area             23     plugin/bridge_probe.cpp
	//
	// Timbre Memory lands exactly where Tone Temporary ends and ends exactly where the
	// System Area begins, and its factory content reads as 64 records naming group A
	// tones 1-64 followed by 64 naming group B - which is what a factory D-110 has.
	static constexpr uint32_t kSysexTimbreTemp = 0x030000;
	static constexpr uint32_t kSysexRhythmTemp = 0x030110;
	static constexpr uint32_t kSysexToneTemp   = 0x040000;
	static constexpr uint32_t kSysexTimbres    = 0x050000;
	static constexpr uint32_t kSysexPatches    = 0x060000;
	static constexpr uint32_t kSysexTones      = 0x080000;
	static constexpr uint32_t kSysexSystem     = 0x100000;
	static constexpr uint32_t kSysexDisplay    = 0x200000;

	static constexpr int kRamPatches    = 0x0000;
	static constexpr int kRamTimbreTemp = 0x2000;
	static constexpr int kRamRhythmTemp = 0x2090;
	static constexpr int kRamToneTemp   = 0x21E4;
	static constexpr int kRamTimbres    = 0x2994;
	static constexpr int kRamSystem     = 0x2D94;
	// Name of the patch the unit is playing right now: the first ten bytes of the working copy,
	// right after the System area. The rest of the patch does not live here but is unpacked
	// into the temporary areas - reverb, reserve and channels in System, part assignments in
	// Timbre Temporary - so only the name is taken from here.
	static constexpr int kRamPatchName  = 0x2DAB;
	// Number of the playing patch, 0..63. Measured by button presses: three Number+ move this
	// byte by exactly three, one Bank+ by exactly eight (plugin/editor_write_probe.cpp). This is
	// what lets the editor go to any patch with the panel's own buttons.
	static constexpr int kRamPatchNumber = 0x2DB9;
	static constexpr int kRamUnitNumber = 0x2DB6; // Exclu Unit# (SysEx device ID), see the native core

	static constexpr int kNumPatches       = 64;
	static constexpr int kPatchRecord      = 128;
	static constexpr int kNumTimbres       = 128;  // internal timbre memory, I-A/I-B
	static constexpr int kTimbreRecord     = 8;
	static constexpr int kNumParts         = 9;    // eight voice parts and rhythm
	static constexpr int kTimbreTempRecord = 16;
	static constexpr int kToneRecord       = 246;
	static constexpr int kNumRhythmKeys    = 85;
	static constexpr int kRhythmRecord     = 4;
	static constexpr int kRhythmFirstKey   = 24;
	static constexpr int kNameChars        = 10;
	// Tone memory: RAM 0x4000 == SysEx 08 00 00, 64 records of 256 bytes - the whole upper half
	// of the battery-backed RAM. The only area of Roland's map that could not be found by
	// contents: in a factory unit it is empty, all 16 KB zeros. Measured by writing
	// (plugin/editor_write_probe.cpp): two names sent to addresses 08 00 00 and 08 04 00 landed
	// at 0x4000 and 0x4200, i.e. a stride of 256. Confirmed a second, independent way: after
	// the write the unit named the group-2 tone with the sent name on ITS OWN display.
	static constexpr int kRamTones     = 0x4000;
	static constexpr int kNumTones     = 64;
	static constexpr int kToneMemRecord = 256;

	// Builds exactly the same "Data set 1" message the mirror builds (see emitRegionSysex), but
	// for an arbitrary address and arbitrary data, and does NOT queue it: the extended editor
	// uses this, and its path leads not to the engine but to the firmware itself - through
	// pushMidi, i.e. through its own MIDI receiver. Returns the message length in `out` (at
	// least kMaxSysexBytes) or 0 if the data does not fit.
	//
	// `offset` is added to the address in Roland's SEVEN-BIT space, not as an ordinary number:
	// each address byte carries seven bits for Roland, and the offset of a 246-byte timbre
	// would otherwise point elsewhere as soon as it crosses 0x80.
	static int buildDt1Message(uint32_t sysexAddress, int offset, const uint8_t *data,
	                           int length, uint8_t *out);

	// --- host MIDI into the firmware ------------------------------------------
	// The firmware has to SEE the notes, not just the sound engine. Its MIDI IN is the
	// CPU's serial port, and the driver already drives it that way for its own test note.
	// Without this the control board never learns a key was pressed, so the top LCD row
	// never lights the playing parts and the display drifts away from the host's own
	// program changes - the panel would be showing a machine that is not being played.
	//
	// Queues bytes to be shifted in at MIDI's own rate. Safe to call from the audio
	// thread; the machine thread consumes them.
	void pushMidi(const uint8_t *bytes, int len);

	// A handful of rhythm timbres (measured: 64, 65, 66 - the very start of preset group B)
	// make the firmware's own note-completion write (ROM 0x278B, f400[ctx]) carry a small
	// number like 1 or 0 instead of the real MIDI key, for reasons that live somewhere
	// earlier than the internal register window this project can currently trace - the real
	// key is nowhere in the CPU's own register file by the time this write happens (measured:
	// regs 0x40-0x50 hold neither the key nor anything resembling it). Rather than forward
	// that number to the sound engine, where RhythmPart::noteOn's own range check (24-108)
	// rejects it and the drum is heard 10-50x quieter than every other one, the bridge
	// remembers the key WE sent in the first place and substitutes it back in.
	//
	// Fed from forwardMidiToFirmware() - but ONLY for the three timbres actually measured
	// broken, checked against the live Rhythm Setup at forward time, not for every rhythm
	// note-on. The queue has no link back to which key a hint was for, so an ordinary,
	// already-correct drum hit sitting in it would eventually get handed to some later,
	// unrelated broken-key completion and print the wrong drum onto that key - measured by
	// ear after the first version of this fix queued everything indiscriminately.
	// Consumed on the machine thread when a rhythm-part triple completes with an
	// out-of-range note. A fixed ring rather than one slot, because a fast run on the same
	// broken key can have more than one hit in flight before the firmware answers either.
	void hintRhythmKey(uint8_t note);

	// --- the missing external interrupt ---------------------------------------
	// When a note reaches it, the firmware enables the CPU's external interrupt and then
	// spins at 0x29E9 waiting for a flag in its own RAM to have bit 7 set:
	//
	//     29E6  orb   int_mask, #80        enable EXTINT
	//     29E9  ldbze 54, f440[52]         read the flag
	//     29EE  jbc   54, 7, 29e9          keep waiting while bit 7 is clear
	//
	// Only an interrupt handler can set that flag, and NOTHING in MAME's roland_d10
	// drives EXTINT - the one interrupt the driver wires up is HSI0 from the key scanner,
	// which the D-110 configuration then removes outright. So the wait never ends and the
	// firmware never returns to scanning the front panel.
	//
	// This supplies the missing edge. The rate is settable because the hardware's real
	// source is unknown - it is somewhere on the sound board, which is exactly the part
	// MAME does not emulate. 0 disables it, restoring the old behaviour.
	// Driving EXTINT turned out to be the wrong lever: in MAME the line also feeds what
	// port 2 reads back (see i8x9x_device::port2_r), so raising it corrupts a port the
	// firmware relies on, and the panel dies even at 62 Hz with no notes at all. Kept
	// because the measurement is worth preserving, but it defaults to off.
	static constexpr int kExtIntTimerHz = 32000;
	// Rising edges arrive at kExtIntTimerHz / (2 * divider). 0 means never.
	void setExtIntDivider(int divider) { extIntDiv.store(divider, std::memory_order_release); }
	int extIntDivider() const { return extIntDiv.load(std::memory_order_acquire); }
	uint64_t extIntEdges() const { return extIntCount.load(std::memory_order_acquire); }

	// --- releasing the stuck wait ---------------------------------------------
	// The loop at 0x29E9 polls a byte in battery RAM (0xF440 goes through fixed_r into the
	// "rams" share at offset 0x3440) and spins until bit 7 of it is set. On the hardware
	// the sound board's interrupt sets that flag; in MAME nothing ever does.
	//
	// So set it - but ONLY while the firmware is demonstrably sitting in that loop, which
	// is checked by looking at the program counter. Outside those two addresses this
	// touches nothing at all, so it cannot disturb a machine that is running normally.
	static constexpr uint16_t kStuckLoopPc = 0x29E9;
	static constexpr uint16_t kStuckLoopPcAlt = 0x29EE;
	// f440[] is one array of per-voice flags; f460[] immediately after it is a DIFFERENT
	// array, walked by the voice-chain loop at 0x268A. Writing across both corrupts the
	// chain and merely moves the hang, which is what a 64-byte span did. 32 bytes covers
	// f440[] alone.
	static constexpr uint16_t kVoiceFlagBase = 0x3440;
	static constexpr int kVoiceFlagSpan = 32;
	// How to answer the firmware when it is caught waiting.
	//   Off       - do nothing; the wait never ends and the panel dies.
	//   PokeRam   - set the flag byte directly. Releases that loop, but bypasses the
	//               interrupt handler, so everything else the handler would have updated
	//               stays stale and the hang simply moves to the voice-chain walk.
	//   PulseExtInt - raise the CPU's external interrupt, which is what the sound board
	//               does on the hardware, and let the firmware's OWN handler run. Fires
	//               only while the firmware is demonstrably in the wait loop: driving that
	//               line continuously kills the panel by itself, because on this CPU the
	//               EXTINT pin is also port 2 bit 2.
	//   La32Stub  - the real answer. Supplies the bookkeeping half of the sound board:
	//               when the firmware is waiting on a voice, raise the interrupt AND hand
	//               its handler the status byte it reads from 0x0C00, encoding the very
	//               voice it is waiting for. The handler then does its own work, which is
	//               what the two cruder policies skipped. See docs/la32_interface.md.
	//   La32Ramps - what this actually is. On the LA32 amplitude and cutoff are hardware
	//               ramps: the CPU stores a target and an increment, the chip walks to the
	//               target on its own and raises INT on arrival, and the firmware loads the
	//               next envelope step from the interrupt. Measured (docs/la32_register_map.md):
	//               over a whole note the firmware gets ZERO answers, so the envelope breaks
	//               off at the first step, and edits to its times and levels never reach the
	//               chip at all. Here the ramps are really computed, and the interrupt
	//               signals their arrival, not that the CPU got stuck somewhere.
	enum class StuckPolicy { Off, PokeRam, PulseExtInt, La32Stub, La32Ramps };
	void setStuckPolicy(StuckPolicy p) { stuckPolicy.store(int(p), std::memory_order_release); }
	StuckPolicy stuckPolicy_() const {
		return StuckPolicy(stuckPolicy.load(std::memory_order_acquire));
	}
	uint64_t stuckReleases() const { return stuckCount.load(std::memory_order_acquire); }
	// IOC1 as it stood the last time the firmware was caught waiting. Bit 1 of it decides
	// whether the CPU accepts the external interrupt at all: i8x9x_device sets the pending
	// flag only `if(!extint && state && !BIT(ioc1, 1))`. If that bit is set, no amount of
	// driving the line can ever be noticed, and EXTINT is a dead end.
	int stuckIoc1Value() const { return stuckIoc1.load(std::memory_order_acquire); }
	void osdRecordIoc1(int v) { stuckIoc1.store(v, std::memory_order_release); }
	// How many times the stub has handed the firmware's handler a status byte.
	uint64_t la32Services() const { return la32Count.load(std::memory_order_acquire); }
	void osdCountLa32Service() { la32Count.fetch_add(1, std::memory_order_relaxed); }
	// Diagnostic (2026-07-31): the last r52 context the slot scan looked for and could NOT
	// find a busy, matching hardware slot for, plus how many consecutive ticks it has been
	// unresolved. Answers "is the firmware waiting for a voice that was never (or no
	// longer) marked busy" directly, instead of inferring it from a trace.
	void osdRecordUnresolvedContext(uint8_t context) {
		if (context == lastUnresolvedContext.load(std::memory_order_relaxed))
			unresolvedStreak.fetch_add(1, std::memory_order_relaxed);
		else {
			lastUnresolvedContext.store(context, std::memory_order_relaxed);
			unresolvedStreak.store(1, std::memory_order_relaxed);
		}
	}
	uint8_t lastUnresolvedContext_() const {
		return uint8_t(lastUnresolvedContext.load(std::memory_order_acquire));
	}
	uint64_t unresolvedStreak_() const { return unresolvedStreak.load(std::memory_order_acquire); }
	// Every read of the status register that reached the tap, whether or not there was
	// anything to hand over. Distinguishes "the tap is not installed" from "the firmware
	// never got as far as reading".
	uint64_t la32Reads() const { return la32ReadCount.load(std::memory_order_acquire); }
	void osdCountLa32Read() { la32ReadCount.fetch_add(1, std::memory_order_relaxed); }
	// The CPU register holding the voice index the wait loop is indexing with: `ldbze 54,
	// f440[52]` uses word register 52, and the register file share starts at 0x18.
	static constexpr int kWaitIndexReg = 0x52;
	static constexpr int kRegFileBase = 0x18;

	// 2026-07-31: r52 turned out to be the WRONG source for "which voice". It is the
	// mainline's own logical wait-context index, used only to address f440[] - not the
	// hardware voice slot the sound board would report. Traced from the dispatch routine
	// at ROM 0x3615 (`stb 50, ee00[54]` / `stb 52, ee01[54]` / ... / `stb #20, edc0[54]`):
	// register 54 (word, already slot*2) IS the hardware voice slot, and dispatch records
	// the logical context (r52's value at dispatch time) into ee01[slot] for the handler to
	// cross-reference later - so the handler never needs us to know r52 at all, only the
	// TRUE slot number. rams 0x2DC0 + 2*slot ("edc0[slot]" at CPU address 0xEDC0, which is
	// rams offset - 0xC000) marks a dispatched voice awaiting completion.
	//
	// 2026-08-03: the "reverts to 0x80 once serviced" this comment used to claim is real
	// CODE - confirmed by disassembly at ROM 0x32AA/0x34FA (`stb #0x80, edc0[64]`), reached
	// only once the voice's envelope-stage counter (eec0[voice]) counts up to 7 - but it is
	// NOT something that currently happens. `plugin/slot_life_probe.cpp` taps every write
	// into this table by the PC that made it: across six notes the counter reaches 6 twelve
	// times and never once reaches 7, so the release code is never entered and slots stay
	// busy forever - measured as 31 of 32 slots stuck busy at idle. See
	// docs/la32_interface.md, "solved kept the panel alive, but not the polyphony".
	//
	// The busy value is 0x40, NOT the 0x20 the disassembly listing at 0x3646 seemed to say -
	// `d110_la32_lifecycle_probe` played one real note and read the table back afterwards:
	// exactly two slots (0 and 1 - one D-110 note uses two LA32 partials) flipped 80 -> 40
	// and stayed there. Something between 0x364E and the unread rest of that routine must
	// overwrite the 0x20 the listing shows with 0x40 before the CPU is done dispatching;
	// trust the measurement over the partial disassembly.
	static constexpr uint16_t kSlotStateTable = 0x2DC0;
	static constexpr int kNumHardwareVoices = 32;
	static constexpr uint8_t kSlotBusyValue = 0x40;
	// 2026-07-31, demo song: most SUSTAINING voices sit at 0x20, not 0x40 - only a voice
	// genuinely mid-handshake shows 0x40. A wait reached via the reset loop at ROM 0x29BB
	// (walks ee40[], zeroes 0x0C00/0x0C80/eec0/ef00 for each slot, but never touches edc0)
	// left its slot at the stale 0x20 from before, and the scan found nothing until this
	// was accepted too - measured with `d110_demo_song_repro`'s unresolved-context dump.
	static constexpr uint8_t kSlotBusyValueAlt = 0x20;
	static constexpr uint8_t kSlotIdleValue = 0x80;

	// Scanning for ANY busy slot is not enough once more than one note is pending at
	// once (any chord): the wrong slot can be answered while the loop the CPU is
	// ACTUALLY parked in (a specific r52 context) never gets its flag set, so the panel
	// stays dead even though the handler is genuinely running and genuinely consuming
	// status bytes - measured 2026-07-31, `d110_hang_probe`'s policy comparison. Dispatch
	// (`stb 52, ee01[54]`, ROM 0x361A) records r52's LOW BYTE into ee01[slot], so the
	// reverse lookup is exact: given the r52 the wait loop is polling right now, the slot
	// to report is whichever n has ee01[n] == (r52 & 0xff). ee01 sits one byte past
	// ee00/kSlotWordTable, i.e. rams 0x2E01 + 2*n (CPU 0xEE01, minus the same 0xC000 the
	// rest of this window uses).
	static constexpr uint16_t kSlotContextTable = 0x2E01;

	// How to encode the voice number in the status byte. The handler treats bit 7 as an
	// event class and derives the index differently on each path - with bit 7 clear it
	// doubles the low bits and subtracts two, with bit 7 set and bit 5 clear it doubles
	// them and does not - so the meaning of a given byte is not obvious from the code
	// alone. Rather than guess, the small space is enumerated and measured.
	//   0  (v+1) & 0x1F        bit 7 clear, index comes out as v
	//   1  v & 0x1F            bit 7 clear, index comes out as v-1
	//   2  0x80 | (v & 0x1F)   bit 7 set, bit 5 clear, index v
	//   3  0x80 | ((v+1)&0x1F) bit 7 set, bit 5 clear, index v+1
	// How long the stub makes the firmware wait before answering, in midiTick periods
	// (1/3125 s each, so 1 tick = 0.32 ms). The real LA32 took real time to accept a voice;
	// the stub answers as fast as it can notice, which is far quicker. If the firmware's
	// sequencer paces itself off those waits at all, the demo songs run too fast - which is
	// exactly what they do. This knob exists to settle that by experiment rather than
	// argument: sweep it and watch the note-on rate.
	void setLa32ResponseDelay(int ticks) { la32Delay.store(ticks, std::memory_order_release); }
	int la32ResponseDelay() const { return la32Delay.load(std::memory_order_acquire); }

	// --- LA32 hardware ramps -----------------------------------------------------
	// Two register banks, two bytes per slot: the even byte is the INCREMENT, the odd one the
	// TARGET. That order is measured, not chosen: on key-down the amplitude bank receives target
	// 0x7E at velocity 100 and 0x6E at velocity 40 (so the target is the level), and on key-up
	// target 0x00 with increment 0xCF, whose top bit means "downward". The cutoff bank behaves
	// exactly the same, which is an argument in itself.
	//
	// The meaning of target and increment is taken from munt's model of the same chip (LA32Ramp),
	// where it was reconstructed by analysing recordings from a live unit: the increment's top
	// bit is the direction, the low seven the speed on an exponential scale, a zero increment
	// moves nothing and raises no interrupt.
	static constexpr uint16_t kAmpRampBase = 0x0C80;   // TVA ramp
	static constexpr uint16_t kFilterRampBase = 0x0C00; // TVF ramp - see the caveat in the document
	// The rate at which the chip computes ramps. Also the rate of its digital audio output.
	static constexpr double kLa32SampleRate = 32000.0;
	// How many ramps started and how many reached their target - counters, not a log: "does the
	// mechanism work at all" cannot be answered by a truncated record.
	uint64_t la32RampStarts() const { return rampStarts.load(std::memory_order_acquire); }
	uint64_t la32RampLandings() const { return rampLandings.load(std::memory_order_acquire); }
	// Whether to treat increment 0xFF as "set without interrupt". Kept as a switch because it
	// is a HYPOTHESIS, and the rig must be able to show both sides.
	void setLa32PresetFf(bool on) { presetFf.store(on, std::memory_order_release); }
	bool la32PresetFf() const { return presetFf.load(std::memory_order_acquire); }
	void osdCountRampStart() { rampStarts.fetch_add(1, std::memory_order_relaxed); }
	void osdCountRampLanding() { rampLandings.fetch_add(1, std::memory_order_relaxed); }

	void setLa32StatusMode(int mode) { la32Mode.store(mode, std::memory_order_release); }
	int la32StatusMode() const { return la32Mode.load(std::memory_order_acquire); }
	static uint8_t encodeLa32Status(int mode, uint16_t voice);

	// --- M-256D memory card -------------------------------------------------------
	// The hardware signals the card's presence in NO way: the firmware has neither a line nor a
	// "card inserted" bit. It recognises a card by WRITING to it and reading back.
	// The detection routine, ROM 0x770A (disassembled, see docs/memory_card.md):
	//
	//   770A  lcall 7d35             bank 0x30 - the card's first 16 KB in the 0x8000-0xBFFF window
	//   770D  ld 78, #7804           twelve-byte signature in ROM
	//   7718  ldb 70, [76]           a card byte
	//   771B  cmpb 70, [78]          compare with the signature; mismatch - go to 7746
	//   7746  ldb 75, 70 / negb 75   complement of the byte read
	//   774B  stb 75, [76]           write it INTO THE CARD
	//   774E  cmpb 70, [76]          read the same place back
	//   7751  jne 7759               changed - the write took, a card IS present (code 'C')
	//   7753  cmpb 70, #ff           unchanged. Was the original byte 0xFF?
	//   7756  jne 7759               no - treat it as a card anyway
	//   7758  ret                    0xFF and not writable - NO CARD, code 0xFF
	//
	// Code 0xFF is handled at 0x76E2 and prints "Card Not Ready". So to the firmware an empty
	// socket is a bus that reads as 0xFF and refuses writes. That is exactly what is portrayed
	// here: the card's contents are carried off into their own buffer, and the shared memory
	// "memcs" is flooded with 0xFF. No MAME patch needed, the driver needs to know nothing.
	//
	// The firmware reads write protection separately and in another bank: 0x7770 sets bank 0x31
	// and reads 0xBFFF, i.e. offset 0x7FFF from the start of the card; a zero in bit 0 yields
	// "Memory Card Write Protected".
	//
	// THIS BYTE IS NOT CARD MEMORY but the status port of the IC21 gate array (D65005G-062).
	// On the main board schematic (service notes p. 7) pin 34 of connector CN8 - `CST` - goes
	// to the array's `SENS` input, pulled up through R11 100K, and pin 33 `VBB` through
	// comparator 22a to the `BATT` input. These are the card's only two lines besides the bus,
	// and the CPU has nowhere else to read them from. That address 0x7FFF reports rather than
	// stores also shows in behaviour: formatting floods the whole card, writing 0x00 there, and
	// if this were memory a freshly formatted card would immediately turn out write-protected -
	// which is what happened until the byte became a port (measured by the d110_card
	// roundtrip). The MAME driver gives the whole 0xC0000-0xC7FFF range to RAM and does not
	// model the array at all, so the port is recreated here by intercepting the read.
	static constexpr int kCardSize = 0x8000;
	static constexpr int kCardStatusOffset = 0x7fff;
	// What an empty socket reads as. This is precisely the value the firmware checks at 0x7753.
	static constexpr uint8_t kCardAbsentByte = 0xff;
	void setCardInserted(bool in) { cardWant.store(in, std::memory_order_release); }
	bool cardInserted() const { return cardWant.load(std::memory_order_acquire); }
	// The write-protect switch on the card itself. Read as bit 0 of the status port: zero -
	// protected. The firmware never looks at the port's other bits (exactly one read of this
	// address in the whole ROM, 0x7778), so they are returned as ones - the same as an
	// unconnected pulled-up line would give.
	void setCardWriteProtect(bool on) { cardProtect.store(on, std::memory_order_release); }
	bool cardWriteProtect() const { return cardProtect.load(std::memory_order_acquire); }
	uint8_t cardStatusByte() const { return cardWriteProtect() ? 0xfe : 0xff; }
	// Replace the card's contents wholesale (kCardSize bytes). The rig fills an empty socket, a
	// blank card and a write-protected card with it, so that the firmware itself, not a reading
	// of its code, speaks for each state.
	void setCardImage(const uint8_t *bytes);
	// The card's current contents. While it is inserted the truth lives in the machine's shared
	// memory, so the snapshot is taken from there, not from the plugin's buffer.
	bool getCardImage(uint8_t *out) const;
	void osdApplyCard(uint8_t *shared);
	void osdDetachCard(uint8_t *shared);

	// --- diagnostics: what does the firmware talk to that is not there? --------
	// The D-110's address map leaves nearly all of the low I/O page unmapped - only the
	// bank register, the SO register, the two panel scan ports and the LCD are claimed.
	// The sound board lives in the gaps. Turning on unmapped-access logging and capturing
	// MAME's own log lines shows exactly which addresses the firmware writes and reads
	// when it plays a note, which IS the LA32's register interface as the firmware sees it.
	void setLogUnmapped(bool on) { logUnmapped.store(on, std::memory_order_release); }
	bool logUnmappedEnabled() const { return logUnmapped.load(std::memory_order_acquire); }
	void osdLogLine(const char *line);
	// Takes everything captured so far and empties the buffer.
	std::vector<std::string> takeLogLines();

	// --- MIDI MESSAGE lamp ----------------------------------------------------------
	// Lit while bytes arrive on MIDI IN, going out about kMidiLampHoldMs after the last one. It
	// is a monostable on the output of the MIDI IN optocoupler - a circuit typical of Roland in
	// those years - not a register the firmware drives.
	//
	// The firmware does not drive it, and that is measured, not deduced from general principles
	// (plugin/so_trace_probe.cpp, target d110_so_trace). Over a whole session the firmware
	// writes the external latches five times, all at boot - to 0x0200 from ROM 0x2106 and
	// 0x2D28, to 0x0280 from 0x1C94, 0x1CC1 and 0x20FF - with bit 0 clear in every one. It
	// NEVER writes the CPU's own port 1 and 2 outputs (register file 0x0F and 0x10). On MIDI
	// arrival nothing is written anywhere. So the firmware has nothing to light the lamp with.
	//
	// Bit 0 of the SO latch used to be read here - that is what the comment in so_w() in
	// roland_d10.cpp calls it. That bit is always zero, so the lamp never lit, while the panel
	// took the reading as authoritative and confidently drew it dark.
	static constexpr int64_t kMidiLampHoldMs = 90;
	static int64_t nowMs() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
		           std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	bool midiLampOn() const {
		const int64_t last = lastMidiByteMs.load(std::memory_order_acquire);
		return last != 0 && nowMs() - last < kMidiLampHoldMs;
	}
	// Left for the panel: this used to mean "the firmware has written the register at least
	// once". Now the source is always known, and the panel's fallback path is not needed.
	bool midiLampValid() const { return true; }
	static constexpr uint16_t kSoRegister = 0x0200;
	// The same address up to A7. MAME's memory map describes the SO latch at exactly 0x0200, and
	// writes to 0x0280 do not reach it - they show only in the unmapped-access log. If the
	// decoder on the board is coarser (the address is adjacent and nothing else occupies it),
	// this is one and the same latch, and half of its writes are currently lost.
	static constexpr uint16_t kSoRegisterAlias = 0x0280;

	// External I/O absent from MAME's memory map altogether: 0x0400-0x0BFF.
	//
	// Per the main board schematic (docs/service_notes_findings.md) the reverb chip IC5 has its
	// own input from the CPU - five data bits D1-D5 and two strobes STB0/STB1 - and those
	// strobes are built from the IC16 gate array outputs: STB0 = NOT(EXIO1 . WL), STB1 =
	// NOT(EXIO2 . WL). The addresses at which the array raises EXIO1 and EXIO2 are internal to
	// it and unnamed on the schematic, but the D-110 map in MAME occupies only 0x0100, 0x0200,
	// 0x021A-0x021D, 0x0300 and 0x0380, and the unmapped-access log showed exactly two
	// unexplained addresses - 0x0400 and 0x0800. The tap takes the whole span so the answer
	// does not depend on whether the pair was guessed right.
	//
	// The upper bound 0x0BFF deliberately stops short of 0x0C00: that is the LA32's own register
	// file (512 addresses, exactly its pins A0-A8), a stream goes through it on every note, and
	// it would drown the capture. That is a DIFFERENT interface, not the reverb's.
	static constexpr uint16_t kExtIoTapBase = 0x0400;
	static constexpr uint16_t kExtIoTapEnd = 0x0BFF;

	// The firmware not only READS 0x021A-0x021D (panel scan) but WRITES them too, and MAME's map
	// declares them `portr("SC0").nopw()` - writes are silently discarded. Yet that is exactly
	// where bits 1-3 of the reverb type go: ROM routine 0x4C93 reads the type from RAM 0x2D95,
	// takes `type & 0x0E`, merges it into a shadow byte and writes it to 0x021A, and puts the
	// remaining low bit of the type into bit 2 of port 0x0800. The tap is needed for this to be
	// visible at all, and it also shows what the driver loses.
	static constexpr uint16_t kPanelPortTapBase = 0x021A;
	static constexpr uint16_t kPanelPortTapEnd = 0x021D;

	// The LA32's own register file: 0x0C00-0x0DFF. That it is this and not "some chunk of
	// addresses" follows from the MB87136APF pinout table in the service notes - the chip has
	// nine address lines A0-A8, i.e. exactly 512 registers, and the window of unmapped accesses
	// the probe found is exactly that size.
	//
	// The tap is always installed but only writes while capture is on, and the note stream
	// through it is dense - hence capture has an address filter (setTraceFilter): without it the
	// panel scan, thousands of entries a second at 0x021A, would fill the ring before a single
	// note got in.
	static constexpr uint16_t kLa32TapBase = 0x0C00;
	static constexpr uint16_t kLa32TapEnd = 0x0DFF;

	// Limit write capture to one address range. By default everything is taken.
	void setTraceFilter(uint16_t lo, uint16_t hi) {
		traceLo.store(lo, std::memory_order_release);
		traceHi.store(hi, std::memory_order_release);
	}

	// The whole SO byte, not just the lamp bit. Per MAME's analysis (`so_w` in roland_d10.cpp):
	// bit 0 - LED, bits 1-2 - reverb program number (that is A13/A14 of the BOSS chip's ROM, so
	// four programs in all), bit 3 - "R. SW." to the analogue board, bit 5 - BOSS clocking. The
	// panel meanwhile offers eight reverb types plus OFF, so two bits are not enough for them,
	// and the path of the remaining parameters has to be found. A histogram over all 256 values
	// can lose nothing.
	void osdCountSoWrite(uint8_t v) {
		soByteHist[v].fetch_add(1, std::memory_order_relaxed);
		soLast.store(int(v), std::memory_order_release);
	}
	uint64_t soWrites(uint8_t v) const { return soByteHist[v].load(std::memory_order_acquire); }
	int soLastValue() const { return soLast.load(std::memory_order_acquire); }

	// Every write to SO together with the address it was made from. A per-value counter is not
	// enough: it says WHAT was written but not which routine did it, and the address is exactly
	// what opens the firmware to the disassembler (plugin/disasm_tool.cpp). The capture can
	// overflow, so it has its own loss counter, and whoever reads the entries must print it.
	struct SoWrite { double ms; uint16_t pc; uint16_t addr; uint8_t value; };
	void osdLogSoWrite(uint16_t pc, uint16_t addr, uint8_t value);
	std::vector<SoWrite> takeSoWrites();
	uint64_t soWritesDropped() const {
		std::lock_guard<std::mutex> lock(soMutex);
		return soDropped;
	}
	void startSoTrace() {
		{
			std::lock_guard<std::mutex> lock(soMutex);
			soTrace.clear();
			soDropped = 0;
			soTraceStart = std::chrono::steady_clock::now();
			soTracing = true;
		}
		// Set last and cleared first: while it is false, osdLogSoWrite does not take the mutex at
		// all - see there for why that path must never block.
		soTracingOn.store(true, std::memory_order_release);
	}
	void stopSoTrace() { soTracingOn.store(false, std::memory_order_release); }

	// --- notes recovered from the firmware itself ------------------------------
	// The firmware plays its own demo songs (ROM Play) internally and does NOT transmit
	// them - measured: its serial TX emitted one 0x00 byte in 40 seconds. So the demo song
	// moved the part indicators and produced silence, because mt32emu never heard a note.
	//
	// It does, however, write every note it starts into its own voice-context arrays, and
	// those are readable. Confirmed by controlled experiment (plugin/note_source_probe.cpp):
	// playing note 60 velocity 100 on channel 2 produced f400[ctx]=0x3C, f420[ctx]=0x64,
	// f3a0[ctx]=0x00; note 36 velocity 40 on channel 5 produced 0x24, 0x28, 0x30. So:
	//
	//   f400[ctx]  (rams 0x3400 + ctx)  MIDI note number; bit 7 set later means
	//                                   "key released but held by the sustain pedal"
	//   f420[ctx]  (rams 0x3420 + ctx)  velocity
	//   f3a0[ctx]  (rams 0x33A0 + ctx)  part * 16 - so 0x00 = part 1, 0x30 = part 4,
	//                                   0x80 = the rhythm part (index 8)
	//
	// Written consecutively by one routine (ROM 0x278B/0x2790/0x2795), so the third write
	// completes the triple. The release side is ROM 0x2496's note-off search, which walks
	// the part's voice chain for a context whose f400 matches the note being released and
	// ends at the reclaim path that sets f460[ctx] bit 6 - that bit is the note-off signal,
	// whichever path sets it (ordinary release, voice stealing or all-notes-off).
	//
	// Events are queued here by the machine thread and drained by the audio thread, which
	// turns them into mt32emu playMsgOnPart() calls. That makes the firmware the single
	// source of truth for what is sounding, exactly as the hardware is.
	struct NoteEvent {
		uint8_t part;     // 0-7 voice parts, 8 rhythm - matches mt32emu's part numbering
		uint8_t note;
		uint8_t velocity; // 0 on a note-off
		bool on;
	};
	static constexpr uint16_t kNoteTable = 0x3400;      // f400[]
	static constexpr uint16_t kVelocityTable = 0x3420;  // f420[]
	static constexpr uint16_t kPartTable = 0x33A0;      // f3a0[]
	static constexpr uint16_t kReleaseTable = 0x3460;   // f460[], bit 6 = reclaimed
	static constexpr uint8_t kReleasedBit = 0x40;
	static constexpr int kNumVoiceContexts = 32;
	// Copies one pending event into `out` and returns true, or false when the queue is
	// empty. Safe to call from the audio thread.
	bool popNoteEvent(NoteEvent &out);
	void osdPushNoteEvent(const NoteEvent &ev);
	// See hintRhythmKey() above. Pops the oldest remembered key, or returns false if none is
	// waiting - which happens if a rhythm note WASN'T forwarded by us (e.g. it came from the
	// panel itself rather than MIDI), and the firmware's own value has to stand uncorrected.
	bool osdTakeRhythmKeyHint(uint8_t &outNote);
	// Every write to a part byte in f3a0[], counted by the part it names, BEFORE any of the
	// bridge's own cut-offs. See partByteWrites().
	void osdCountPartByte(uint8_t rawValue) {
		partByteHist[rawValue >> 4].fetch_add(1, std::memory_order_relaxed);
	}
	// Diagnostics: deliver only this part's notes to the engine, so one part of a piece can
	// be rendered and measured on its own. -1 delivers everything, which is the only value
	// the plugin ever uses. Measuring a part inside a full mix cannot distinguish "this part
	// is silent" from "this part is buried", and that distinction is the whole question.
	void setSoloPart(int part) { soloPart.store(part, std::memory_order_release); }
	int soloPart_() const { return soloPart.load(std::memory_order_acquire); }
	// How many note-ons the firmware has handed over - diagnostics, so "the demo song is
	// silent" can be told apart from "the demo song is not playing".
	uint64_t firmwareNoteOns() const { return noteOnCount.load(std::memory_order_acquire); }
	uint64_t firmwareNoteOffs() const { return noteOffCount.load(std::memory_order_acquire); }
	// MIDI Panic support - see D110CoreNative::releaseStuckNoteContexts()/resetVoiceSlotTable()'s
	// own comments for the full story (a stuck voice's release write from the firmware's real
	// envelope-stage pacing can simply never come). NOT implemented on this backend: the
	// per-context m_ctxSounding/m_ctxPart/m_ctxNote state releaseStuckNoteContexts() needs lives
	// inside D110Osd (D110Core.cpp), a class this header has no access to, and `ram` here is only
	// a periodically-refreshed READ snapshot for getRam() (see D110Core::getRam()) -
	// resetVoiceSlotTable() writing into it wouldn't reach the actual running MAME machine, only
	// get overwritten by the next refresh. This backend is dormant and not being actively
	// extended (see CLAUDE.md) - no-ops rather than a half-verified poke into MAME's live memory
	// this sandbox has no MAME tree to build or test against.
	void releaseStuckNoteContexts() {}
	void resetVoiceSlotTable() {}
	// Mean time a note was held, in milliseconds - the measurement that tells "the song is
	// genuinely dense" apart from "every note is being cut to a click", which sounds like a
	// runaway tempo even when the note rate is right.
	double meanNoteMs() const {
		const uint64_t n = noteOffCount.load(std::memory_order_acquire);
		return n ? double(noteMsTotal.load(std::memory_order_acquire)) / double(n) : 0.0;
	}
	// Timestamped log of the first events, purely diagnostic: the shape of the stream is
	// what distinguishes "one musical note is firing several times, once per partial" from
	// "the sequencer itself is running fast". Both inflate the note rate; only the first
	// puts several identical notes inside the same millisecond.
	struct NoteLog { double ms; uint8_t part, note, velocity; bool on; };
	// A capture that hits its ceiling must SAY SO. Three separate wrong conclusions in one
	// session came from buffers that filled and then quietly stopped recording, so the
	// numbers looked complete and were not: a part that entered late read as a part that
	// never played, and a tally of 192 writes sat next to 1305 notes built from them
	// without either figure looking suspicious on its own. Every taker below returns its
	// drop count, and every caller is expected to print it.
	void osdLogNote(const NoteLog &e) {
		std::lock_guard<std::mutex> lock(noteLogMutex);
		if (noteLog.size() < 200000) noteLog.push_back(e);
		else ++noteLogDropped;
	}
	uint64_t noteLogDropped_() const {
		std::lock_guard<std::mutex> lock(noteLogMutex);
		return noteLogDropped;
	}
	uint64_t ctxDropped_() const {
		std::lock_guard<std::mutex> lock(ctxMutex);
		return ctxDropped;
	}
	std::vector<NoteLog> takeNoteLog() {
		std::lock_guard<std::mutex> lock(noteLogMutex);
		std::vector<NoteLog> out;
		out.swap(noteLog);
		return out;
	}
	void startNoteLog() {
		std::lock_guard<std::mutex> lock(noteLogMutex);
		noteLog.clear();
		noteLogDropped = 0;
		noteLogStart = std::chrono::steady_clock::now();
		noteLogging = true;
	}
	double noteLogElapsedMs() const {
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - noteLogStart).count();
	}
	bool noteLoggingOn() const { return noteLogging; }

	void osdAddNoteDuration(double ms) {
		noteMsTotal.fetch_add(uint64_t(ms), std::memory_order_relaxed);
		noteOffCount.fetch_add(1, std::memory_order_relaxed);
	}

	// --- diagnostics: the voice-context bookkeeping arrays, live ---------------
	// f3c0/f400/f440/f460/f480 (CPU 0xF3C0-0xF4FF, rams 0x33C0-0x34FF - the same
	// "addr - 0xC000" mapping as the sound-board window) are where the second stall
	// (docs/la32_interface.md, "a SECOND stall behind the first") lives: the
	// voice-reclaim scan at 0x2673 waits on f460[] bit 6, which nothing in the LA32
	// completion handler ever sets. This is a WRITE tap - unlike setLogUnmapped, it
	// works on a range the address map already claims - so it can say whether the
	// firmware's own housekeeping ever reaches these arrays at all while La32Stub is
	// answering the first wait, instead of guessing further from a static disassembly.
	// CPU-visible address, NOT the rams array index - fixed_r/fixed_w route CPU
	// 0xC000-0xFFFF to rams[addr-0xC000], and a write tap on the program space has to be
	// installed at the address the CPU actually issues (0xF3C0), not the rams offset
	// (0x33C0) - a first attempt used the rams offset directly and silently tapped ROM
	// space instead, which of course the CPU never writes to (0 events, every time).
	static constexpr uint16_t kVoiceCtxTapBase = 0xF3A0;
	static constexpr uint16_t kVoiceCtxTapSpan = 0x160; // covers f3a0..f4ff
	// Second window, same tap machinery, same osdLogCtxEvent stream (both taps run on the
	// single CPU thread, so appends land in true chronological order without needing a
	// separate sequence number): the DISPATCH tables (edc0/ee00/ee01/eec0/ef80, CPU
	// 0xEDC0-0xEFFF, rams 0x2DC0-0x2FFF - "addr - 0xC000" again). Watching dispatch and
	// completion in one merged, time-ordered log is what answering "why does only some
	// fraction of dispatched voices ever complete" needs.
	static constexpr uint16_t kDispatchTapBase = 0xEDC0;
	static constexpr uint16_t kDispatchTapSpan = 0x240; // covers edc0..efff
	// Turning the tap ON starts a fresh capture window, drop count included, so a reading
	// always describes one window rather than everything since the machine booted.
	void setVoiceCtxTap(bool on) {
		if (on) {
			std::lock_guard<std::mutex> lock(ctxMutex);
			ctxEvents.clear();
			ctxDropped = 0;
		}
		voiceCtxTap.store(on, std::memory_order_release);
	}
	bool voiceCtxTapEnabled() const { return voiceCtxTap.load(std::memory_order_acquire); }
	struct CtxEvent { uint16_t pc; uint16_t addr; uint8_t value; };
	void osdLogCtxEvent(uint16_t pc, uint16_t addr, uint8_t value);
	std::vector<CtxEvent> takeCtxEvents();

	// One-off diagnostic (2026-07-31): is port2 bit 2 genuinely low when the handler bails
	// at 0x313D, or is something else going on? Logged only for PC in [0x3138,0x3140],
	// with our own bookkeeping (m_stuckIntHigh) alongside the CPU's own port2_r() so the
	// two can be compared directly instead of reasoning about MAME's i8x9x source further.
	struct Port2Sample { uint16_t pc; uint8_t port2; bool stuckIntHigh; bool la32Pending; };
	void osdLogPort2Sample(uint16_t pc, uint8_t port2, bool stuckIntHigh, bool la32Pending);
	std::vector<Port2Sample> takePort2Samples();

	// --- diagnostics: where is the CPU actually spending its time? -------------
	// Samples the program counter densely while the machine runs. When the firmware
	// stops servicing the front panel the PC collapses onto a handful of addresses, and
	// those addresses say exactly which loop it is stuck in - which is the only way to
	// find out what it is waiting for from the LA32 that MAME does not emulate.
	void setPcSampling(bool on) { pcSampling.store(on, std::memory_order_release); }
	void resetPcHistogram();
	// Fills `out` with the `count` most-sampled addresses, most frequent first.
	struct PcHit { uint16_t pc; uint64_t hits; };
	std::vector<PcHit> topPcs(int count) const;
	uint64_t pcSampleTotal() const { return pcTotal.load(std::memory_order_acquire); }
	// Samples that landed anywhere in [lo, hi]. Used to answer a plain question: is the
	// interrupt handler being entered at all?
	uint64_t pcHitsInRange(uint16_t lo, uint16_t hi) const;
	void osdSamplePc(uint16_t pc);
	void osdCountExtInt() { extIntCount.fetch_add(1, std::memory_order_relaxed); }
	void osdCountStuckRelease() { stuckCount.fetch_add(1, std::memory_order_relaxed); }

	// --- called by the internal OSD only --------------------------------------
	void osdSnapshotLcd(const uint8_t *renderBuf);
	void osdSnapshotRam(const uint8_t *ram);
	// Takes the next byte for the CPU's serial receiver, or returns false if none is
	// waiting. Exactly one byte per call by design: the emulated UART has no FIFO, so a
	// second write before the firmware's interrupt handler has read the first simply
	// overwrites it. The caller is a MAME timer running at the MIDI byte rate, which is
	// what puts real emulated time between one byte and the next.
	bool popMidiByte(uint8_t &out);
	// MIDI's own byte rate: 31250 baud, one start bit and one stop bit around each byte.
	static constexpr double kMidiBytesPerSecond = 31250.0 / 10.0;
	bool shouldStop() const { return stopFlag.load(std::memory_order_acquire); }
	// Diagnostics: bytes accepted, bytes actually shifted into the CPU, and bytes dropped
	// because the queue was full.
	uint64_t midiForwarded() const { return midiInCount.load(std::memory_order_acquire); }
	uint64_t midiDelivered() const { return midiOutCount.load(std::memory_order_acquire); }
	uint64_t midiDropped() const { return midiDropCount.load(std::memory_order_acquire); }
	// Not tracked on this (dormant, MAME-backed) core - see D110CoreType::serialOverrunCount()
	// in D110CoreNative.h for what this actually measures on the shipping native core.
	uint32_t serialOverrunCount() const { return 0; }

	// One block of the firmware's RAM that is mirrored into the LA engine, and where
	// Roland's own exclusive map says it lives.
	struct MirrorRegion {
		uint16_t ramOffset;
		uint32_t sysexAddress; // 21-bit, seven bits per transmitted byte
		uint16_t length;
		const char *name;
		// Resend this region whenever the Timbre Temporary region goes out, even if it did not
		// change in the firmware itself.
		//
		// A write to Timbre Temporary makes the LA engine reload the part's timbre from one of its
		// four banks (Synth::writeMemoryRegion -> Part::setTimbre) and clobbers the Tone Temporary
		// the firmware provided. Right for the MT-32, wrong for the D-110: the firmware holds
		// timbre groups the engine's four banks cannot name. In the demo song parts 6 and 7 carry
		// group 5, and the engine's own maximum table pins it to 3 - and instead of a lead synth a
		// closed hi-hat sounded, 21 dB quieter. The truth about what a part plays is the firmware's
		// Tone Temporary, so it is reasserted after the write that would otherwise cancel it. The
		// order is set by array position, and all timbres come after Timbre Temporary, so no
		// separate synchronisation is needed.
		bool reassertAfterTimbreTemp = false;
	};
	static const MirrorRegion kMirrorRegions[];
	static const int kNumMirrorRegions;
	// For test rigs only: disables the timbre reassertion described at
	// MirrorRegion::reassertAfterTimbreTemp. Needed so that the CONTROL and the measurement run
	// in one pass: with reassertion off a timbre edit must be clobbered, with it on it must
	// survive. A check that can only show "survived" proves nothing: it looks the same whether
	// the fix works or there was simply nothing to clobber. In the plugin itself this is always
	// on, and there is no switch in the UI.
	void setToneReassert(bool on) { toneReassert.store(on, std::memory_order_release); }
	bool toneReassertEnabled() const { return toneReassert.load(std::memory_order_acquire); }
	// Taken with headroom so the counter array below need not be touched when a region is
	// added; a static_assert in the .cpp keeps them consistent.
	static constexpr int kMaxMirrorRegions = 32;

	// --- lossless counters --------------------------------------------------------
	// Count, do not log. Both event logs in this class can fill up and quietly stop writing
	// (that is exactly how a part that joined later once read as a part that never plays at
	// all), so questions that cannot be answered by a truncated record are answered by
	// fixed-size counters - they can lose nothing.
	//
	// noteOnPart   - note-ons completed by the firmware, per part.
	// partByteHist - every write to f3a0[] by its raw value >> 4, INCLUDING values the note
	//                bridge rejects. "The firmware never assigns this part" and "the bridge
	//                drops its notes" are indistinguishable by a note count alone; these two
	//                rows side by side tell them apart.
	// regionEmits  - how many DT1 messages each mirrored region sent. A region that fired alone,
	//                without the region that repairs its side effect, is a specific, checkable
	//                kind of failure.
	uint64_t noteOnsForPart(int part) const {
		return (part >= 0 && part < 9) ? noteOnPart[part].load(std::memory_order_acquire) : 0;
	}
	uint64_t partByteWrites(int part) const {
		return (part >= 0 && part < 16) ? partByteHist[part].load(std::memory_order_acquire) : 0;
	}
	uint64_t regionEmitCount(int region) const {
		return (region >= 0 && region < kMaxMirrorRegions)
		           ? regionEmits[region].load(std::memory_order_acquire) : 0;
	}
	void resetTallies() {
		for (auto &v : noteOnPart) v.store(0, std::memory_order_release);
		for (auto &v : partByteHist) v.store(0, std::memory_order_release);
		for (auto &v : regionEmits) v.store(0, std::memory_order_release);
		for (auto &v : soByteHist) v.store(0, std::memory_order_release);
	}

private:
	void threadFunc();

	std::thread mameThread;
	// Process-wide: set while ANY instance holds the machine. See start().
	static std::atomic<bool> sMachineLive;
	bool holdsMachine = false;
	std::atomic<bool> running{false};
	std::atomic<bool> stopFlag{false};
	std::string romPath, nvramDir;

	// One bit per button, written by the GUI thread and read by the machine thread.
	std::atomic<uint32_t> wantButtons{0};
	std::atomic<bool> resetting{false};
	std::thread resetThread;

	mutable std::mutex lcdMutex;
	std::vector<uint8_t> lcd;
	std::atomic<bool> lcdValid{false};

	mutable std::mutex ramMutex;
	std::vector<uint8_t> ram;
	std::atomic<bool> ramValid{false};
	std::atomic<uint64_t> ramGen{0};

	void emitRegionSysex(const MirrorRegion &region, const uint8_t *ramImage);
	void pushSysex(const uint8_t *msg, int len);

	// Previous contents of each mirrored region, so only real changes are sent.
	std::vector<std::vector<uint8_t>> mirrorPrev;
	bool mirrorPrimed = false;
	std::atomic<bool> toneReassert{true};
	std::atomic<bool> mirrorResync{false};
	// The firmware scribbles all over its RAM while it boots, so the one-shot resync that
	// brings the engine into line waits for it to settle rather than firing on the first
	// snapshot. Measured from the moment the machine is started.
	static constexpr int kBootSettleMs = 4000;
	std::chrono::steady_clock::time_point startedAt;
	bool bootResyncPending = false;

	// SysEx ring: MAME thread producer, audio thread consumer. Fixed-size slots so
	// the audio thread never allocates or blocks.
	//
	// 64 slots were not enough. A mirror rebuild sends all 13 regions at once, and since a
	// timbre change drags along eight more Tone Temporary reassertions
	// (MirrorRegion::reassertAfterTimbreTemp), the per-frame peak grew tenfold - and
	// sysexDropped() showed 3 lost messages over 40 seconds of demo. A lost mirror message is a
	// silently unapplied parameter, i.e. exactly the class of error being fixed here. A slot
	// is 256 bytes, so 256 slots cost 64 KB.
	static constexpr int kSysexSlots = 256;
	static constexpr int kSysexMask = kSysexSlots - 1;
	std::vector<uint8_t> sysexBuf;      // kSysexSlots * kMaxSysexBytes
	std::vector<uint16_t> sysexLen;
	std::atomic<int> sW{0}, sR{0};
	std::atomic<uint64_t> sysexCount{0}, sysexDropCount{0};

	// MIDI going the other way - host to firmware. A plain byte ring, because MIDI is a
	// byte stream and a message may legitimately be split across calls. Sized to swallow a
	// whole imported SysEx bank in one go: it drains at MIDI's real 3125 bytes a second,
	// so a large bank sits here for several seconds and must not be truncated meanwhile.
	static constexpr int kMidiSlots = 65536;
	static constexpr int kMidiMask = kMidiSlots - 1;
	std::vector<uint8_t> midiBuf;
	std::atomic<int> mW{0}, mR{0};
	std::atomic<uint64_t> midiInCount{0}, midiOutCount{0}, midiDropCount{0};

	std::atomic<int> extIntDiv{0};
	std::atomic<uint64_t> extIntCount{0};
	std::atomic<int> stuckPolicy{0};
	std::atomic<uint64_t> stuckCount{0};
	std::atomic<int> stuckIoc1{-1};
	std::atomic<uint64_t> la32Count{0}, la32ReadCount{0};
	std::atomic<int> lastUnresolvedContext{-1};
	std::atomic<uint64_t> unresolvedStreak{0};
	std::atomic<int> la32Mode{0};
	std::atomic<uint64_t> rampStarts{0}, rampLandings{0};
	std::atomic<bool> presetFf{false};
	std::atomic<int> la32Delay{0};

	// Memory card. cardWant is written by any thread, cardIsIn is touched only by the machine
	// thread - the only one allowed to touch MAME's shared memory.
	std::atomic<bool> cardWant{true};
	std::atomic<bool> cardProtect{false};
	std::atomic<bool> cardImageDirty{false};
	bool cardIsIn = true;
	mutable std::mutex cardMutex;
	std::vector<uint8_t> cardImage;
	const uint8_t *cardShared = nullptr; // the "memcs" shared memory, while the machine is alive

	std::atomic<bool> logUnmapped{false};
	mutable std::mutex logMutex;
	std::vector<std::string> logLines;
	static constexpr size_t kMaxLogLines = 200000;

	// Note events recovered from the firmware: machine thread produces, audio thread
	// consumes. Fixed slots so the audio thread never allocates or blocks.
	static constexpr int kNoteSlots = 512;
	static constexpr int kNoteMask = kNoteSlots - 1;
	std::vector<NoteEvent> noteBuf;
	std::atomic<int> nW{0}, nR{0};

	// Same shape, for hintRhythmKey()/osdTakeRhythmKeyHint(): audio thread pushes the MIDI
	// key of every note-on it forwards to the rhythm channel, machine thread pops the oldest
	// one when the firmware's own note-completion write is out of range. 32 slots is already
	// more concurrent rhythm hits than the hardware has voices for.
	static constexpr int kRhythmHintSlots = 32;
	static constexpr int kRhythmHintMask = kRhythmHintSlots - 1;
	std::vector<uint8_t> rhythmHintBuf;
	std::atomic<int> rhW{0}, rhR{0};
	std::atomic<uint64_t> noteOnCount{0}, noteOffCount{0}, noteMsTotal{0};
	// Fixed-size counters: unlike the two logs they CANNOT lose anything. Why that matters -
	// see the accessors above.
	std::atomic<uint64_t> noteOnPart[9] = {};
	std::atomic<uint64_t> partByteHist[16] = {};
	std::atomic<uint64_t> regionEmits[kMaxMirrorRegions] = {};
	std::atomic<int> soloPart{-1};
	// When the last MIDI byte went into the CPU's receiver, in milliseconds of the monotonic
	// clock. Zero means "none yet".
	std::atomic<int64_t> lastMidiByteMs{0};
	std::atomic<uint64_t> soByteHist[256] = {};
	std::atomic<int> soLast{-1};
	mutable std::mutex soMutex;
	std::vector<SoWrite> soTrace;
	std::chrono::steady_clock::time_point soTraceStart;
	bool soTracing = false;
	std::atomic<bool> soTracingOn{false};
	std::atomic<uint16_t> traceLo{0x0000}, traceHi{0xFFFF};
	uint64_t soDropped = 0;
	// Twenty thousand was not enough for the question the capture was made for: while a note
	// sounds, the envelope bank 0x0CC0 is rewritten continuously, and a half-second window was
	// already losing two thousand entries. A truncated stream cannot in principle answer "what
	// does it carry over time" - a lost tail is indistinguishable from an ended one. An entry
	// weighs 16 bytes.
	static constexpr size_t kMaxSoWrites = 400000;
	mutable std::mutex noteLogMutex;
	std::vector<NoteLog> noteLog;
	std::chrono::steady_clock::time_point noteLogStart;
	bool noteLogging = false;
	uint64_t noteLogDropped = 0;
	uint64_t ctxDropped = 0;

	std::atomic<bool> voiceCtxTap{false};
	mutable std::mutex ctxMutex;
	std::vector<CtxEvent> ctxEvents;
	static constexpr size_t kMaxCtxEvents = 200000;

	mutable std::mutex port2Mutex;
	std::vector<Port2Sample> port2Samples;
	static constexpr size_t kMaxPort2Samples = 50000;

	// PC histogram: written only by the machine thread, read under the mutex.
	std::atomic<bool> pcSampling{false};
	mutable std::mutex pcMutex;
	std::vector<uint64_t> pcHist;
	std::atomic<uint64_t> pcTotal{0};
};
