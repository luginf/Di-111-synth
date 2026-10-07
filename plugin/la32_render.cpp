// First bridge from REGISTERS to SOUND: take the state that the real firmware put into the
// LA32 and run it through a model of the same chip.
//
// The model already exists - munt's `LA32FloatWaveGenerator`. What it lacks is feeding it
// state from a live firmware: munt feeds it what its own synthesizer computed, which replaces
// the firmware. Here it is the other way round - what the firmware wrote to the registers is
// taken and decoded according to docs/la32_register_map.md.
//
// The check is not by ear. For note 60 the frequency is known in advance, and it is measured
// by the zero crossings of the signal. If the register decoding is right, the fundamental must
// match; if the pitch scale is misunderstood it will diverge, and by how much will be visible
// too. It also resolves the 4111 versus 4096 per octave discrepancy recorded in the document
// as unexplained.
//
// Only the SYNTHETIC partial so far: PCM needs decoding of the wave address and length in the
// ROM, which is not done yet.
#include "Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "LA32FloatWaveGenerator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kChipRate = D110Core::kLa32SampleRate;
// The generator expects not the level but its LOGARITHMIC COMPLEMENT - see the detailed comment
// at the first use below.
constexpr MT32Emu::Bit32u kAmpFull = 67117056;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

void pressButton(D110AudioProcessor &proc, const char *name, int times = 1) {
	for (const auto &b : kButtons)
		if (std::strcmp(b.name, name) == 0) {
			const int idx = D110Core::buttonIndex(b.port, b.bit);
			for (int i = 0; i < times; ++i) {
				proc.getCore().setButton(idx, true);
				render(proc, 0.13);
				proc.getCore().setButton(idx, false);
				render(proc, 0.30);
			}
			return;
		}
}

// State of one voice, assembled from the register writes.
struct Voice {
	bool seen = false;
	bool isPcm = false;      // 0x0D00 even byte, bit 7
	bool sawtooth = false;   // the same, bit 6
	uint8_t resonance = 0;   // 0x0D00 odd byte, low 5 bits, minus one
	uint8_t pulseWidth = 0;  // 0x0C40 even byte (SYNTH) - unused for a PCM partial
	uint8_t cutoff = 0;      // 0x0C40 odd byte (SYNTH) - for PCM this is the wave pos, see below
	uint8_t wavePos = 0;     // 0x0C40 odd byte (PCM) - see below
	uint8_t ampTarget = 0;   // ramp bank selected by the flag, odd byte
	uint16_t pitch = 0;      // 0x0CC0, sixteen bits
	// 0x0D00 even byte, bit 5. Matched munt's Partial::isRingModulatingNoMix() on all
	// eleven reachable structures and both slots - 22 points without exception
	// (docs/la32_register_map.md). For the master partial it is set only at mix 2, for the
	// slave - at mix 1 and mix 2, i.e. everywhere ring modulation runs without mixing in the
	// master.
	bool ringNoMix = false;
	// The same, bit 6, for a PCM partial: it is cleared exactly for the slave in ring modulation.
	// This is precisely munt's pcmWaveInterpolated condition, which says that for such a partial
	// the interpolation multiplier is taken by the ring modulator. For a synthetic partial the same
	// bit carries the sawtooth - there is no way to separate these two meanings on the current
	// data, neither is refuted.
	bool pcmInterpolated = true;
	// 0x0D00 odd byte for a PCM partial: the high bits are the wave length and loop (see below),
	// the low ones are resonance, the same as for a synthetic one.
	bool pcmLoop = false;
	uint32_t pcmLen = 0;
};

// The wave table from the presets ROM is not read here at all - and that is not an omission.
// The register analysis (docs/la32_register_map.md, "PCM: wave address and length are read
// straight from the registers") found an exact match of register 0x0C40.x.1 with this table's
// `pos` byte for three different consecutive pcmWave values - but the table itself is needed
// only by the FIRMWARE, to compute the address; the chip does not know the table at all, it
// receives a ready address and length. So the render below takes the address and length
// straight from the registers, as the real LA32 does.

