// Minimal address decode for the native D-110 core, Phase 1 scope only: enough to boot the
// real firmware ROM on Mcs96Cpu and prove the CPU interpreter itself is correct via a PC-trace
// comparison against the MAME-backed core. LCD/panel/SO and everything else this phase doesn't
// need are no-op stubs (writes dropped, reads return 0) - see plugin/Source/native/README-ish
// comment in mcs96_cpu.h and the project plan for what phases 2+ add here.
//
// Address map transcribed from roland_d10.cpp's d110_map()/d110_bank_map() (the ACTUAL
// map the d110() machine config installs - not d10_map/d10_bank_map, which are the D-10's,
// laid out differently):
//   0x0100          W  bank register (selects which 16KB chunk of the 20-bit space appears
//                      at 0x8000-0xBFFF; stride 0x4000, per ADDRESS_MAP_BANK config)
//   0x1000-0x7FFF   R  firmware ROM, ALWAYS bytes [offset+0x1000] regardless of current bank
//                      (the driver reads this through the bank device's own address space at
//                      an absolute physical address, bypassing the bank register entirely)
//   0x8000-0xBFFF   RW banked window: physical = bankReg*0x4000 + (addr-0x8000), decoded as
//                        0x00000-0x0FFFF firmware ROM
//                        0x40000-0x47FFF rams (32KB, zero-init here - no persistence yet)
//                        0x80000-0x9FFFF presets ROM (128KB)
//                        0xC0000-0xC7FFF memcs (32KB, zero stub - no card logic yet)
//   0xC000-0xFFFF   RW fixed window into rams[0x0000-0x3FFF] (low 16KB only - the upper half
//                      of rams is only reachable through the banked window)
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "msm6222b.h"

class D110Bus {
public:
	using u8 = uint8_t;
	using u16 = uint16_t;
	using u32 = uint32_t;

	// firmware: 0x10000 bytes (only the low 0x8000 is a real ROM dump per roland_d10.cpp's
	// ROM_REGION size - the rest reads as zero, matching an unpopulated ROM region).
	// presets: 0x20000 bytes.
	void setFirmwareRom(const uint8_t *data, size_t size);
	void setPresetsRom(const uint8_t *data, size_t size);
	void setLcdCgrom(const uint8_t *data) { lcd.setCgrom(data); }

	u8 read(u16 addr);
	void write(u16 addr, u8 data);

	std::vector<u8> rams = std::vector<u8>(0x8000, 0);   // battery-backed RAM, 32KB
	std::vector<u8> memcs = std::vector<u8>(0x8000, 0);  // memory-card window, 32KB (stub)

	Msm6222b lcd;

	// Active-low, matching roland_d10.cpp's INPUT_PORTS_START(d110): bit clear = held.
	// Default 0xff = nothing pressed. setButton() below flips the right bit.
	u8 sc0 = 0xff, sc1 = 0xff;
	void setButton(int bit, bool down, bool isSc1) {
		u8 &reg = isSc1 ? sc1 : sc0;
		if (down) reg &= ~(1 << bit); else reg |= (1 << bit);
	}

	// Memory-card window (0xC0000-0xC7FFF, 32KB) - the real driver hands this whole range
	// to plain RAM and doesn't model a card slot at all, so D110Core restores it with a
	// READ-only intercept: writes always land in memcs unmodified, but a read sees
	// kCardAbsentByte throughout when no card is "inserted", or the write-protect status
	// byte at the last offset, exactly matching D110Core::resolveDevices()'s m_cardTap.
	static constexpr int kCardStatusOffset = 0x7fff;
	static constexpr u8 kCardAbsentByte = 0xff;
	// The memory card starts seated in its slot (Alan, 2026-10-02): the panel draws it there, and a unit that has
	// never been touched does not have a card lying on a table. Ejecting it is an explicit act of the user.
	bool cardInserted = true;
	bool cardWriteProtect = false;
	u8 cardStatusByte() const { return cardWriteProtect ? 0xfe : 0xff; }

	// Voice-context write tap: fires with the RAMS OFFSET (not the absolute CPU address -
	// same convention D110Core::noteWatch() uses) whenever the firmware writes anywhere in
	// f3a0[]..f460[] (note/velocity/part/release tables for the 32 hardware voice contexts),
	// which is how D110CoreNative learns what the firmware itself thinks is sounding. Matches
	// D110Core's kVoiceCtxTapBase/kVoiceCtxTapSpan (0xF3A0, 0x160) exactly - both addresses
	// land in the fixed rams window (0xC000-0xFFFF -> rams[addr-0xC000]), so this is a plain
	// address-range check on top of the normal rams write, not a separate memory region.
	static constexpr u16 kVoiceCtxTapBase = 0xf3a0;
	static constexpr u16 kVoiceCtxTapSpan = 0x160;
	std::function<void(u16 ramsOffset, u8 value)> onVoiceCtxWrite;

	// The sound board's status register (0x0C00/0x0C01) - unmapped in the real driver too
	// (MAME has no LA32 emulation at all), restored the same way D110Core does: a read
	// intercept that hands back la32Status once la32Pending is set, and 0 (open bus)
	// otherwise. See D110CoreNative's StuckPolicy::La32Stub port for what sets these.
	bool la32Pending = false;
	u8 la32Status = 0;

	// LA32 ramp registers (0x0C00-0x0DFF: filter ramp at 0x0C00-0x0C3F, amp ramp at
	// 0x0C80-0x0CBF, per-slot bank-select flags at 0x0D00-0x0D3F) - a write-only range the
	// real driver has no memory backing either, restored the same way as the status byte:
	// D110CoreNative owns the actual ramp math (StuckPolicy::La32Ramps) and just needs to
	// see every write in this range, regardless of which StuckPolicy is active (matches
	// D110Core's own m_la32WriteTap, installed unconditionally).
	static constexpr u16 kLa32RegBase = 0x0c00;
	static constexpr u16 kLa32RegEnd = 0x0dff; // inclusive
	std::function<void(u16 addr, u8 value)> onLa32RegWrite;

private:
	std::vector<u8> firmware_ = std::vector<u8>(0x10000, 0);
	std::vector<u8> presets_ = std::vector<u8>(0x20000, 0);
	u8 bankReg_ = 0;
	u8 swScan_ = 0;

	// roland_d10_state's own lcd_data_w()/lcd_ctrl_w() quirk: data writes only ever land in
	// this buffer; a control write flushes the whole buffer through the LCD's data_w() AFTER
	// applying the control byte itself. Not a real MSM6222B behaviour - a driver-level
	// buffering scheme this port has to replicate to stay bit-for-bit with real hardware.
	u8 lcdDataBuffer_[256] = {};
	int lcdDataBufferPos_ = 0;

	u8 bankedRead(u32 physical);
	void bankedWrite(u32 physical, u8 data);
};
