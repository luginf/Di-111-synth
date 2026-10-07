// "In the demo song the patches do not match" - but the demo song is self-contained (the
// firmware plays itself, the tones are baked into a separate chip ic15/ic12), and it may
// be entirely unrelated to the patches from battery memory (0x0000, which have no real
// factory names - only FACTORY PRESET cards, which we do not have, supply them). The only
// way to learn where the demo takes what it plays and shows is to see whether it touches
// Patch Memory (0x0000-0x1FFF) and Timbre Memory (0x2994-0x33BF) at all while playing.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

std::vector<uint8_t> g_cgrom;

bool isCgrom(const juce::MemoryBlock &data) {
	if (data.getSize() != 4096) return false;
	const auto *p = static_cast<const uint8_t *>(data.getData());
	static const uint8_t kA[7] = {0x0e, 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11};
	for (int r = 0; r < 7; ++r)
		if ((p[16 * 0x41 + r] & 0x1f) != kA[r]) return false;
	return true;
}

void loadCgrom() {
	const auto dir = D110AudioProcessor::getAutoRomFolder();
	for (const auto &entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles)) {
		const auto f = entry.getFile();
		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && isCgrom(data)) {
			g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
			               static_cast<const uint8_t *>(data.getData()) + 4096);
			return;
		}
	}
}

char decodeCell(const uint8_t *rows) {
	if (g_cgrom.empty()) return '?';
	for (int code = 0x20; code < 0x80; ++code) {
		bool same = true;
		for (int r = 0; r < 7; ++r)
			if ((g_cgrom[(size_t)16 * code + r] & 0x1f) != (rows[r] & 0x1f)) { same = false; break; }
		if (same) return char(code);
	}
	return '?';
}

std::string lcdLine(D110AudioProcessor &proc, int line) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int col = 0; col < D110Core::kCols; ++col)
		s.push_back(decodeCell(rows + ((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar));
	return s;
}

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
	}
}

void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	render(proc, hold / 1000.0);
	for (int i : idx) proc.getCore().setButton(i, false);
	render(proc, settle / 1000.0);
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	std::vector<uint8_t> before(D110Core::kRamSize, 0);
	proc.getCore().getRam(before.data());

	std::printf("запускаю встроенную демо-песню (Edit+Enter, потом Enter)...\n");
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);

	std::printf("играю 20 секунд...\n");
	for (int t = 0; t < 20; ++t) {
		render(proc, 1.0);
		std::printf("  %2ds  строка 1: [%s]  строка 2: [%s]\n", t + 1,
		            lcdLine(proc, 0).c_str(), lcdLine(proc, 1).c_str());
	}

	std::vector<uint8_t> after(D110Core::kRamSize, 0);
	proc.getCore().getRam(after.data());

	int patchChanges = 0, timbreMemChanges = 0, timbreTempChanges = 0, toneMemChanges = 0;
	for (int i = D110Core::kRamPatches; i < D110Core::kRamPatches + 64 * D110Core::kPatchRecord; ++i)
		if (before[(size_t)i] != after[(size_t)i]) ++patchChanges;
	for (int i = D110Core::kRamTimbres; i < D110Core::kRamTimbres + 128 * D110Core::kTimbreRecord; ++i)
		if (before[(size_t)i] != after[(size_t)i]) ++timbreMemChanges;
	for (int i = D110Core::kRamTimbreTemp; i < D110Core::kRamTimbreTemp + 9 * D110Core::kTimbreTempRecord; ++i)
		if (before[(size_t)i] != after[(size_t)i]) ++timbreTempChanges;
	for (int i = D110Core::kRamTones; i < D110Core::kRamTones + 64 * D110Core::kToneMemRecord; ++i)
		if (before[(size_t)i] != after[(size_t)i]) ++toneMemChanges;

	std::printf("\n=== что демо тронула за 20 секунд ===\n");
	std::printf("  Patch Memory (0x0000, ваши 64 патча):        %d изменённых байт\n", patchChanges);
	std::printf("  Timbre Memory (0x2994, ваши 128 тембров):    %d изменённых байт\n", timbreMemChanges);
	std::printf("  Timbre Temporary (0x2000, живая область):    %d изменённых байт\n", timbreTempChanges);
	std::printf("  Tone Memory (0x4000, ваши 64 тона):          %d изменённых байт\n", toneMemChanges);
	std::printf("\nтекущий патч (0x2DB9) до/после: %d / %d\n",
	            int(before[(size_t)D110Core::kRamPatchNumber]),
	            int(after[(size_t)D110Core::kRamPatchNumber]));

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
