// Native (MAME-free) D-110 core, Phase 2 scope: CPU + minimal bus + LCD + panel buttons,
// stepped synchronously - no background thread, no real-time throttling, the caller decides
// how much emulated time to advance and when. This is the piece that, once complete, replaces
// the MAME-backed D110Core's own real-time thread and removes the two-clocks jitter documented
// in note_latency_probe.cpp and the project plan (see the "Native D-110 CPU core port" plan).
//
// NOT YET a drop-in replacement for D110Core: only the subset of its public interface Phase 2
// needs (boot, panel buttons, LCD) is implemented. NVRAM persistence, MIDI serial, the RAM
// mirror bridge to mt32emu, and StuckPolicy/EXTINT are Phases 3-5. PluginProcessor keeps using
// the MAME-backed D110Core unconditionally until this class's interface is complete and the
// full probe suite passes against it (see the plan's "Backend switch design" - a
// D110_NATIVE_CORE build option is added only once that point is reached).
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "d110_bus.h"
#include "mcs96_cpu.h"
#include "ram_mirror.h"

class D110CoreNative {
public:
	using u8 = uint8_t;

	static constexpr int kCols = 16;
	static constexpr int kLines = 2;
	static constexpr int kRowsPerChar = 8;
	static constexpr int kLcdBytes = kLines * kCols * kRowsPerChar;

	static constexpr int kNumPorts = 2;
	static constexpr int kNumBits = 8;
	static constexpr int kNumButtons = kNumPorts * kNumBits;
	static constexpr int buttonIndex(int port, int bit) { return port * kNumBits + bit; }

	// Roland exclusive-map layout constants, duplicated verbatim from D110Core.h so
	// PluginProcessor.cpp's deep-editor SysEx builders (sendTimbreTempParam, editPatchField,
	// sendRhythmParam, etc.) can address either backend through a single type alias without
	// changing their own logic - these are pure protocol/memory-map facts, not behaviour, and
	// identical for both cores since both run the same real firmware. See D110Core.h for how
	// each was originally measured; not re-derived here, just copied.
	static constexpr int kMaxSysexBytes = 256;
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
	static constexpr int kRamPatchName  = 0x2DAB;
	static constexpr int kRamPatchNumber = 0x2DB9;
	// Exclu Unit# as the firmware stores it (two copies, 0x2DB6/0x2DB7): the SysEx device ID it
	// answers to. Factory 0x10 (unit 17); a leftover Display Message flag makes the next boot
	// reset it to 0 (see D110AudioProcessor::firmwareDeviceId()).
	static constexpr int kRamUnitNumber = 0x2DB6;

	static constexpr int kNumPatches       = 64;
	static constexpr int kPatchRecord      = 128;
	static constexpr int kNumTimbres       = 128;
	static constexpr int kTimbreRecord     = 8;
	static constexpr int kNumParts         = 9;
	static constexpr int kTimbreTempRecord = 16;
	static constexpr int kToneRecord       = 246;
	static constexpr int kNumRhythmKeys    = 85;
	static constexpr int kRhythmRecord     = 4;
	static constexpr int kRhythmFirstKey   = 24;
	static constexpr int kNameChars        = 10;
	static constexpr int kRamTones      = 0x4000;
	static constexpr int kNumTones      = 64;
	static constexpr int kToneMemRecord = 256;

	static constexpr double kMidiBytesPerSecond = 31250.0 / 10.0;
	static constexpr int kCardSize = 0x8000;

	// LA32 voice-slot table layout - PluginEditor.cpp's Monitor tab reads these directly off
	// a RAM snapshot for its own diagnostic display, same as D110Core exposes them publicly.
	static constexpr int kNumHardwareVoices = 32;
	static constexpr uint16_t kSlotStateTable = 0x2DC0;
	static constexpr u8 kSlotBusyValue = 0x40;
	static constexpr u8 kSlotBusyValueAlt = 0x20;
	static constexpr u8 kSlotIdleValue = 0x80;

