#include "D110CoreNative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

bool loadFile(const std::string &path, std::vector<uint8_t> &out, size_t expectedSize) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return out.size() == expectedSize;
}

// In two-line mode the MSM6222B's render buffer puts line 1 at character 0 and line 2 at
// character 40, 16 bytes per character (msm6222b.h/.cpp) - same layout D110Core.cpp's own
// osdSnapshotLcd extraction uses, kept identical here so the two cores' getLcd() output is
// directly comparable byte for byte.
constexpr int kRenderLine0 = 0;
constexpr int kRenderLine1 = 40;
constexpr int kRenderStride = 16;

} // namespace

bool D110CoreNative::start(const std::string &romFolder, const std::string &nvramDir) {
	std::vector<uint8_t> firmware, presets, cgrom;
	if (!loadFile(romFolder + "/d-110.v1.10.ic19.bin", firmware, 0x8000)) {
		lastStartError_ = "d-110.v1.10.ic19.bin missing or wrong size (expected 32768 bytes) in " + romFolder;
		return false;
	}
	if (!loadFile(romFolder + "/r15179873-lh5310-97.ic12.bin", presets, 0x20000)) {
		lastStartError_ = "r15179873-lh5310-97.ic12.bin missing or wrong size (expected 131072 bytes) in " + romFolder;
		return false;
	}
	if (!loadFile(romFolder + "/msm6222b-01.bin", cgrom, 0x1000)) {
		lastStartError_ = "msm6222b-01.bin missing or wrong size (expected 4096 bytes) in " + romFolder;
		return false;
	}
	lastStartError_.clear();

	bus_.setFirmwareRom(firmware.data(), firmware.size());
	bus_.setPresetsRom(presets.data(), presets.size());
	// cgromStorage_ keeps the bytes alive for the bus/LCD's raw pointer - see header.
	cgromStorage_ = std::move(cgrom);
	bus_.setLcdCgrom(cgromStorage_.data());

	cpu_.busRead8 = [this](uint16_t a) { return bus_.read(a); };
	cpu_.busWrite8 = [this](uint16_t a, uint8_t v) { bus_.write(a, v); };
	bus_.onVoiceCtxWrite = [this](uint16_t off, uint8_t v) { noteWatch(off, v); };
	bus_.onLa32RegWrite = [this](uint16_t addr, uint8_t v) { rampWrite(addr, v); };
	// machine_reset()'s m_port0 = 0x80 (battery ok), bit 4 the samples_timer toggle - see
	// port0Bit4_'s comment for why this can't be a fixed value.
	cpu_.inP0Cb = [this] { return uint8_t(0x80 | (port0Bit4_ ? 0x10 : 0)); };

	nvramDir_ = nvramDir;
	if (!nvramDir_.empty()) {
		std::vector<uint8_t> rams, memcs;
		// Same 32KB-raw-dump format nvram_device::DEFAULT_ALL_0 writes - an existing
		// MAME-backed session's folder is read back byte for byte, or left zero-filled if
		// absent, matching a factory-fresh battery exactly as the MAME path does.
		if (loadFile(nvramDir_ + "/d110/rams", rams, 0x8000)) {
			// A Display Message SysEx (address 0x200000) still pending when the RAM was saved
			// leaves bit 6 of 0x36E6 set; the firmware's next boot then resets its Exclu Unit#
			// (0x2DB6/7) to 0, after which it ignores every DT1 addressed to the factory device
			// ID 0x10 (measured, plugin/channel_import_probe.cpp, since removed). The message is
			// cosmetic and gone after a restart anyway, so drop the flag before the boot sees it.
			rams[0x36E6] &= uint8_t(~0x40);
			// Already-damaged saves: both copies of Exclu Unit# at 0 is that reset (a deliberate
			// "unit 1" is far less likely than the damage), and would leave every external
			// sequencer/librarian addressing 0x10 ignored. Restore the factory 17.
			if (rams[0x2DB6] == 0 && rams[0x2DB7] == 0) rams[0x2DB6] = rams[0x2DB7] = 0x10;
			bus_.rams = std::move(rams);
		}
		if (loadFile(nvramDir_ + "/d110/memcs", memcs, 0x8000)) bus_.memcs = std::move(memcs);
	}

	cpu_.reset();
	cycleAccum_ = 0.0;
	mirrorAccum_ = 0.0;
	elapsedSeconds_ = 0.0;
	tickPhase_ = 0.0;
	mirror_ = RamMirror();
	{
		std::lock_guard<std::mutex> lk(midiInMutex_);
		midiInQueue_.clear();
	}
	noteQueue_.clear();
	rhythmHintQueue_.clear();
	for (int i = 0; i < kNumVoiceContexts; ++i) {
		ctxNote_[i] = ctxVelocity_[i] = ctxPart_[i] = 0;
		ctxSounding_[i] = ctxNoteFresh_[i] = false;
	}
	noteOnCount_ = noteOffCount_ = 0;
	extIntHigh_ = false;
	bus_.la32Pending = false;
	bus_.la32Status = 0;
	port0Bit4_ = false;
	lastMidiByteSeconds_ = -1.0;
	midiDelivered_ = 0;
	ramGen_ = 0;
	sysexEmitted_ = 0;
	rampLanded_.clear();
	for (int b = 0; b < kRampBanks; ++b)
		for (int s = 0; s < kNumHardwareVoices; ++s) {
			ramp_[b][s] = La32RampState{};
			rampTarget_[b][s] = rampIncrement_[b][s] = 0;
		}
	for (int s = 0; s < kNumHardwareVoices; ++s) slotBank_[s] = 0;
	running_ = true;
	return true;
}

