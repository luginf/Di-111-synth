// The live Timbre Temporary reads as a solid centre (pan 7 on all eight parts) while the
// patch memory area carries the correct factory values (4,10,6,8,2,12,0,14) - i.e. the
// right data sits in the patch but did not reach the sounding area. Tests one hypothesis:
// is it enough to SELECT the current patch again (like pressing Patch on the panel) for
// the firmware to carry its fields into Timbre Temporary itself - or is the problem deeper.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// The button presses in selectPatch() are driven by a juce::Timer, which needs a pumped
// message loop to fire - in a console program nobody pumps it, unlike in the plugin,
// where the window host does. Without pumping, the button queue stands still forever, and
// the first version of this probe never noticed that 0x2DB9 did not move even once.
void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
	}
}

void printPan(D110AudioProcessor &proc, const char *label) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
			// Group/number pairs of the live area - the tone each part actually plays right now,
			// not what is stored in the patch. If navigation really reaches the firmware, these
			// digits should change along with the patch number at 0x2DB9.
	std::printf("%s  (0x2DB9=%d)\n", label, int(ram[(size_t)D110Core::kRamPatchNumber]));
	std::printf("  живые тона партий (группа/номер): ");
	for (int part = 0; part < 8; ++part) {
		const int at = D110Core::kRamTimbreTemp + 16 * part;
		std::printf("%d/%d ", ram[(size_t)at], ram[(size_t)at + 1]);
	}
	std::printf("\n");
	for (int part = 0; part < 8; ++part) {
		const int patchAt = 31 + part * 12 + 9;
		const int liveAt = D110Core::kRamTimbreTemp + 16 * part + 9;
		std::printf("  партия %d: патч=%d  живая=%d\n", part + 1, ram[(size_t)patchAt],
		            ram[(size_t)liveAt]);
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	printPan(proc, "=== ДО ===");

	const int current = proc.currentPatchNumber();
			// If we ask for the same number that is already set, bankStep and numberStep come out
			// zero, and selectPatch presses only the patch select screen - NOT ONE Bank/Number
			// press, and copying the field into the live area apparently happens exactly on those.
			// So we first move to another patch - a real Bank/Number press - then back.
	const int away = (current == 0) ? 5 : 0;
	std::printf("\nтекущий патч: %d, ухожу на %d...\n", current, away);
	proc.selectPatch(away);
	render(proc, 4.0);
	printPan(proc, "\n=== НА ЧУЖОМ ПАТЧЕ ===");

	std::printf("\nвозвращаюсь на %d...\n", current);
	proc.selectPatch(current);
	render(proc, 4.0);
	printPan(proc, "\n=== ПОСЛЕ ВОЗВРАТА ===");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