	// Same DT1 "Data set 1" builder as D110Core::buildDt1Message() - pure message-formatting
	// logic, no instance state, ported verbatim.
	static int buildDt1Message(uint32_t sysexAddress, int offset, const uint8_t *data,
	                           int length, uint8_t *out);

	// --- diagnostics the Monitor tab reads (PluginEditor.cpp) ---
	uint64_t ramGeneration() const { return ramGen_; }
	uint64_t sysexEmitted() const { return sysexEmitted_; }
	uint64_t sysexDropped() const { return 0; } // mirror queue is a plain deque - never drops
	uint64_t midiDelivered() const { return midiDelivered_; }
	uint64_t midiDropped() const { return 0; }  // MIDI-in queue is a plain deque - never drops
	// A byte handed to the CPU (midiDelivered above) can still be LOST if the firmware never
	// reads it before the next one lands - see Mcs96Cpu::serialOverrunCount()'s own comment.
	uint32_t serialOverrunCount() const { return cpu_.serialOverrunCount(); }

	// romFolder must contain d-110.v1.10.ic19.bin, r15179873-lh5310-97.ic12.bin and
	// msm6222b-01.bin - the same three files D110AudioProcessor::getAutoRomFolder() already
	// points at. Returns false (and leaves isRunning() false) if any is missing/wrong-sized.
	//
	// nvramDir, if non-empty, is treated exactly as MAME's own -nvram_directory: battery RAM
	// and the memory-card window are loaded from <nvramDir>/d110/rams and .../memcs if
	// present (32KB raw each, matching nvram_device::DEFAULT_ALL_0's plain-dump format
	// exactly - an existing MAME-backed D110Core session's folder loads here unchanged), or
	// left zero-filled otherwise. Nothing is written back until stop() is called.
	bool start(const std::string &romFolder, const std::string &nvramDir = {});
	// Writes rams/memcs back to nvramDir (if one was given) and stops stepping. Idempotent -
	// safe to call when not running.
	void stop();
	bool isRunning() const { return running_; }

	// Which of the three required files start() couldn't read (missing or wrong size), set
	// only when start() just returned false. Unlike D110Core (MAME-backed), there is no
	// process-wide "only one machine" lock here - each instance is fully independent - so a
	// false return can only ever mean a ROM problem, never another instance already running.
	const std::string &lastStartError() const { return lastStartError_; }

	// Advances the emulated machine by `seconds` of its own 12MHz-crystal time - deterministic
	// wall-clock-free stepping, unlike D110Core::start()'s real-time MAME thread. A host would
	// call this with exactly one audio block's worth of time per processBlock(), the same
	// shape as DX100-VST's stepOneSample() (see the plan) - not yet wired to an audio thread
	// in Phase 2, which is verified standalone via native_core_test.cpp instead.
	void runForSeconds(double seconds);

	void setButton(int index, bool down);
	void releaseAllButtons();
	uint32_t buttonMask() const { return buttonMask_; }

	// The documented cold start (D110Core::factoryReset()'s own comment: hold Write/Copy
	// across a reset, confirm with Enter). Ported almost unchanged - the one real difference
	// is that D110Core has to run this on a background thread because it can't block the
	// caller for ~10 real seconds without freezing the host, whereas this core's
	// runForSeconds() advances emulated time as fast as it can actually be computed (measured
	// well under real-time - see native_audio_probe.cpp), so this can simply block and return
	// once genuinely done. isResetting() always reads false as a result - by the time
	// factoryReset() returns, it already is. Does NOT wipe rams/memcs itself; exactly like
	// power-cycling real hardware, only the CPU resets - the firmware's own Write/Copy+Enter
	// routine is what overwrites battery RAM from the preset ROM, through ordinary RAM writes,
	// not an external shortcut.
	void factoryReset();
	bool isResetting() const { return false; }

	bool getLcd(u8 *out) const;

