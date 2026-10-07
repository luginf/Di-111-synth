// Does an edit from the extended editor reach the instrument, the sound, and back?
//
// The box puts nothing into the firmware memory or the sound engine: it sends the
// instrument a sysex message for one parameter - exactly what an external
// editor would send over MIDI. From there the already-verified path works: the firmware changes its memory,
// the mirror carries the change into the engine. Both halves of this chain are checked here.
//
// There are three checks, and each is built so that it can show a failure:
//
//   1. EVERY processor sender puts its byte at the measured place in firmware memory.
//      The byte is read first, then a KNOWINGLY DIFFERENT one is set - otherwise "matched" means
//      nothing, since the parameter might already have been there.
//   2. The edit is audible: part 1's volume drops from a hundred to ten, and the same
//      chord is taken. The control is the same pair of measurements WITHOUT an edit between them.
//   3. Switching to a patch with the panel buttons brings the instrument to the requested number, not to a
//      neighbouring one: both "forward across a bank boundary" and the reverse move are checked.
//
// Everything the probe changed is undone at the end by a factory reset: the instrument's memory is shared with
// the plugin, and leaving our marks in it is not allowed.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

int g_passed = 0, g_failed = 0;

void check(bool ok, const char *what, const juce::String &detail) {
	std::printf("  [%s] %s   %s\n", ok ? " OK " : "FAIL", what, detail.toRawUTF8());
	if (ok) ++g_passed; else ++g_failed;
}

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int i = 0; i < blocks; ++i) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

int byteAt(D110AudioProcessor &proc, int offset) {
	const auto ram = snapshot(proc);
	return (offset >= 0 && offset < D110Core::kRamSize) ? int(ram[(size_t)offset]) : -1;
}

// A value knowingly different from the current one and not exceeding the parameter's limit.
uint8_t differentFrom(int current, int hi) {
	const int candidate = (current == hi) ? hi - 1 : current + 1;
	return uint8_t(juce::jlimit(0, hi, candidate));
}

