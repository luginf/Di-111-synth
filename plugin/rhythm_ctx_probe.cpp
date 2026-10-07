// closed_hat (key 42, timbre 64) sounds 10-50 times quieter than its neighbours, and next to it in the
// engine log the warning "Attempted to play invalid key 1 (velocity 121)" appears -
// which is nowhere else in the whole run. The velocity match (121 = 0.95*127, exactly what
// our own probe sends) says that OUR bridge produced the extra event, not
// munt by itself.
//
// mt32emu::RhythmPart::noteOn gets ev.note directly from D110Core::popNoteEvent(), and that from
// m_ctxNote[ctx], which the firmware itself writes into f400[] (rams 0x3400+ctx). If "1" really
// lands there - the firmware writes it, and the question "why" moves to its side;
// if not - the substitution happens in OUR bridge. The only way to separate these two cases is
// to listen to the write itself, exactly as la32_ctx_probe.cpp does for melodic parts.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <map>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kRhythmChannel = 10;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (b == 0 && midi) ? *midi : none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

struct Decoded { const char *array; int index; };
Decoded decode(uint16_t addr) {
	if (addr >= 0x2DC0 && addr < 0x2E00) return {"edc0", (addr - 0x2DC0) / 2};
	if (addr >= 0x2E00 && addr < 0x2E40) return {"ee00", (addr - 0x2E00) / 2};
	if (addr >= 0x2E40 && addr < 0x2E80) return {"ee40", (addr - 0x2E40)};
	if (addr >= 0x2E80 && addr < 0x2EC0) return {"ee80", (addr - 0x2E80) / 2};
	if (addr >= 0x2EC0 && addr < 0x2F00) return {"eec0", (addr - 0x2EC0) / 2};
	if (addr >= 0x2F80 && addr < 0x2FC0) return {"ef80", (addr - 0x2F80) / 2};
	if (addr >= 0x33A0 && addr < 0x33C0) return {"f3a0", addr - 0x33A0}; // part (part*16)
	if (addr >= 0x33C0 && addr < 0x33E0) return {"f3c0", addr - 0x33C0};
	if (addr >= 0x3400 && addr < 0x3420) return {"f400", addr - 0x3400}; // note
	if (addr >= 0x3420 && addr < 0x3440) return {"f420", addr - 0x3420}; // velocity
	if (addr >= 0x3440 && addr < 0x3460) return {"f440", addr - 0x3440};
	if (addr >= 0x3460 && addr < 0x3480) return {"f460", addr - 0x3460}; // release
	if (addr >= 0x3480 && addr < 0x34a0) return {"f480", addr - 0x3480};
	return {"?", -1};
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().setVoiceCtxTap(true);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	proc.getCore().takeCtxEvents(); // clear the load noise

	for (int note : {42, 46, 90, 35}) { // hi-hats + one certainly sounding (kick) for control
		proc.getCore().takeCtxEvents();
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kRhythmChannel, note, 0.95f), 0);
		render(proc, 0.4, &on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kRhythmChannel, note), 0);
		render(proc, 0.4, &off);

		const auto events = proc.getCore().takeCtxEvents();
		std::printf("\n=== клавиша %d, %d событий ===\n", note, int(events.size()));
		for (const auto &e : events) {
			const Decoded d = decode(e.addr);
			if (d.index < 0) continue;
			std::printf("  PC %04X  %s[%d]  = %d (0x%02X)\n", e.pc, d.array, d.index, e.value,
			            e.value);
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