// Decoding of the raw wave ROM bytes into the same logarithmic samples that
// Synth::loadPCMROM builds in munt - the same file, the same interleaved unpacking across the two
// chips, repeated here one to one, because the plugin gives no public access to the already
// loaded synthesis data, and the bytes themselves are the same ones it loads through the
// same file.
std::vector<MT32Emu::Bit16s> decodePcmRom(const juce::File &waveRom) {
	juce::MemoryBlock d;
	std::vector<MT32Emu::Bit16s> out;
	if (!waveRom.loadFileAsData(d)) return out;
	const auto *p = static_cast<const uint8_t *>(d.getData());
	const size_t n = d.getSize() / 2;
	out.resize(n);
	static const int order[16] = {0, 9, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 8};
	for (size_t i = 0; i < n; ++i) {
		const uint8_t s = p[2 * i], c = p[2 * i + 1];
		int16_t log = 0;
		for (int u = 0; u < 16; ++u) {
			const int bit = order[u] < 8 ? ((s >> (7 - order[u])) & 1)
			                             : ((c >> (7 - (order[u] - 8))) & 1);
			log = int16_t(log | (bit << (15 - u)));
		}
		out[i] = log;
	}
	return out;
}

// Note frequency at the tuning the D-110 shows on its screen.
double noteHz(int note, double masterTuneHz) {
	return masterTuneHz * std::pow(2.0, (note - 69) / 12.0);
}