void D110CoreNative::stop() {
	if (running_ && !nvramDir_.empty()) {
		std::error_code ec;
		std::filesystem::create_directories(nvramDir_ + "/d110", ec);
		std::ofstream rams(nvramDir_ + "/d110/rams", std::ios::binary);
		rams.write(reinterpret_cast<const char *>(bus_.rams.data()), std::streamsize(bus_.rams.size()));
		std::ofstream memcs(nvramDir_ + "/d110/memcs", std::ios::binary);
		memcs.write(reinterpret_cast<const char *>(bus_.memcs.data()), std::streamsize(bus_.memcs.size()));
	}
	running_ = false;
}

void D110CoreNative::runForSeconds(double seconds) {
	if (!running_) return;

	// CPU stepping and serial/EXTINT servicing interleave one tick (1/3125s) at a time - see
	// the header comment on kTickPeriodSeconds for why running the whole CPU budget first and
	// delivering pending ticks afterward is wrong.
	double remaining = seconds;
	while (remaining > 0.0) {
		const double slice = std::min(remaining, kTickPeriodSeconds - tickPhase_);

		cycleAccum_ += slice * kCpuClockHz;
		while (cycleAccum_ >= 1.0) {
			int chunk = int(cycleAccum_ > 4000.0 ? 4000.0 : cycleAccum_);
			if (chunk <= 0) break;
			int actual = cpu_.run(chunk);
			cycleAccum_ -= actual;
		}
		elapsedSeconds_ += slice;
		remaining -= slice;

		tickPhase_ += slice;
		if (tickPhase_ >= kTickPeriodSeconds) {
			tickPhase_ -= kTickPeriodSeconds;
			// One byte per elapsed tick - matches D110Osd::midiTick() (bytes arrive spaced
			// as they would down a real MIDI cable). Originally no readiness gate at all
			// here (deliberately, matching that same real-cable timing), but that let a
			// byte the firmware hadn't read yet get silently clobbered by the next one -
			// SBUF (Mcs96Cpu) only models a single holding register, one byte deep, with
			// no shift-register buffering the way real 8097BH silicon likely has. Measured
			// on Alan's own machine (2026-08-19, see
			// project_sequencer_channel_collision_fix memory): 31 overruns in under 2400
			// bytes during ONE short passage - not a rare corner case. Waiting a tick
			// instead of overwriting can only cost a little extra latency (at most a
			// couple of byte periods, ~0.6ms, in the rare case the firmware is genuinely
			// still busy) - never loses data outright, which silently corrupting whatever
			// MIDI message happened to be next (a note-off often enough to matter) is
			// strictly worse than.
			{
				std::lock_guard<std::mutex> lk(midiInMutex_);
				if (!midiInQueue_.empty() && cpu_.serialRxReady()) {
					cpu_.serialWrite(midiInQueue_.front());
					midiInQueue_.pop_front();
					++midiDelivered_;
				}
			}
			serviceStuckPolicy();
			port0Bit4_ = !port0Bit4_;
		}
	}

	mirrorAccum_ += seconds;
	while (mirrorAccum_ >= kMirrorPeriodSeconds) {
		const uint64_t before = mirror_.messagesEmitted();
		mirror_.update(bus_.rams.data(), elapsedSeconds_);
		sysexEmitted_ += mirror_.messagesEmitted() - before;
		++ramGen_;
		mirrorAccum_ -= kMirrorPeriodSeconds;
	}
}