double chordRms(D110AudioProcessor &proc, int channel) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer on;
	for (int note : { 48, 52, 55 }) on.addEvent(juce::MidiMessage::noteOn(channel, note, 0.9f), 0);
	double sumSq = 0.0;
	int64_t samples = 0;
	const int blocks = int(1.5 * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer midi;
		if (b == 0) midi = on;
		proc.processBlock(buffer, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
		if (b < blocks / 5) continue;   // the attack does not count: we measure the steady-state sound
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
			const float *d = buffer.getReadPointer(ch);
			for (int i = 0; i < buffer.getNumSamples(); ++i) {
				sumSq += double(d[i]) * double(d[i]);
				++samples;
			}
		}
	}
	juce::MidiBuffer off;
	for (int note : { 48, 52, 55 }) off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
	buffer.clear();
	proc.processBlock(buffer, off);
	render(proc, 0.8);
	return samples > 0 ? std::sqrt(sumSq / double(samples)) : 0.0;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   ПЗУ: %s\n\n", proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.isSynthReady() ? "загружены" : "НЕТ");
	if (!proc.getCore().isRunning()) return 1;

	// --- 1. each sender puts its byte where it should ---------------------
	std::printf("=== 1. КАЖДАЯ ОБЛАСТЬ, ЧЕРЕЗ ОТПРАВИТЕЛИ ПРОЦЕССОРА ===\n");
	{
		struct Case {
			const char *what;
			int ramOffset;
			int hi;
			std::function<void(uint8_t)> send;
		};
		const Case kCases[] = {
			{ "Timbre Temporary, партия 3, Output Level",
			  D110Core::kRamTimbreTemp + 2 * D110Core::kTimbreTempRecord + 8, 100,
			  [&proc](uint8_t v) { proc.sendTimbreTempParam(2, 8, v); } },
			{ "Tone Temporary, партия 2, TVF Cutoff партиала 1",
			  D110Core::kRamToneTemp + D110Core::kToneRecord + 14 + 23, 100,
			  [&proc](uint8_t v) { proc.sendToneTempParam(1, 14 + 23, v); } },
			{ "Rhythm Setup, запись 17, Output Level",
			  D110Core::kRamRhythmTemp + 16 * D110Core::kRhythmRecord + 1, 100,
			  [&proc](uint8_t v) { proc.sendRhythmParam(16, 1, v); } },
			// The partial reserve does NOT fit here, and that is a property of the instrument: its nine values
			// must sum to exactly 32, so a single increment is rejected, and
			// the sender check would fail where the sender is not at fault.
			{ "System, Reverb Time",
			  D110Core::kRamSystem + 2, 7,
			  [&proc](uint8_t v) { proc.sendSystemParam(2, v); } },
			{ "Timbre Memory, ячейка 6, Key Shift",
			  D110Core::kRamTimbres + 5 * D110Core::kTimbreRecord + 2, 48,
			  [&proc](uint8_t v) { proc.sendTimbreMemoryParam(5, 2, v); } },
			{ "Patch Memory, патч 4, Reverb Level",
			  D110Core::kRamPatches + 3 * D110Core::kPatchRecord + 12, 7,
			  [&proc](uint8_t v) { proc.sendPatchMemoryParam(3, 12, v); } },
		};

		for (const Case &c : kCases) {
			const int before = byteAt(proc, c.ramOffset);
			const uint8_t wanted = differentFrom(before, c.hi);
			c.send(wanted);
			render(proc, 1.0);
			const int after = byteAt(proc, c.ramOffset);
			check(after == int(wanted), c.what,
			      "0x" + juce::String::toHexString(c.ramOffset) + ": " + juce::String(before)
			          + " -> " + juce::String(after) + ", хотели " + juce::String(int(wanted)));
		}

		// The name - ten bytes at once, along the same path.
		proc.sendName(D110Core::kSysexToneTemp, 0, "EditorTest");
		render(proc, 1.2);
		const auto ram = snapshot(proc);
		juce::String read;
		for (int i = 0; i < 10; ++i) read += char(ram[(size_t)D110Core::kRamToneTemp + (size_t)i]);
		check(read == "EditorTest", "Tone Temporary, партия 1, имя", "прочитано \"" + read + "\"");
	}

	// --- 2. is it audible --------------------------------------------------
	//
	// The control is mandatory: "got quieter" proves nothing by itself, because the second
	// measurement differs from the first also by being the second. So two measurements
	// WITHOUT an edit between them are taken first, and only then one with an edit.
	std::printf("\n=== 2. ДОХОДИТ ЛИ ПРАВКА ДО ЗВУКА ===\n");
	{
		proc.sendTimbreTempParam(0, 8, 100);   // part 1 at full volume
		render(proc, 1.0);
		const double a = chordRms(proc, 2);    // part 1 answers on channel 2
		const double control = chordRms(proc, 2);
		proc.sendTimbreTempParam(0, 8, 10);    // and the same part at ten
		render(proc, 1.0);
		const double quiet = chordRms(proc, 2);

		const double controlDb = 20.0 * std::log10(juce::jmax(1e-9, control) / juce::jmax(1e-9, a));
		const double quietDb = 20.0 * std::log10(juce::jmax(1e-9, quiet) / juce::jmax(1e-9, a));
		std::printf("  RMS: %.6f -> контроль %.6f (%.1f дБ) -> после правки %.6f (%.1f дБ)\n",
		            a, control, controlDb, quiet, quietDb);
		check(std::abs(controlDb) < 1.5, "контроль: без правки громкость не меняется",
		      juce::String(controlDb, 2) + " дБ");
		check(quietDb < -6.0, "Output Level 100 -> 10 слышен",
		      juce::String(quietDb, 2) + " дБ");
		proc.sendTimbreTempParam(0, 8, 100);
		render(proc, 0.5);
	}

	// --- 3. switching to a patch with the panel buttons -------------------------------------
	//
	// The timer that presses the buttons lives on the message queue, and in a console program nobody
	// runs it - so here it is run explicitly. In the plugin the window's host does this,
	// and nothing needs to be started.
	std::printf("\n=== 3. ПЕРЕХОД НА ПАТЧ КНОПКАМИ ПАНЕЛИ ===\n");
	{
		auto pump = [&proc](int ms) {
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
			while (std::chrono::steady_clock::now() < until) {
				juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
				juce::AudioBuffer<float> block(2, kBlock);
				juce::MidiBuffer none;
				block.clear();
				proc.processBlock(block, none);
			}
		};

		// Forward across a bank boundary, backward within a bank and exactly to the start - three different paths
		// through the same arithmetic.
		for (int target : { 27, 3, 0 }) {
			proc.selectPatch(target);
			pump(4000);
			const int now = byteAt(proc, D110Core::kRamPatchNumber);
			check(now == target, "переход на патч",
			      "просили I-" + juce::String(target / 8 + 1) + juce::String(target % 8 + 1)
			          + ", прибор стоит на I-" + juce::String(now / 8 + 1)
			          + juce::String(now % 8 + 1));
		}
	}

	// --- 4. the edit is audible IMMEDIATELY ------------------------------------------------
	//
	// Two things that are arranged on the instrument differently from what the hand expects. A tone in memory does not sound
	// by itself - the part's temporary area sounds, and the tone has to be put there. A patch in memory
	// does not sound either - the instrument plays from temporary areas, which a patch reaches only on
	// selection. The editor does both transfers itself; here we check that it really does them.
	std::printf("\n=== 4. ПОДСТАНОВКА: ТОН И ПАТЧ СЛЫШНЫ СРАЗУ ===\n");
	{
		// A tone from a memory cell - into part 3.
		const auto ram = snapshot(proc);
		const size_t slotAt = size_t(D110Core::kRamTones) + 4 * D110Core::kToneMemRecord;
		juce::String wanted;
		for (int i = 0; i < 10; ++i) {
			const char ch = char(ram[slotAt + size_t(i)]);
			wanted += (ch >= 32 && ch < 127) ? ch : ' ';
		}
		proc.auditionTone(2, 4);
		render(proc, 1.6);
		const auto after = snapshot(proc);
		juce::String got;
		for (int i = 0; i < 10; ++i) {
			const char ch = char(after[size_t(D110Core::kRamToneTemp)
			                          + 2 * D110Core::kToneRecord + size_t(i)]);
			got += (ch >= 32 && ch < 127) ? ch : ' ';
		}
		// An empty cell proves nothing: if the tone memory is not filled, there is nothing
		// to compare, and that must be said, not passed off as success.
		if (wanted.trim().isEmpty())
			std::printf("  [ -- ] ячейка памяти тонов пуста, проверять нечего\n");
		else
			check(got == wanted, "тон из памяти встал в партию 3",
			      "ячейка \"" + wanted + "\", в партии \"" + got + "\"");

		// A field of the patch the instrument plays: it must change BOTH in memory AND in the live area.
		const int current = proc.currentPatchNumber();
		if (current < 0) {
			std::printf("  [FAIL] номер текущего патча не прочитан\n");
			++g_failed;
		} else {
			constexpr int kPart = 1;
			const int field = 31 + kPart * 12 + 8;   // Output Level of the second part
			const int storedAt = D110Core::kRamPatches + current * D110Core::kPatchRecord + field;
			const int liveAt = D110Core::kRamTimbreTemp + kPart * D110Core::kTimbreTempRecord + 8;
			const uint8_t v = differentFrom(byteAt(proc, liveAt), 100);
			proc.editPatchField(current, field, v);
			render(proc, 1.4);
			check(byteAt(proc, storedAt) == int(v), "правка легла в память патча",
			      "0x" + juce::String::toHexString(storedAt) + " = "
			          + juce::String(byteAt(proc, storedAt)));
			check(byteAt(proc, liveAt) == int(v), "и в живую область, то есть слышна",
			      "0x" + juce::String::toHexString(liveAt) + " = "
			          + juce::String(byteAt(proc, liveAt)));
		}

		// CONTROL: for a DIFFERENT patch the live area must not move - otherwise the editor would change
		// the sound where it was not asked to.
		if (current >= 0) {
			const int other = (current + 1) % D110Core::kNumPatches;
			constexpr int kPart = 3;
			const int field = 31 + kPart * 12 + 8;
			const int liveAt = D110Core::kRamTimbreTemp + kPart * D110Core::kTimbreTempRecord + 8;
			const int before = byteAt(proc, liveAt);
			proc.editPatchField(other, field, differentFrom(before, 100));
			render(proc, 1.4);
			check(byteAt(proc, liveAt) == before,
			      "контроль: правка ЧУЖОГО патча звук не трогает",
			      "живой байт остался " + juce::String(byteAt(proc, liveAt)));
		}
	}

	// --- 5. the name in the box and the name on the display must match ------------------
	//
	// "Which tone a part plays" is a PAIR of bytes, group and number, not a single number. The box
	// shows the name by the pair from the patch record, the instrument - by the pair from the live area, and if
	// only one byte of the two is carried over, both sides stay with their own: below "Fantasy"
	// (b01), on the display "AcouPiano 1" (a01). The number agrees, the group does not.
	//
	// What is checked is precisely a divergence of pairs, not of one byte: first the groups are deliberately
	// made to differ, then the number is edited - with the same call the mouse wheel uses to edit it.
	std::printf("\n=== 5. ГРУППА И НОМЕР ТОНА ПЕРЕНОСЯТСЯ ВМЕСТЕ ===\n");
	{
		const int patch = proc.currentPatchNumber();
		if (patch < 0) {
			std::printf("  [FAIL] номер текущего патча не прочитан\n");
			++g_failed;
		} else {
			constexpr int kPart = 0;
			const int groupField = 31 + kPart * 12 + 0;
			const int numberField = 31 + kPart * 12 + 1;
			const size_t liveGroup = size_t(D110Core::kRamTimbreTemp)
			                       + size_t(kPart) * D110Core::kTimbreTempRecord;

			// We deliberately make them differ: in the patch record group b, in the live area group a.
			proc.sendPatchMemoryParam(patch, groupField, 1);
			proc.sendTimbreTempParam(kPart, 0, 0);
			render(proc, 1.4);
			std::printf("  разведено: в патче группа %d, в живой области группа %d\n",
			            byteAt(proc, D110Core::kRamPatches + patch * D110Core::kPatchRecord
			                             + groupField),
			            byteAt(proc, int(liveGroup)));

			// And now exactly what the wheel over the TONE field of the lower table does.
			proc.editPatchField(patch, numberField, 0);
			render(proc, 1.6);

			const int patchGroup = byteAt(proc, D110Core::kRamPatches
			                                        + patch * D110Core::kPatchRecord + groupField);
			const int patchNumber = byteAt(proc, D110Core::kRamPatches
			                                         + patch * D110Core::kPatchRecord + numberField);
			const int liveG = byteAt(proc, int(liveGroup));
			const int liveN = byteAt(proc, int(liveGroup) + 1);
			std::printf("  после правки номера: патч (%d,%d), живая область (%d,%d)\n",
			            patchGroup, patchNumber, liveG, liveN);
			check(liveG == patchGroup && liveN == patchNumber,
			      "живая область повторяет пару из патча",
			      "иначе ящик и индикатор называют разные тона");
		}
	}

	std::printf("\n=== ИТОГ: %d прошло, %d не прошло ===\n", g_passed, g_failed);

	// Clean up after ourselves: the probe wrote into the instrument's real battery-backed memory.
	std::printf("\nзаводской сброс...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 8.0);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return g_failed == 0 ? 0 : 1;
}