// Fundamental by upward zero crossings, on a settled stretch. The method is crude, but for
// the question "is it the right octave and the right semitone" it is more than enough, and
// it gives the fine fraction by the mean period, not by a single one.
double fundamentalHz(const std::vector<float> &x, double sr) {
	size_t first = 0, last = 0;
	int crossings = 0;
	for (size_t i = 1; i < x.size(); ++i) {
		if (!(x[i - 1] <= 0.0f && x[i] > 0.0f)) continue;
		if (!crossings++) first = i;
		last = i;
	}
	if (crossings < 3) return 0.0;
	return sr * double(crossings - 1) / double(last - first);
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const int note = argc > 1 ? std::atoi(argv[1]) : 60;
	// Velocity is a controlled input: it moves the ramp TARGET, i.e. the level. The loudness law
	// must be checked by the SLOPE, not by a single peak: the absolute peak also depends on the
	// waveform, whereas the ratio of two peaks for a known level difference does not.
	const int velocity = argc > 3 ? std::atoi(argv[3]) : 100;
	// By how many steps to shift the pair 1&2 structure from the factory one. Needed to check the
	// ring modulation path: the factory timbre has structure 2, i.e. plain addition, and the ring
	// modulator does not work on it at all - so there is nothing to claim about it.
	const int structureSteps = argc > 4 ? std::atoi(argv[4]) : 0;
	const juce::File outDir = argc > 2 ? juce::File(juce::String(argv[2]))
	                                   : juce::File::getCurrentWorkingDirectory();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	if (!proc.getCore().isRunning()) { std::printf("прошивка не поднялась\n"); return 1; }

	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Ramps);
	proc.getCore().setLa32PresetFf(true);
	proc.getCore().setLa32StatusMode(1);
	proc.getCore().setTraceFilter(D110Core::kLa32TapBase, D110Core::kLa32TapEnd);

	std::printf("заводской сброс...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);

	if (structureSteps) {
		pressButton(proc, "Exit", 3);
		pressButton(proc, "Timbre");
		pressButton(proc, "Edit");
		pressButton(proc, "Edit");
		pressButton(proc, "Group+", 1); // common part, pair 1&2 structure page
		pressButton(proc, "Number+", structureSteps);
		render(proc, 0.4);
		pressButton(proc, "Exit", 3);
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		std::printf("структура пары 1&2: %d\n", ram[0x21E4 + 10]);
	}

	// One note, a short window: the values the firmware put in at voice issue are needed.
	// The note is held and released INSIDE the capture window: envelope steps arrive both on
	// press and on release, and without the second half there is nothing to check in the ramp.
	proc.getCore().startSoTrace();
	const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)};
	proc.getCore().pushMidi(on, 3);
	render(proc, 1.0);
	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	render(proc, 1.2);
	proc.getCore().stopSoTrace();
	const auto writes = proc.getCore().takeSoWrites();

	// Decoding. The FIRST value of each register is taken: a leading 0xFF is a preset, not
	// data, but here it sits in the even bytes, which do not interest us in this part.
	Voice v[D110Core::kNumHardwareVoices];
	int bankOf[D110Core::kNumHardwareVoices] = {};
	std::map<uint16_t, uint8_t> firstValue;
	for (const auto &w : writes)
		if (!firstValue.count(w.addr)) firstValue[w.addr] = w.value;

	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const auto flag = firstValue.find(uint16_t(0x0D00 + 2 * s));
		if (flag == firstValue.end()) continue;
		v[s].seen = true;
		v[s].isPcm = (flag->second & 0x80) != 0;
		v[s].sawtooth = (flag->second & 0x40) != 0;
		v[s].pcmInterpolated = (flag->second & 0x40) != 0;
		v[s].ringNoMix = (flag->second & 0x20) != 0;
		bankOf[s] = v[s].isPcm ? 0x0C00 : 0x0C80;
		const auto res = firstValue.find(uint16_t(0x0D01 + 2 * s));
		if (res != firstValue.end()) {
			v[s].resonance = uint8_t(res->second & 0x1F);
			// Measured by exact match (docs/la32_register_map.md): for a tone with pcmWave changed
			// 61->81 register 0D00.x.1 gave 18->88, and the ROM entry itself has len 0x10 and
			// 0x80. 0x18 = 0x10|0x08, 0x88 = 0x80|0x08 - the high bits are exactly the len byte,
			// the low ones are untouched by the wave edit. So length and loop sit in the same high
			// bits that are empty for a synthetic partial.
			v[s].pcmLoop = (res->second & 0x80) != 0;
			v[s].pcmLen = 0x800u << ((res->second & 0x70) >> 4);
		}
		const auto pw = firstValue.find(uint16_t(0x0C40 + 2 * s));
		if (pw != firstValue.end()) v[s].pulseWidth = pw->second;
		const auto co = firstValue.find(uint16_t(0x0C41 + 2 * s));
		// The same odd byte of bank 0x0C40 carries DIFFERENT things depending on the kind of
		// partial: cutoff for a synthetic one, the wave pos byte for PCM. Measured by exact match
		// with the wave table (docs/la32_register_map.md): for tones 61 and 64 the register gave BA and C0
		// - exactly the same as pos of entries 61 and 64 in the table itself, without a single mismatch.
		if (co != firstValue.end()) {
			if (v[s].isPcm) v[s].wavePos = co->second;
			else v[s].cutoff = co->second;
		}
		const auto amp = firstValue.find(uint16_t(bankOf[s] + 2 * s + 1));
		if (amp != firstValue.end()) v[s].ampTarget = amp->second;
		const auto pl = firstValue.find(uint16_t(0x0CC0 + 2 * s));
		const auto ph = firstValue.find(uint16_t(0x0CC1 + 2 * s));
		// A leading 0xFF in the pitch stream is a reset, not data: the first NON-0xFF is taken.
		uint8_t lo = 0, hi = 0;
		for (const auto &w : writes) {
			if (w.addr == uint16_t(0x0CC0 + 2 * s) && w.value != 0xFF && !lo) lo = w.value;
			if (w.addr == uint16_t(0x0CC1 + 2 * s) && w.value != 0xFF && !hi) hi = w.value;
		}
		(void)pl; (void)ph;
		v[s].pitch = uint16_t((hi << 8) | lo);
	}

	std::printf("\n  слот | род      | пила/адрес | ширина/длина | срез | резонанс | цикл |"
	            " уровень | высота\n");
	int synthSlot = -1, pcmSlot = -1;
	// The note's slots in issue order: the first of the pair is the master partial, the second the slave.
	std::vector<int> pairSlots;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		if (!v[s].seen) continue;
		if (v[s].isPcm)
			std::printf("  %4d | %-8s | %010X | %12u | %4s | %8d | %-4s | %7d | %04X\n", s,
			            "PCM", unsigned(v[s].wavePos) << 11, v[s].pcmLen, "-", v[s].resonance,
			            v[s].pcmLoop ? "да" : "нет", v[s].ampTarget, v[s].pitch);
		else
			std::printf("  %4d | %-8s | %-10s | %12d | %4d | %8d | %-4s | %7d | %04X\n", s,
			            "синтез", v[s].sawtooth ? "пила" : "прямоуг", v[s].pulseWidth,
			            v[s].cutoff, v[s].resonance, "-", v[s].ampTarget, v[s].pitch);
		if (!v[s].isPcm && synthSlot < 0) synthSlot = s;
		if (v[s].isPcm && pcmSlot < 0) pcmSlot = s;
		pairSlots.push_back(s);
	}

	// The wave ROM - the same bytes the plugin autoload takes from the same MAME files
	// (waveIc8+waveIc7), assembled in the same order (PluginProcessor.cpp:
	// pcmRomPath = "assembled from MAME chip dumps: wave IC8 + IC7").
	const auto romDir = D110AudioProcessor::getAutoRomFolder();
	std::vector<MT32Emu::Bit16s> pcmRom;
	{
		juce::MemoryBlock ic7, ic8;
		for (const auto &e : juce::RangedDirectoryIterator(romDir, false, "*", juce::File::findFiles)) {
			const auto name = e.getFile().getFileName().toLowerCase();
			if (name.contains("r15179878")) e.getFile().loadFileAsData(ic8); // wave IC8, goes first
			if (name.contains("r15179880")) e.getFile().loadFileAsData(ic7); // wave IC7, goes second
		}
		if (ic7.getSize() && ic8.getSize()) {
			juce::MemoryBlock joined(ic8);
			joined.append(ic7.getData(), ic7.getSize());
			const juce::File tmp = outDir.getChildFile("_pcm_rom.bin");
			tmp.replaceWithData(joined.getData(), joined.getSize());
			pcmRom = decodePcmRom(tmp);
			tmp.deleteFile();
		}
	}
	std::printf("\n  волновое ПЗУ: %s\n", pcmRom.empty() ? "НЕ НАЙДЕНО" :
	            (juce::String(pcmRom.size()) + " отсчётов").toRawUTF8());

	// --- a PAIR of partials as ONE voice ---------------------------------------------------
	// On the D-110 a note takes two slots not because it sounds twice but because the voice
	// consists of a pair of partials, and the chip itself decides whether to add them or multiply
	// them with the ring modulator. While they were rendered separately, this was not a voice but
	// its halves.
	//
	// Which is master and which is slave is decided by slot order: the firmware issues them in a
	// row, and the first of the pair is the master. The mix mode is taken from bit 5, found by
	// stepping through structures.
	if (int(pairSlots.size()) == 2) {
		const Voice &m = v[pairSlots[0]], &sv = v[pairSlots[1]];
		// ringModulated - whether ring modulation runs at all; mixed - whether the master partial is
		// mixed into its output. Per munt: init(hasRingModulatingSlave(), mixType == 1), and
		// mixType == 1 is exactly "slave in the ring, master mixed in" - i.e. bit 5 is set for the
		// slave and not for the master.
		const bool ringModulated = sv.ringNoMix;
		const bool mixed = sv.ringNoMix && !m.ringNoMix;
		std::printf("\n  === голос как пара: ведущий слот %d, ведомый слот %d ===\n",
		            pairSlots[0], pairSlots[1]);
		std::printf("  кольцевая модуляция: %s, ведущий подмешан: %s\n",
		            ringModulated ? "да" : "нет", mixed ? "да" : "нет");

		bool ok = true;
		MT32Emu::LA32FloatPartialPair pair;
		pair.init(ringModulated, mixed);
		const MT32Emu::LA32PartialPair::PairType kRoles[2] = {
			MT32Emu::LA32PartialPair::MASTER, MT32Emu::LA32PartialPair::SLAVE};
		for (int i = 0; i < 2 && ok; ++i) {
			const Voice &pv = v[pairSlots[(size_t)i]];
			if (!pv.isPcm) {
				pair.initSynth(kRoles[i], pv.sawtooth, pv.pulseWidth,
				               uint8_t(pv.resonance ? pv.resonance : 1));
				continue;
			}
			const uint32_t addr = uint32_t(pv.wavePos) << 11;
			if (pcmRom.empty() || addr + pv.pcmLen > pcmRom.size()) {
				std::printf("  PCM-волна по адресу %06X недоступна - пара пропущена\n", addr);
				ok = false;
				break;
			}
			std::printf("  слот %d: волна %06X, длина %u, цикл %s, интерполяция %s\n",
			            pairSlots[(size_t)i], addr, pv.pcmLen, pv.pcmLoop ? "да" : "нет",
			            pv.pcmInterpolated ? "да" : "нет");
			pair.initPCM(kRoles[i], pcmRom.data() + addr, pv.pcmLen, pv.pcmLoop);
		}

		if (ok) {
			const int nPair = int(kChipRate * 0.4);
			std::vector<float> outPair((size_t)nPair, 0.0f);
			float peakPair = 0.0f;
			for (int i = 0; i < nPair; ++i) {
				for (int p = 0; p < 2; ++p) {
					const Voice &pv = v[pairSlots[(size_t)p]];
					const MT32Emu::Bit32u a = kAmpFull - (MT32Emu::Bit32u(pv.ampTarget) << 18);
					const MT32Emu::Bit32u c = pv.isPcm ? (240u << 18)
					                                   : (MT32Emu::Bit32u(pv.cutoff) << 18);
					pair.generateNextSample(kRoles[p], a, pv.pitch, c);
				}
				outPair[(size_t)i] = pair.nextOutSample();
				peakPair = std::max(peakPair, std::abs(outPair[(size_t)i]));
			}
			// Printed with extra digits on purpose: ring modulation MULTIPLIES two signals, and the
			// product of two quiet partials looks like zero at four digits although it is not zero. Once
			// it was almost read as "does not work".
			std::printf("  пик пары: %.8f\n", double(peakPair));
			const juce::File wavPair =
				outDir.getChildFile("la32_pair_note" + juce::String(note) + ".wav");
			juce::AudioBuffer<float> bufPair(1, nPair);
			std::memcpy(bufPair.getWritePointer(0), outPair.data(), sizeof(float) * (size_t)nPair);
			wavPair.deleteFile();
			juce::WavAudioFormat fmtPair;
			std::unique_ptr<juce::FileOutputStream> stPair(wavPair.createOutputStream());
			if (stPair != nullptr) {
				std::unique_ptr<juce::AudioFormatWriter> wrPair(
					fmtPair.createWriterFor(stPair.get(), kChipRate, 1, 16, {}, 0));
				if (wrPair != nullptr) {
					stPair.release();
					wrPair->writeFromAudioSampleBuffer(bufPair, 0, nPair);
				}
			}
			std::printf("  записано: %s\n", wavPair.getFullPathName().toRawUTF8());
		}
	}

	if (synthSlot < 0) { std::printf("\nсинтетического партиала в этой ноте нет\n"); return 0; }

	// Run through the chip model. Amplitude and cutoff are held constant: pitch and waveform
	// decoding is checked here, and the envelopes have already been checked separately.
	const Voice &vv = v[synthSlot];
	MT32Emu::LA32FloatWaveGenerator wg;
	wg.initSynth(vv.sawtooth, vv.pulseWidth, uint8_t(vv.resonance ? vv.resonance : 1));
	// The generator expects not the level but its LOGARITHMIC COMPLEMENT: munt's Partial.cpp has
	// `ampRampVal = 67117056 - ampRamp.nextValue()`, and then `amp = 2^(-ampVal/2^22)`,
	// i.e. a bigger number means quieter. The register, however, carries the LEVEL, and feeding it
	// directly would turn loudness inside out: full level 255 would give silence.
	// One level step comes out as 2^(1/16), i.e. about 0.376 dB.
	const MT32Emu::Bit32u amp = kAmpFull - (MT32Emu::Bit32u(vv.ampTarget) << 18);
	const MT32Emu::Bit32u cutoff = MT32Emu::Bit32u(vv.cutoff) << 18;

	const int samples = int(kChipRate * 0.5);
	std::vector<float> out((size_t)samples, 0.0f);
	for (int i = 0; i < samples; ++i)
		out[(size_t)i] = wg.generateNextSample(amp, vv.pitch, cutoff);

	// --- the same, but with a LIVE envelope -------------------------------------------------
	// Above, the amplitude was held constant to check pitch and loudness separately.
	// Here the render is fed what the firmware actually told the chip to do over time: the ramp
	// steps as they arrive. The law of motion is the same as in D110Core.
	{
		struct Ev { double ms; uint8_t inc, target; };
		std::vector<Ev> events;
		uint8_t pendingInc = 0;
		const uint16_t evenAddr = uint16_t(bankOf[synthSlot] + 2 * synthSlot);
		for (const auto &w : writes) {
			if (w.addr == evenAddr) pendingInc = w.value;
			else if (w.addr == uint16_t(evenAddr + 1)) events.push_back({w.ms, pendingInc, w.value});
		}
		const double t0 = events.empty() ? 0.0 : events.front().ms;

		MT32Emu::LA32FloatWaveGenerator wg2;
		wg2.initSynth(vv.sawtooth, vv.pulseWidth, uint8_t(vv.resonance ? vv.resonance : 1));
		const int n2 = int(kChipRate * 2.5);
		std::vector<float> out2((size_t)n2, 0.0f);
		double current = 0.0, increment = 0.0, target = 0.0;
		bool descending = false, running = false;
		size_t next = 0;
		double landedAtMs = -1.0;
		for (int i = 0; i < n2; ++i) {
			const double ms = 1000.0 * i / kChipRate;
			while (next < events.size() && events[next].ms - t0 <= ms) {
				const Ev &e = events[next++];
				target = double(e.target) * double(1 << 18);
				if (e.inc == 0) { running = false; continue; }
				if (e.inc == 0xFF) { current = target; running = false; continue; }
				const double large = std::pow(2.0, (double(e.inc & 0x7F) + 24.0) / 8.0);
				descending = (e.inc & 0x80) != 0;
				increment = descending ? large + 1.0 : large;
				running = !((descending && current <= target) || (!descending && current >= target));
				if (!running) current = target;
			}
			if (running) {
				current += descending ? -increment : increment;
				if (descending ? current <= target : current >= target) {
					current = target;
					running = false;
					if (landedAtMs < 0.0) landedAtMs = ms;
				}
			}
			const MT32Emu::Bit32u a =
				kAmpFull - MT32Emu::Bit32u(std::min(current, double(kAmpFull)));
			out2[(size_t)i] = wg2.generateNextSample(a, vv.pitch, cutoff);
		}

		// The envelope is printed from the envelope of the SOUND, not from an internal counter:
		// otherwise it would be checking that a variable equals itself.
		std::printf("\n  огибающая рендера (пик по 25 мс, дБ от максимума):\n   ");
		double top = 0.0;
		std::vector<double> env;
		for (int b = 0; b + int(kChipRate / 40) <= n2; b += int(kChipRate / 40)) {
			double p = 0.0;
			for (int i = b; i < b + int(kChipRate / 40); ++i) p = std::max(p, std::abs(double(out2[(size_t)i])));
			env.push_back(p);
			top = std::max(top, p);
		}
		for (size_t i = 0; i < env.size(); i += 4)
			std::printf(" %.0f:%.0f", 25.0 * double(i), top > 0 ? 20.0 * std::log10(std::max(env[i], 1e-9) / top) : 0.0);
		std::printf("\n  ступеней рампы за ноту: %zu", events.size());
		for (const auto &e : events)
			std::printf(" | %.0f мс: цель %d, приращение %02X", e.ms - t0, e.target, e.inc);
		std::printf("\n");

		const juce::File wav2 = outDir.getChildFile("la32_env_note" + juce::String(note) + ".wav");
		juce::AudioBuffer<float> buf2(1, n2);
		std::memcpy(buf2.getWritePointer(0), out2.data(), sizeof(float) * (size_t)n2);
		wav2.deleteFile();
		juce::WavAudioFormat fmt2;
		std::unique_ptr<juce::FileOutputStream> st2(wav2.createOutputStream());
		if (st2 != nullptr) {
			std::unique_ptr<juce::AudioFormatWriter> w2(
				fmt2.createWriterFor(st2.get(), kChipRate, 1, 16, {}, 0));
			if (w2 != nullptr) { st2.release(); w2->writeFromAudioSampleBuffer(buf2, 0, n2); }
		}
		std::printf("  с огибающей: %s\n", wav2.getFullPathName().toRawUTF8());
	}

	// The settled stretch is measured, without the first milliseconds.
	const std::vector<float> steady(out.begin() + samples / 4, out.end());
	const double got = fundamentalHz(steady, kChipRate);
	const double want440 = noteHz(note, 440.0), want442 = noteHz(note, 442.0);
	float peak = 0.0f;
	for (float x : steady) peak = std::max(peak, std::abs(x));

	std::printf("\n  нота %d, слот %d (синтетический)\n", note, synthSlot);
	std::printf("  основной тон:      %8.2f Гц\n", got);
	std::printf("  ожидается при 440: %8.2f Гц   при 442: %8.2f Гц\n", want440, want442);
	if (got > 0.0) {
		const double cents = 1200.0 * std::log2(got / want442);
		std::printf("  расхождение с 442: %+8.1f цента   (октав: %+.2f)\n", cents, cents / 1200.0);
	}
	// The peak is checked not "by eye" but against what the loudness law itself predicts:
	// each level unit is 2^(1/16). If the level decoding is right, prediction and measurement
	// must agree to within the waveform (whose peak is always below full scale).
	const double predicted = std::pow(2.0, -double(kAmpFull - (MT32Emu::Bit32u(vv.ampTarget) << 18))
	                                            / 4194304.0);
	std::printf("  пик: %.4f   потолок по уровню %d: %.4f   ниже потолка на %.1f дБ\n",
	            double(peak), vv.ampTarget, predicted,
	            peak > 0.0f ? 20.0 * std::log10(predicted / double(peak)) : 0.0);

	const juce::File wav = outDir.getChildFile("la32_note" + juce::String(note) + ".wav");
	{
		juce::AudioBuffer<float> buf(1, samples);
		std::memcpy(buf.getWritePointer(0), out.data(), sizeof(float) * (size_t)samples);
		wav.deleteFile();
		juce::WavAudioFormat fmt;
		std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
		if (stream != nullptr) {
			std::unique_ptr<juce::AudioFormatWriter> writer(
				fmt.createWriterFor(stream.get(), kChipRate, 1, 16, {}, 0));
			if (writer != nullptr) {
				stream.release(); // the writer owns it now
				writer->writeFromAudioSampleBuffer(buf, 0, samples);
			}
		}
	}
	std::printf("  записано: %s\n", wav.getFullPathName().toRawUTF8());

	proc.setPoweredOn(false);
	return 0;
}