void D110CoreNative::serviceStuckPolicy() {
	// La32Ramps runs unconditionally, every tick, regardless of the CPU's PC - real hardware
	// counts ramps and raises the interrupt whatever the processor happens to be doing.
	if (stuckPolicy_ == StuckPolicy::La32Ramps) {
		advanceRamps(kLa32SampleRate / kMidiBytesPerSecond);

		// The line must actually go LOW for a tick before it can go high again - see
		// Mcs96Cpu::setExtIntLine()'s own comment: level 7 is the one interrupt the CPU never
		// auto-clears on take, specifically so a level-triggered line's requester (here, us)
		// is the one responsible for lowering it once serviced. Without this, promoting the
		// next queued landing in the SAME tick the previous one was read would leave
		// extIntHigh_ true straight through - no falling edge ever reaches the CPU, and
		// pending_irq's EXTINT bit is never cleared. This alone turned out NOT to be the
		// user's reported freeze (measured: native_ramp_edge_stress_probe.cpp still hung with
		// only this fix), but it is still a real, separate correctness bug worth keeping fixed.
		if (extIntHigh_ && !bus_.la32Pending) {
			cpu_.setExtIntLine(false);
			extIntHigh_ = false;
			return;
		}

		if (!bus_.la32Pending && !rampLanded_.empty()) {
			const auto ev = rampLanded_.front();
			rampLanded_.pop_front();
			bus_.la32Status = rampStatusByte(ev.first, ev.second);
			bus_.la32Pending = true;
		}

		// The actual root cause (found by reading, THEN confirmed by measurement - first
		// reproduction of this freeze in any offline probe): a ramp landing only ever answers
		// a voice's OWN envelope-stage completion. It says nothing about a voice still parked
		// at the dispatch wait-loop below (D110Core.h's "the missing external interrupt":
		// every note-on spins at kStuckLoopPc polling its own f440[] flag until an interrupt
		// sets it) - because nothing has been written to THAT voice's ramp registers yet for
		// anything to land; the firmware can't get there until this very wait releases it.
		// Dispatch-ack and envelope-stage-ack are the SAME physical interrupt/status-byte
		// channel on real hardware (this is exactly what La32Stub's branch below answers), so
		// when the channel is idle, service this handshake here too - a lone note's own
		// dispatch has nothing else competing for the channel and resolves practically
		// instantly, which is why single/well-spaced notes essentially never showed this; dense
		// overlapping notes/chords do, 100% reproducibly, within about 50 chords
		// (native_ramp_edge_stress_probe.cpp): the newly dispatched voice's context never gets
		// its own bit set, the CPU never leaves the busy-wait, and since that wait is the
		// firmware's entire mainline execution, NOTHING else - other dispatches, panel scan,
		// LCD refresh - runs again either. Silent full freeze, matching the user's report.
		if (!bus_.la32Pending && !extIntHigh_) {
			const uint16_t pc = cpu_.pc();
			if (pc == kStuckLoopPc || pc == kStuckLoopPcAlt) {
				const uint8_t context = cpu_.regFile[kWaitIndexReg];
				for (int n = 0; n < kNumHardwareVoices; ++n) {
					const uint8_t busy = bus_.rams[kSlotStateTable + 2 * n];
					if ((busy == kSlotBusyValue || busy == kSlotBusyValueAlt) &&
					    bus_.rams[kSlotContextTable + 2 * n] == context) {
						bus_.la32Status = uint8_t((n + 1) & 0x1f); // same encoding as La32Stub
						bus_.la32Pending = true;
						break;
					}
				}
			}
		}

		if (bus_.la32Pending && !extIntHigh_) {
			cpu_.setExtIntLine(true);
			extIntHigh_ = true;
		}
		return;
	}

	// La32Stub only: once the handler has collected the status byte (bus_.la32Pending
	// cleared by the read at 0x0C00), drop the line so it can rise again for the next voice.
	// setExtIntLine(false) already clears the CPU's own pending-interrupt bit (Phase 1's
	// Finding-2 fix), so unlike D110Osd this needs no separate MCS96_INT_PENDING patch.
	if (stuckPolicy_ == StuckPolicy::La32Stub && extIntHigh_ && !bus_.la32Pending) {
		cpu_.setExtIntLine(false);
		extIntHigh_ = false;
	}

	const uint16_t pc = cpu_.pc();
	if (pc != kStuckLoopPc && pc != kStuckLoopPcAlt) return;
	++stuckLoopHits_;

	switch (stuckPolicy_) {
	case StuckPolicy::Off:
		break;

	case StuckPolicy::PokeRam:
		// Sets the flag byte directly, bypassing the interrupt handler - releases this loop
		// but everything else the handler would have updated stays stale (D110Core.h's own
		// documentation of why this is the crude option, not the shipping one).
		for (int i = 0; i < kVoiceFlagSpan; ++i)
			bus_.rams[kVoiceFlagBase + i] |= 0x80;
		break;

	case StuckPolicy::PulseExtInt:
		// Give it a falling edge between pulses even while still stuck - matches
		// D110Osd's own comment on why this alternates rather than just asserting once.
		cpu_.setExtIntLine(!extIntHigh_);
		extIntHigh_ = !extIntHigh_;
		break;

	case StuckPolicy::La32Stub: {
		if (bus_.la32Pending || extIntHigh_) break; // already servicing one

		// Report the hardware voice SLOT dispatch assigned to the SPECIFIC context the CPU
		// is parked waiting for right now (r52, read straight out of the register file - no
		// MAME memshare lookup needed, cpu_.regFile is already absolute-indexed).
		const uint8_t context = cpu_.regFile[kWaitIndexReg];
		int slot = -1;
		for (int n = 0; n < kNumHardwareVoices; ++n) {
			const uint8_t busy = bus_.rams[kSlotStateTable + 2 * n];
			if ((busy == kSlotBusyValue || busy == kSlotBusyValueAlt) &&
			    bus_.rams[kSlotContextTable + 2 * n] == context) {
				slot = n;
				break;
			}
		}
		if (slot < 0) break;

		// Mode-0 encoding (the shipping default - PluginProcessor never calls
		// setLa32StatusMode()): (voice+1)&0x1f. See D110Core::encodeLa32Status().
		bus_.la32Status = uint8_t((slot + 1) & 0x1f);
		bus_.la32Pending = true;
		cpu_.setExtIntLine(true);
		extIntHigh_ = true;
		break;
	}

	case StuckPolicy::La32Ramps:
		break; // handled by the early return above; listed so the switch stays exhaustive
	}
}