	// Direct access to the persisted battery RAM - mirrors D110Core::getRam()'s shape (a
	// plain 32KB snapshot), used by native_nvram_probe.cpp to verify the save/load round
	// trip and, from Phase 3's RAM-mirror hookup onward, by whatever feeds the existing
	// mirror-to-SysEx bridge (D110Core::osdSnapshotRam and friends - unchanged logic, just
	// needs a raw pointer, per the plan).
	static constexpr int kRamSize = 0x8000;
	bool getRam(u8 *out) const;
	void pokeRamForTest(size_t offset, u8 value);

	// RAM-to-SysEx mirror bridge, ported unchanged from D110Core's own (see ram_mirror.h).
	// popSysex drains one queued DT1 message (F0..F7) per call, matching D110Core::popSysex's
	// shape so PluginProcessor's existing drain loop needs no changes once this is wired in.
	int popSysex(u8 *out) { return mirror_.popSysex(out); }
	void resyncMirror() { mirror_.forceResync(); }

	// EXTINT/StuckPolicy - see D110Core.h's "the missing external interrupt"/"releasing the
	// stuck wait" comments for the full story. Off/PokeRam/PulseExtInt ported for interface
	// completeness (matching D110Core's own default of Off); La32Stub is what the shipping
	// plugin actually runs (PluginProcessor::setPoweredOn()) and is the only one exercised
	// against real audio so far (native_audio_probe.cpp). La32Ramps - the full hardware-ramp
	// envelope model - is not yet ported; it was rolled back in the MAME-backed shipping
	// plugin too (a real stuck-notes regression, see project memory), so porting it is a
	// Phase 6+ item, not a gap blocking anything currently working.
	enum class StuckPolicy { Off, PokeRam, PulseExtInt, La32Stub, La32Ramps };
	void setStuckPolicy(StuckPolicy p) { stuckPolicy_ = p; }
	StuckPolicy stuckPolicy() const { return stuckPolicy_; }

	// --- host MIDI into the firmware --------------------------------------------------
	// Queues bytes to be shifted into the CPU's own serial receiver at MIDI's own rate
	// (3125 bytes/sec, matching D110Core::kMidiBytesPerSecond) - see runForSeconds().
	void pushMidi(const u8 *bytes, int len);

	// See D110Core::hintRhythmKey() for the full story: three rhythm timbres (64/65/66)
	// make the firmware's own note-completion write carry a placeholder instead of the
	// real key, and the fix is to remember the key we sent and substitute it back in
	// inside noteWatch() below. Same ring-queue shape, just a plain deque now that
	// everything runs on one thread.
	void hintRhythmKey(u8 note) { rhythmHintQueue_.push_back(note); }

	// --- note tracking (D110Core::NoteEvent / noteWatch(), ported) ---------------------
	struct NoteEvent { u8 part; u8 note; u8 velocity; bool on; };
	bool popNoteEvent(NoteEvent &out);

	uint64_t firmwareNoteOns() const { return noteOnCount_; }
	uint64_t firmwareNoteOffs() const { return noteOffCount_; }

