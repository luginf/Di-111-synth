// Does the documented factory reset procedure ("POWER off, Ctrl+click WRITE/COPY,
// POWER on, ENTER") reach the firmware when performed with REAL clicks on the real panel -
// rather than by calling D110Core::setButton() bypassing it, as coldstart_test.cpp does.
//
// The owner reported this procedure never worked for them, not once. Code analysis
// found the cause: D110Panel::setButtonState() before this fix returned immediately while
// `!core.isRunning()` - which is exactly when the procedure asks to latch a button. The Ctrl-
// click was swallowed by the early return, core.setButton() was never called, and only the
// panel's local flag latched, meaning nothing to the real scan matrix. The unit powered on
// "clean" and never saw the button held.
//
// This bench presses EXACTLY the same five steps from the README - with real mouse events on the real
// D110Panel::mouseDown/mouseUp, with the Ctrl modifier where a latch is needed - and checks the
// result the same way as plugin/nvram_recovery.cpp: the partial reserve must
// add up to 32, not stay what it was.
#include "Source/PluginEditor.h"
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Button coordinates - the same constants as in the panel itself (see PluginEditor.cpp).
constexpr float kWriteCopyX = 1473.0f + 63.0f / 2.0f, kWriteCopyY = 80.0f + 26.0f / 2.0f;
constexpr float kEnterX = 1473.0f + 63.0f / 2.0f, kEnterY = 168.0f + 26.0f / 2.0f;
constexpr float kPowerX = 1913.0f + 85.0f / 2.0f, kPowerY = 96.0f + 106.0f / 2.0f;

void renderBlocks(D110AudioProcessor &proc, int blocks) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer none;
		proc.processBlock(audio, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
}

// A real mouse event on the real panel - same trick as in panel_render.cpp.
// `holdMs` - how much REAL time passes between press and release: with an instantaneous
// click (0) it is the same millisecond, and the machine runs on ITS OWN thread and may simply
// not manage to turn the scan loop even once in between. factoryReset() holds Enter 400 ms -
// the same is needed here for a fair comparison.
void click(D110Panel &panel, juce::Point<float> p, juce::ModifierKeys mods, int holdMs = 0) {
	const juce::MouseEvent down(juce::Desktop::getInstance().getMainMouseSource(), p, mods, 1.0f,
	                            0.0f, 0.0f, 0.0f, 0.0f, &panel, &panel,
	                            juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(),
	                            1, false);
	panel.mouseDown(down);
	if (holdMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
	const juce::MouseEvent up(juce::Desktop::getInstance().getMainMouseSource(), p,
	                          juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &panel, &panel,
	                          juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1,
	                          false);
	panel.mouseUp(up);
}

int reserveSum(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int sum = 0;
	for (int i = 0; i < 9; ++i) sum += ram[0x2D98 + i];
	return sum;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);   // powers on by itself, per the new default
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	// Spoil it in an OBVIOUSLY RECOGNISABLE way, not a number that could match anyway: rename
	// patch 1. "Sum = 32" proves nothing if the reserve was already 32 (that is what happened with the first
	// attempt of this bench - the substitution into the system area did not take, and "after == 32"
	// was not proof but a coincidence). The name cannot come from anywhere except
	// a real run of the factory ROM.
	proc.sendName(D110Core::kSysexPatches, 0, "SABOTAGE!");
	render(proc, 1.5);
	{
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		std::printf("патч 1 переименован нарочно: \"%.10s\"\n",
		            reinterpret_cast<const char *>(ram.data()));
	}

	D110Panel panel(proc);
	panel.setSize(D110Panel::kRefW, D110Panel::kRefH);

	std::printf("\nвыполняю пять шагов из README настоящими нажатиями по панели...\n");

	std::printf("1. POWER off\n");
	click(panel, { kPowerX, kPowerY }, juce::ModifierKeys());
	render(proc, 1.0);
	std::printf("   isRunning = %s (ожидается: no)\n", proc.getCore().isRunning() ? "yes" : "no");

	std::printf("2. Ctrl+click WRITE/COPY (защёлкнуть, пока выключен)\n");
	click(panel, { kWriteCopyX, kWriteCopyY }, juce::ModifierKeys::ctrlModifier);

	std::printf("3. POWER on\n");
	click(panel, { kPowerX, kPowerY }, juce::ModifierKeys());
	render(proc, 9.0); // let the firmware come up and see the button held
	std::printf("   isRunning = %s\n", proc.getCore().isRunning() ? "yes" : "no");

	// ORDER SWAPPED relative to the README and to the first version of this bench.
	// D110Core::factoryReset() (a proven working implementation of the same procedure from the inside)
	// releases Write/Copy, waits 800 ms and ONLY THEN presses Enter - not the other way round.
	// The first run of this bench, in the literal README order (Enter, then release
	// Write/Copy), with an honest control (a distinguishable patch name) restored nothing - the name
	// stayed "SABOTAGE!". Here we check whether the order really is the missing
	// part, and not just the protection removed in setButtonState.
	std::printf("4. Ctrl+click WRITE/COPY (отпустить защёлку)\n");
	click(panel, { kWriteCopyX, kWriteCopyY }, juce::ModifierKeys::ctrlModifier);
	render(proc, 1.0);

	std::printf("5. click ENTER (подтвердить, держим 400 мс - как factoryReset())\n");
	click(panel, { kEnterX, kEnterY }, juce::ModifierKeys(), 400);
	render(proc, 6.0); // let the factory reset finish writing the banks

	const int after = reserveSum(proc);
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
	const std::string patchName(reinterpret_cast<const char *>(ram.data()), 10);
	std::printf("\nрезерв после процедуры: сумма = %d (заводская: 32)\n", after);
	std::printf("патч 1 после процедуры: \"%s\" (заводское: \"Patch   01\")\n",
	            patchName.c_str());

	const bool ok = (after == 32) && (patchName == "Patch   01");
	std::printf("\n%s\n", ok ? "*** ПРОЦЕДУРА С ПАНЕЛИ РАБОТАЕТ ***"
	                        : "*** ПРОЦЕДУРА С ПАНЕЛИ НЕ СРАБОТАЛА ***");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return ok ? 0 : 1;
}