void D110CoreNative::pushMidi(const u8 *bytes, int len) {
	if (len > 0) lastMidiByteSeconds_ = elapsedSeconds_;
	std::lock_guard<std::mutex> lk(midiInMutex_);
	for (int i = 0; i < len; ++i) midiInQueue_.push_back(bytes[i]);
}

bool D110CoreNative::popNoteEvent(NoteEvent &out) {
	if (noteQueue_.empty()) return false;
	out = noteQueue_.front();
	noteQueue_.pop_front();
	return true;
}

// Ported from D110Core.cpp's D110Osd::noteWatch() - see that comment for the full story
// (ROM 0x278B/0x2790/0x2795's note-start triple, the voice-stealing release-before-restart
// fix, and the rhythm-key-hint substitution for timbres 64/65/66).
void D110CoreNative::noteWatch(uint16_t ramsOffset, u8 value) {
	++voiceCtxWriteCount_;
	if (verboseNoteWatchForTest)
		std::printf("noteWatch off=%04x val=%02x\n", ramsOffset, value);
	if (ramsOffset >= kNoteTable && ramsOffset < kNoteTable + kNumVoiceContexts) {
		const int ctx = ramsOffset - kNoteTable;
		if ((value & 0x80) == 0) {
			if (ctxSounding_[ctx] && ctxNote_[ctx] != value) releaseContext(ctx);
			ctxNote_[ctx] = value;
			ctxNoteFresh_[ctx] = true;
		}
		return;
	}
	if (ramsOffset >= kVelocityTable && ramsOffset < kVelocityTable + kNumVoiceContexts) {
		ctxVelocity_[ramsOffset - kVelocityTable] = value;
		return;
	}
	if (ramsOffset >= kPartTable && ramsOffset < kPartTable + kNumVoiceContexts) {
		const int ctx = ramsOffset - kPartTable;
		const u8 part = u8(value >> 4);
		if (part > 8) return;
		ctxPart_[ctx] = part;
		if (ctxNoteFresh_[ctx]) {
			u8 note = ctxNote_[ctx];
			if (part == 8 && (note < 24 || note > 108) && !rhythmHintQueue_.empty()) {
				note = rhythmHintQueue_.front();
				rhythmHintQueue_.pop_front();
			}
			if (note > 0 && note <= 127) {
				noteQueue_.push_back({part, note, ctxVelocity_[ctx], true});
				ctxSounding_[ctx] = true;
				++noteOnCount_;
			}
		}
		ctxNoteFresh_[ctx] = false;
		return;
	}
	if (ramsOffset >= kReleaseTable && ramsOffset < kReleaseTable + kNumVoiceContexts) {
		const int ctx = ramsOffset - kReleaseTable;
		if (value & kReleasedBit) releaseContext(ctx);
	}
}