	// MIDI Panic support (Alan's report, 2026-08-30: notes sometimes stay audibly stuck, the
	// Monitor tab's voice-slot grid stays lit, and the LCD's per-part "sounding" indicator
	// stays on, all surviving a MIDI Panic that only sends CC64/CC123 as MIDI bytes). Root
	// cause: the real firmware's own release path needs its per-voice envelope-stage counter
	// (ROM-side, not emulated - see docs/la32_interface.md, "solved kept the panel alive, but
	// not the polyphony") to count up to 7 before it ever writes kReleaseTable's released bit -
	// pacing this emulation doesn't implement, so that write can simply never come, leaving a
	// context stuck ctxSounding_==true (and its LA32 dispatch slot stuck busy) forever with no
	// firmware action left to free it.
	//
	// Two separate methods, not one, because they need different CALLING patterns
	// (PluginProcessor::midiPanic()/processBlock() decide exactly how) - measured with
	// plugin/native_midi_panic_stuck_probe.cpp:
	//
	// releaseStuckNoteContexts() calls releaseContext() directly - the exact same path a real
	// firmware release already takes - for every context still marked sounding, so the note-off
	// genuinely reaches the sound engine through the normal noteQueue_/popNoteEvent() drain
	// (PluginProcessor.cpp). Safe and complete as a ONE-SHOT call - confirmed by the probe,
	// engineActivePartials() reliably reaches 0 right after.
	//
	// resetVoiceSlotTable() resets the LA32 voice-slot table (kSlotStateTable) to idle for
	// every hardware voice - a raw dispatch-bookkeeping table noteWatch() never watches, so a
	// direct write here doesn't interact with any CPU-side flow control DIRECTLY... but a
	// SINGLE call isn't enough: the panic's own CC64/CC123 bytes are still being paced out to
	// the firmware's own MIDI IN for ~30ms after this runs, and the firmware's response to THAT
	// (its own note-off search/reclaim attempt for the very same voices, ROM 0x2496) writes
	// kSlotStateTable again as it goes - overwriting a one-shot poke with a fresh busy value
	// before settling (measured: 2 of 3 slots still read busy 200ms after a single poke, but
	// reach idle on their own within ~2s if simply left alone - this is often a legitimately
	// slow but WORKING release, not the permanently-stuck case, and fighting it with one poke
	// only wins the race sometimes). The caller is expected to call this repeatedly for a short
	// window after a panic (PluginProcessor.h's own pendingSlotTableResetSamplesRemaining) so
	// the LAST call in that window - after the firmware's own delayed response has had time to
	// either genuinely finish or genuinely never finish - is the one that sticks either way.
	// Still doesn't reach the LCD's own per-part "sounding" indicator - that's real ROM display
	// code reacting to the same stuck firmware state from the CPU side, not a value this class
	// can just poke; a known remaining gap (see PluginProcessor::midiPanic()'s own comment).
	void releaseStuckNoteContexts();
	void resetVoiceSlotTable();

	// --- memory card, mirrors D110Core's own accessors over D110Bus's card fields ---
	void setCardInserted(bool in) { bus_.cardInserted = in; }
	bool cardInserted() const { return bus_.cardInserted; }
	void setCardWriteProtect(bool on) { bus_.cardWriteProtect = on; }
	bool cardWriteProtect() const { return bus_.cardWriteProtect; }
	bool getCardImage(u8 *out) const {
		std::memcpy(out, bus_.memcs.data(), bus_.memcs.size());
		return true;
	}
	void setCardImage(const u8 *data) {
		std::memcpy(bus_.memcs.data(), data, bus_.memcs.size());
	}

	// --- MIDI activity lamp, matches D110Core::midiLampOn()/midiLampValid() ---
	// Driven off elapsedSeconds_ (emulated time) rather than wall-clock steady_clock: when
	// this core is actually stepped from a live audio thread, emulated time already tracks
	// real time block by block, and using the same clock the rest of this class already
	// uses avoids a second, independent notion of "now".
	static constexpr double kMidiLampHoldSeconds = 0.090;
	bool midiLampOn() const {
		return lastMidiByteSeconds_ >= 0.0 && (elapsedSeconds_ - lastMidiByteSeconds_) < kMidiLampHoldSeconds;
	}
	bool midiLampValid() const { return true; }

	// Diagnostics for native_note_probe.cpp while it's tracked down - not part of the
	// eventual D110Core-identical interface.
	size_t midiQueuePendingForTest() const { std::lock_guard<std::mutex> lk(midiInMutex_); return midiInQueue_.size(); }
	uint64_t voiceCtxWriteCountForTest() const { return voiceCtxWriteCount_; }
	bool verboseNoteWatchForTest = false;
	bool serialRxReadyForTest() const { return cpu_.serialRxReady(); }
	uint16_t pcForTest() const { return cpu_.pc(); }
	bool extIntHighForTest() const { return extIntHigh_; }
	bool la32PendingForTest() const { return bus_.la32Pending; }
	uint64_t stuckLoopHitsForTest() const { return stuckLoopHits_; }

