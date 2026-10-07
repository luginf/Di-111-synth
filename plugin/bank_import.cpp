// Loading a tone bank into the unit's real battery-backed memory - and confirming that it
// really landed there.
//
// Needed because the D-110's internal tone memory is EMPTY from the factory and is not filled by a factory reset
// (verified: after a reset 0x4000 still has zero non-zero bytes out of 16384).
// So after a crash that zeroed the NVRAM there is nothing to restore the bank from: nvram_recovery.cpp repairs the
// system area and patches, but cannot restore tones - they were never there.
//
// It goes THE SAME WAY as the plugin: D110AudioProcessor::importSysexBank() puts
// the messages in a queue, and processBlock delivers them both to the sound engine and to the control board
// through core.pushMidi(). There is deliberately no SysEx parsing of its own here - otherwise the
// tool would be testing itself, not the plugin.
//
// The file path comes from the argument and ONLY from it. Folder names in the collection contain a
// non-breaking hyphen (U+2011), which does not survive passing through the system encoding,
// so the file must first be copied to a path of ordinary characters and that path given.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Waiting must be by the clock and processBlock must be run: the import queue is drained by
// it and nobody else. Counting iterations here would give a false "did not arrive" - forty thousand turns
// pass in seconds, while the cable delivers its 3125 bytes per second and not a byte faster.
void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

// How many non-zero bytes are in tone memory. The measure is crude and chosen on purpose: it depends on not a
// single guess about the record layout, so it tells "zero" from "not zero" honestly.
int toneBytes(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int n = 0;
	for (int i = D110Core::kRamTones; i < D110Core::kRamSize; ++i)
		if (ram[(size_t)i] != 0) ++n;
	return n;
}

void printNames(D110AudioProcessor &proc, int count) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return;
	for (int t = 0; t < count; ++t) {
		std::printf("  тон %2d: '", t + 1);
		for (int i = 0; i < 10; ++i)
			std::printf("%c", ram[(size_t)D110Core::kRamTones + (size_t)t * 256 + (size_t)i]);
		std::printf("'\n");
	}
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::printf("укажите файл банка: d110_bank_import <путь к .syx>\n");
		return 2;
	}
	const juce::File bank(juce::String::fromUTF8(argv[1]));
	if (!bank.existsAsFile()) {
		std::printf("файл не найден: %s\n", argv[1]);
		return 2;
	}

	std::printf("банк: %s (%lld байт)\n", bank.getFullPathName().toRawUTF8(),
	            (long long)bank.getSize());
	std::printf("папка НВР (та же, что у установленного плагина): %s\n",
	            D110AudioProcessor::getNvramRoot().getFullPathName().toRawUTF8());

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	std::printf("ПЗУ прибора: %s\n", proc.isSynthReady() ? "загружены" : "НЕ НАЙДЕНЫ");
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	const int before = toneBytes(proc);
	std::printf("\nдо заливки: ненулевых байт в памяти тонов %d из %d\n",
	            before, D110Core::kRamSize - D110Core::kRamTones);

	proc.importSysexBank(bank);
	std::printf("%s\n", proc.getLastImportMessage().toRawUTF8());

	// With a margin over the computed cable time: the queue is drained one block at a time, and the firmware
	// still needs time to lay out what it received into its banks. The result below is what is actually
	// measured, not this deadline.
	std::printf("\nотдаю по кабелю на скорости MIDI...\n");
	render(proc, 25.0);

	const int after = toneBytes(proc);
	std::printf("\nпосле заливки: ненулевых байт в памяти тонов %d из %d\n",
	            after, D110Core::kRamSize - D110Core::kRamTones);
	if (after > 0) printNames(proc, 6);

	// Power-off is the only moment when MAME writes the NVRAM to disk. Without it everything
	// loaded would remain only in the process's memory.
	std::printf("\nвыключаю (это и есть момент записи на диск)...\n");
	proc.setPoweredOn(false);
	proc.releaseResources();

	// Independent check: the file is re-read from disk by our OWN means, not through the plugin.
	// Otherwise the confirmation would be the same code that just wrote it - and a zeroing
	// that happened during the write would go unnoticed exactly as it did last time.
	const juce::File rams = D110AudioProcessor::getNvramRoot().getChildFile("d110").getChildFile("rams");
	juce::MemoryBlock raw;
	if (!rams.loadFileAsData(raw) || raw.getSize() < (size_t)D110Core::kRamSize) {
		std::printf("файл НВР не прочитался: %s\n", rams.getFullPathName().toRawUTF8());
		return 1;
	}
	const auto *p = static_cast<const uint8_t *>(raw.getData());
	int onDisk = 0;
	for (int i = D110Core::kRamTones; i < D110Core::kRamSize; ++i)
		if (p[i] != 0) ++onDisk;
	std::printf("\nв файле на диске: ненулевых байт в памяти тонов %d из %d  %s\n",
	            onDisk, D110Core::kRamSize - D110Core::kRamTones,
	            onDisk > 0 ? "*** банк на месте ***" : "ПУСТО - на диск не легло");
	return onDisk > 0 ? 0 : 1;
}