void D110CoreNative::releaseContext(int ctx) {
	if (!ctxSounding_[ctx]) return;
	noteQueue_.push_back({ctxPart_[ctx], ctxNote_[ctx], 0, false});
	ctxSounding_[ctx] = false;
	++noteOffCount_;
}

// See the header's own comment on why this is split from resetVoiceSlotTable() and what each
// does/doesn't reach.
void D110CoreNative::releaseStuckNoteContexts() {
	for (int ctx = 0; ctx < kNumVoiceContexts; ++ctx) releaseContext(ctx);
}

void D110CoreNative::resetVoiceSlotTable() {
	for (int slot = 0; slot < kNumHardwareVoices; ++slot)
		bus_.rams[size_t(kSlotStateTable) + size_t(slot) * 2] = kSlotIdleValue;
}

void D110CoreNative::setButton(int index, bool down) {
	if (index < 0 || index >= kNumButtons) return;
	const uint32_t bit = 1u << index;
	buttonMask_ = down ? (buttonMask_ | bit) : (buttonMask_ & ~bit);
	bus_.setButton(index % kNumBits, down, index >= kNumBits);
}

// Same DT1 "Data set 1" message builder as D110Core::buildDt1Message() - pure formatting
// logic (Roland address split into 3 seven-bit bytes, checksum making address+data+checksum
// a multiple of 128), no instance state, ported verbatim.
int D110CoreNative::buildDt1Message(uint32_t sysexAddress, int offset, const uint8_t *data,
                                    int length, uint8_t *out) {
	if (length <= 0 || length > kMaxSysexBytes - 12 || offset < 0) return 0;

	int n = 0;
	out[n++] = 0xF0;
	out[n++] = 0x41; // Roland
	out[n++] = 0x10; // device ID
	out[n++] = 0x16; // model: MT-32 family
	out[n++] = 0x12; // DT1

	const uint32_t linear = (((sysexAddress >> 16) & 0x7f) << 14)
	                      | (((sysexAddress >> 8) & 0x7f) << 7) | (sysexAddress & 0x7f);
	const uint32_t target = linear + uint32_t(offset);
	const uint8_t a1 = uint8_t((target >> 14) & 0x7f);
	const uint8_t a2 = uint8_t((target >> 7) & 0x7f);
	const uint8_t a3 = uint8_t(target & 0x7f);
	out[n++] = a1;
	out[n++] = a2;
	out[n++] = a3;

	uint32_t sum = a1 + a2 + a3;
	for (int i = 0; i < length; ++i) {
		const uint8_t v = data[i] & 0x7f;
		out[n++] = v;
		sum += v;
	}
	out[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
	out[n++] = 0xF7;
	return n;
}

// Same law LA32Ramp in munt uses (reconstructed there from real hardware recordings) -
// increment byte: high bit is direction, low seven bits are an exponential rate. Zero moves
// nothing and raises nothing.
double D110CoreNative::rampIncrementOf(u8 increment) {
	if (increment == 0) return 0.0;
	const double large = std::exp2((double(increment & 0x7f) + 24.0) / 8.0);
	return (increment & 0x80) ? large + 1.0 : large; // descending is slightly faster on the real chip
}

// Register write: even byte of the 0x0D00 bank is the per-slot bank-select flag (bit 7 picks
// which of the two ramp banks this voice uses - see D110Core.cpp's own measurement, ROM
// 0x3B11); everything else is target/increment pairs in the two ramp banks. The ramp starts
// on the TARGET write specifically, because a 16-bit write lands the low byte first and the
// firmware's own separate byte writes follow the same order.
void D110CoreNative::rampWrite(uint16_t addr, u8 value) {
	if (addr >= 0x0d00 && addr < 0x0d40 && !(addr & 1)) {
		const int slot = (addr - 0x0d00) / 2;
		slotBank_[slot] = (value & 0x80) ? 1 : 0;
		return;
	}
	int bankIndex = -1;
	if (addr >= kAmpRampBase && addr < kAmpRampBase + 0x40) bankIndex = 0;
	else if (addr >= kFilterRampBase && addr < kFilterRampBase + 0x40) bankIndex = 1;
	if (bankIndex < 0) return;
	const int within = addr - (bankIndex ? kFilterRampBase : kAmpRampBase);
	const int slot = within / 2;
	if (within & 1) {
		rampTarget_[bankIndex][slot] = value;
		// Only the bank this voice is actually flagged to use is a real ramp - a write to
		// the other one isn't a ramp and treating it as one invents interrupts.
		if (slotBank_[slot] == bankIndex) startRamp(bankIndex, slot);
	} else {
		rampIncrement_[bankIndex][slot] = value;
	}
}

// Target+increment pair now known - start (or immediately land) the ramp. Starting point is
// wherever the value currently sits: on the real chip that's the end of the previous ramp,
// and it's the same here.
void D110CoreNative::startRamp(int bankIndex, int slot) {
	if (bankIndex < 0 || bankIndex >= kRampBanks || slot < 0 || slot >= kNumHardwareVoices) return;
	La32RampState &r = ramp_[bankIndex][slot];
	const u8 inc = rampIncrement_[bankIndex][slot];
	r.increment = rampIncrementOf(inc);
	r.descending = (inc & 0x80) != 0;
	r.target = double(rampTarget_[bankIndex][slot]) * double(1 << 18);
	r.landed = false;
	r.running = r.increment != 0.0;
	if (!r.running) return;
	// Increment 0xFF means "set and don't interrupt", not a speed - without this a note dies
	// at zero milliseconds: the firmware writes these registers BEFORE marking the slot
	// busy, so an immediate interrupt finds the slot still free and mutes it (ROM 0x3160).
	if (inc == 0xff) {
		r.current = r.target;
		r.running = false;
		return;
	}
	// Target already reached - the chip lands on it immediately and reports right away.
	if ((r.descending && r.current <= r.target) || (!r.descending && r.current >= r.target)) {
		r.current = r.target;
		r.running = false;
		r.landed = true;
		rampLanded_.emplace_back(bankIndex, slot);
	}
}

// Advances every running ramp by `samples` LA32-chip-samples (32kHz) and queues arrivals.
// Called at whatever cadence is convenient (the increment is constant, so any step size
// computes with one multiply) - here, once per MIDI-rate tick, matching D110Core.cpp's own
// advanceRamps(kLa32SampleRate / kMidiBytesPerSecond) call.
void D110CoreNative::advanceRamps(double samples) {
	for (int b = 0; b < kRampBanks; ++b)
		for (int s = 0; s < kNumHardwareVoices; ++s) {
			La32RampState &r = ramp_[b][s];
			if (!r.running) continue;
			r.current += r.descending ? -r.increment * samples : r.increment * samples;
			const bool arrived = r.descending ? r.current <= r.target : r.current >= r.target;
			if (!arrived) continue;
			r.current = r.target;
			r.running = false;
			r.landed = true;
			rampLanded_.emplace_back(b, s);
		}
}

void D110CoreNative::factoryReset() {
	if (!running_) return;
	releaseAllButtons();
	setButton(buttonIndex(0, 0), true); // Write/Copy, held across the reset

	// "Restart the machine": reset the CPU and everything that's genuinely CPU/session
	// state, but not battery RAM (rams/memcs) - matches power-cycling real hardware, which
	// does not forget what was in it.
	cpu_.reset();
	cycleAccum_ = 0.0;
	tickPhase_ = 0.0;
	extIntHigh_ = false;
	bus_.la32Pending = false;
	port0Bit4_ = false;

	runForSeconds(5.0); // let the firmware come up
	setButton(buttonIndex(0, 0), false);
	runForSeconds(0.8);

	setButton(buttonIndex(1, 0), true); // Enter, to confirm
	runForSeconds(0.4);
	setButton(buttonIndex(1, 0), false);

	// Give the firmware time to rewrite its patch and timbre memory before anything else
	// touches it.
	runForSeconds(4.0);
	// The whole memory has just been rebuilt from the preset ROM, so tell the engine about
	// all of it rather than waiting for a diff to notice piecemeal.
	resyncMirror();
}

void D110CoreNative::releaseAllButtons() {
	for (int i = 0; i < kNumButtons; ++i)
		if (buttonMask_ & (1u << i)) bus_.setButton(i % kNumBits, false, i >= kNumBits);
	buttonMask_ = 0;
}

bool D110CoreNative::getRam(u8 *out) const {
	if (!running_) return false;
	std::memcpy(out, bus_.rams.data(), size_t(kRamSize));
	return true;
}

void D110CoreNative::pokeRamForTest(size_t offset, u8 value) {
	if (offset < bus_.rams.size()) bus_.rams[offset] = value;
}

bool D110CoreNative::getLcd(u8 *out) const {
	if (!running_) return false;
	const uint8_t *rendered = const_cast<D110Bus &>(bus_).lcd.render();
	for (int line = 0; line < kLines; ++line) {
		const int first = (line == 0) ? kRenderLine0 : kRenderLine1;
		for (int col = 0; col < kCols; ++col) {
			const uint8_t *src = rendered + size_t(first + col) * kRenderStride;
			uint8_t *dst = out + (size_t(line) * kCols + col) * kRowsPerChar;
			for (int row = 0; row < kRowsPerChar; ++row) dst[row] = src[row] & 0x1f;
		}
	}
	return true;
}