	// Snapshot of what the firmware is doing right now, for the DEBUG log (PluginProcessor.cpp's TALLY line):
	// when the panel dies and no note starts although MIDI keeps arriving, this says whether the CPU is parked
	// at the dispatch wait loop, and whether any hardware slot matches the context it waits for (-1 = none, so
	// nothing here can ever release it). Call from the thread that steps the core (the audio thread).
	struct FirmwareDiag {
		uint16_t pc = 0;
		uint64_t stuckLoopHits = 0;
		uint64_t midiDelivered = 0;
		size_t midiPending = 0;
		size_t rampBacklog = 0;
		bool la32Pending = false;
		bool extIntHigh = false;
		int waitContext = -1;
		int slotForContext = -1;
		int busySlots = 0;
		uint64_t unmatchedWaitReleases = 0;
	};
	FirmwareDiag firmwareDiag() const;
	// Test hook: pretend no hardware slot ever matches the wait context, to exercise the last-resort release.
	void setIgnoreSlotMatchForTest(bool v) { ignoreSlotMatchForTest_ = v; }
	// Raw voice-slot table, for the same log, only worth printing while the CPU is parked at the wait loop:
	// "slot:state/context" for every slot whose state byte is not the idle value (any other value than the two
	// busy ones serviceStuckPolicy() recognises would show up here), then the f440[] flags that are non-zero.
	std::string slotTableDump() const;
	size_t rampQueueDepthForTest() const { return rampLanded_.size(); }

private:
	D110Bus bus_;
	Mcs96Cpu cpu_;
	RamMirror mirror_;
	std::vector<uint8_t> cgromStorage_;   // keeps the CGROM bytes alive for D110Bus's raw pointer
	bool running_ = false;
	uint32_t buttonMask_ = 0;
	std::string nvramDir_;
	std::string lastStartError_;

	static constexpr double kCpuClockHz = 12'000'000.0;
	double cycleAccum_ = 0.0;
	// Mirror runs on its own cadence (D110Core's own "once per emulated video frame", 50Hz),
	// independent of however runForSeconds() happens to be chunked by the caller.
	static constexpr double kMirrorPeriodSeconds = 1.0 / 50.0;
	double mirrorAccum_ = 0.0;
	double elapsedSeconds_ = 0.0;

	// MIDI-in pacing, matching D110Core's own timer-driven midiTick(): one byte per elapsed
	// tick, no readiness gate (the real driver doesn't gate on it either). Ticks and CPU
	// stepping are interleaved at this granularity in runForSeconds() - NOT run as two
	// separate phases (all CPU cycles for the call, then all pending ticks) - because
	// delivering several MIDI bytes back-to-back with no CPU execution between them would
	// overwrite SBUF before the firmware's interrupt handler ever gets to run and read the
	// previous one, corrupting the message. Matches MAME's own scheduler, which interleaves
	// the midiTick emu_timer with CPU execution for the same reason. kMidiBytesPerSecond
	// itself is declared in the public layout-constants block above (PluginProcessor.cpp
	// needs it too, via D110Core::kMidiBytesPerSecond's counterpart).
	static constexpr double kTickPeriodSeconds = 1.0 / kMidiBytesPerSecond;
	double tickPhase_ = 0.0;
	// pushMidi() is called from the UI/message thread (editor writes, MIDI-file import) while
	// runForSeconds() drains the queue on the audio thread: an unguarded std::deque there is a
	// data race (garbled or dropped bytes, and eventually a wedged firmware).
	mutable std::mutex midiInMutex_;
	std::deque<u8> midiInQueue_;
	double lastMidiByteSeconds_ = -1.0;
	uint64_t midiDelivered_ = 0;

	// ramGen_ increments once per mirror tick (50Hz, kMirrorPeriodSeconds) - coarser than
	// D110Core's own "only on genuine content change" tracking, but ramGeneration() only
	// exists so the Monitor tab can skip redundant redraws, not for correctness, so the
	// simpler always-fresh-at-50Hz counter is a fine trade for not hooking every RAM write.
	uint64_t ramGen_ = 0;
	uint64_t sysexEmitted_ = 0;

	// Voice-context tracking, ported from D110Core.cpp's D110Osd (private m_ctx* arrays) -
	// see noteWatch()/releaseContext() in the .cpp.
	static constexpr uint16_t kNoteTable = 0x3400;
	static constexpr uint16_t kVelocityTable = 0x3420;
	static constexpr uint16_t kPartTable = 0x33A0;
	static constexpr uint16_t kReleaseTable = 0x3460;
	static constexpr uint8_t kReleasedBit = 0x40;
	static constexpr int kNumVoiceContexts = 32;
	u8 ctxNote_[kNumVoiceContexts] = {};
	u8 ctxVelocity_[kNumVoiceContexts] = {};
	u8 ctxPart_[kNumVoiceContexts] = {};
	bool ctxSounding_[kNumVoiceContexts] = {};
	bool ctxNoteFresh_[kNumVoiceContexts] = {};
	std::deque<NoteEvent> noteQueue_;
	std::deque<u8> rhythmHintQueue_;
	uint64_t noteOnCount_ = 0, noteOffCount_ = 0;
	uint64_t voiceCtxWriteCount_ = 0;

	void noteWatch(uint16_t ramsOffset, u8 value);
	void releaseContext(int ctx);

	// EXTINT/StuckPolicy::La32Stub, ported from D110Osd::midiTick() in D110Core.cpp - see
	// that comment ("the missing external interrupt") for the full story: the firmware
	// spins at 0x29E9 waiting for a sound-board interrupt MAME never emulated, and this
	// supplies it. Only La32Stub is ported (the shipping plugin's actual default, per
	// PluginProcessor::setPoweredOn()) - the cruder Off/PokeRam/PulseExtInt policies and the
	// full La32Ramps envelope model are Phase 5 completeness items, not needed to unblock
	// note round-trip testing.
	static constexpr uint16_t kStuckLoopPc = 0x29E9;
	static constexpr uint16_t kStuckLoopPcAlt = 0x29EE;
	static constexpr int kWaitIndexReg = 0x52;
	// kSlotStateTable/kNumHardwareVoices/kSlotBusyValue/kSlotBusyValueAlt are declared in the
	// public block above (the Monitor tab reads them too); kSlotContextTable stays here since
	// only La32Stub's own slot lookup uses it.
	static constexpr uint16_t kSlotContextTable = 0x2E01;
	static constexpr uint16_t kVoiceFlagBase = 0x3440; // f440[], PokeRam target
	static constexpr int kVoiceFlagSpan = 32;
	StuckPolicy stuckPolicy_ = StuckPolicy::Off;
	bool extIntHigh_ = false;
	uint64_t stuckLoopHits_ = 0;
	// How long (emulated seconds) the CPU may sit at the dispatch wait with no slot to answer it before the
	// flag is raised by hand - see serviceStuckPolicy().
	static constexpr double kUnmatchedWaitSeconds = 2.0;
	// resetVoiceSlotTable() leaves the table alone until the MIDI input has been quiet this long.
	static constexpr double kSlotResetQuietSeconds = 0.15;
	static constexpr double kLeftWaitSeconds = 0.05;
	double stuckWaitSince_ = -1.0;
	double stuckWaitLastSeen_ = -1.0;
	int stuckWaitContext_ = -1;
	bool ignoreSlotMatchForTest_ = false;
	uint64_t unmatchedWaitReleases_ = 0;
	void serviceStuckPolicy();

	// La32Ramps: the real hardware model, not a stub. LA32's amplitude/filter envelopes are
	// actual analogue ramps the chip counts down itself and reports on arrival, whatever the
	// CPU happens to be doing - so this is what makes a voice slot genuinely free again once
	// its envelope finishes, instead of only ever filling up (La32Stub's own limitation: it
	// answers "voice dispatched" but never "voice finished", so all 32 hardware slots end up
	// permanently busy after enough notes and further ones get stolen or dropped - exactly
	// the polyphony complaint this is fixing). Ported from D110Core.cpp's D110Osd (rampWrite/
	// startRamp/advanceRamps/rampStatusByte, La32RampState) - same law LA32Ramp in munt uses,
	// reconstructed from real recordings, not invented here.
	//
	// One real bug fixed relative to the original: D110Osd's EXTINT-clear check used
	// `!stuck` (CPU not at the La32Stub wait-loop PC) as a fallback clear trigger, shared
	// across every policy. Under La32Ramps the CPU is essentially never AT that PC - the
	// whole point of real ramps is that it isn't parked waiting - so `!stuck` was almost
	// always true and dropped the line one tick (~320us) after every assert, independent of
	// whether the firmware's interrupt handler had actually read the status yet. A landed
	// ramp whose interrupt arrived slightly late could have its edge withdrawn before the
	// CPU ever took it, and EXTINT is edge-triggered - the next assert needs a fresh
	// low-to-high transition, so a swallowed one doesn't just delay that notification, it
	// can be dropped. That reads exactly like the unresolved "second note plays 95% quieter"
	// regression that got La32Ramps rolled back on the MAME-backed core. This port's clear
	// condition is la32Pending having genuinely gone false (the read happened) - no PC-based
	// fallback - matching what La32Stub already does correctly here (see serviceStuckPolicy).
	struct La32RampState {
		double current = 0.0;   // same large units munt's own LA32Ramp uses: value << 18
		double increment = 0.0; // per LA32 chip sample (32kHz)
		double target = 0.0;
		bool descending = false;
		bool running = false;
		bool landed = false;
	};
	static constexpr int kRampBanks = 2; // 0 = amp (0x0C80), 1 = filter (0x0C00)
	static constexpr uint16_t kAmpRampBase = 0x0C80;
	static constexpr uint16_t kFilterRampBase = 0x0C00;
	static constexpr double kLa32SampleRate = 32000.0;
	La32RampState ramp_[kRampBanks][kNumHardwareVoices];
	u8 rampTarget_[kRampBanks][kNumHardwareVoices] = {};
	u8 rampIncrement_[kRampBanks][kNumHardwareVoices] = {};
	int slotBank_[kNumHardwareVoices] = {};
	std::deque<std::pair<int, int>> rampLanded_;

	static double rampIncrementOf(u8 increment);
	void rampWrite(uint16_t addr, u8 value);
	void startRamp(int bankIndex, int slot);
	void advanceRamps(double samples);
	// Mode "voice+1, no bank flag" - the one D110Core.cpp's own investigation proved correct
	// for this specific mechanism (a different default than La32Stub's encoding needs; see
	// the class comment above and D110Core.cpp's matching note on setLa32StatusMode(1)).
	static u8 rampStatusByte(int /*bank*/, int slot) { return u8((slot + 1) & 0x1f); }

	// roland_d10.cpp's "samples_timer" toggles port0 bit 4 at 32000*2 Hz - not just cosmetic:
	// note-processing code busy-waits on it changing (ROM 0x2824/0x282A - "ldb 8a, port0" /
	// "jbc 8a, 4, 2824" then the mirror wait for it to clear again), so a fixed port0 value
	// left the CPU spinning forever the instant a note tried to settle. Toggled once per
	// MIDI-rate tick (3125Hz) rather than the real 64kHz - about 10x slower, but this is a
	// plain busy-wait for "has it changed at all", not a rate-sensitive signal, so the
	// coarser cadence only adds a similarly small, bounded delay, not a correctness issue.
	bool port0Bit4_ = false;
};
