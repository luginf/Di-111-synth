// A snapshot of the extended editor to a file, without a window and without a mouse.
//
// Needed for the same reason everything in this project is measured by picture rather than by eye (see
// plugin/panel_render.cpp, which captures the memory card's travel): to look at the result before
// the user sees it. A REAL component is drawn - the very D110EditorPane
// that sits in the plugin - with live values from the memory of a running unit, not
// some mock-up.
//
// The tab is chosen by the second argument; "all" captures all nine in a row.
#include "Source/PluginEditor.h"
#include "Source/PluginProcessor.h"
#include "Source/UiTheme.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

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

bool writeShot(juce::Component &c, const juce::File &out) {
	const juce::Image shot = c.createComponentSnapshot(c.getLocalBounds(), false, 1.0f);
	juce::PNGImageFormat png;
	out.deleteFile();
	std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
	if (stream == nullptr || !png.writeImageToStream(shot, *stream)) {
		std::printf("не удалось записать %s\n", out.getFullPathName().toRawUTF8());
		return false;
	}
	// The stream is closed EXPLICITLY, before everything else: otherwise the file is finished being written at exit
	// from main, and the reading side manages to see a truncated image.
	stream->flush();
	stream.reset();
	std::printf("снимок: %s (%d x %d, %d байт)\n", out.getFullPathName().toRawUTF8(),
	            shot.getWidth(), shot.getHeight(), int(out.getSize()));
	return true;
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::String which = (argc > 1) ? argv[1] : "all";
	const bool playing = (argc > 2) && juce::String(argv[2]) == "playing";

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   ПЗУ: %s\n", proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.isSynthReady() ? "загружены" : "НЕТ");

	// Window width and drawer height - the same as in the assembled editor, so the snapshot
	// shows what the user will see, not a separately chosen size. An optional
	// third argument overrides it - handy for checking how the LCD indicator behaves at the very
	// lower bound of the constrainer (900), not only at the default window.
	const int width = (argc > 3) ? std::atoi(argv[3]) : 1500;
	const float scale = float(width) / float(D110Panel::kRefW);
	// D110_SHOT_HEIGHT overrides the pane height, to check the short-window layouts (scrollbars).
	const int height = std::getenv("D110_SHOT_HEIGHT") != nullptr
	                       ? std::atoi(std::getenv("D110_SHOT_HEIGHT"))
	                       : int(D110AudioProcessorEditor::kPaneRefH * scale + 0.5f);

	// Optional 5th argument, "light" - so the THEME toggle (Utility tab) can be checked
	// headlessly the same way every other bit of this UI is, instead of just by eye.
	const bool light = (argc > 4) && juce::String(argv[4]) == "light";
	proc.setUiThemeLight(light);
	d110ui::setTheme(light ? d110ui::Theme::Light : d110ui::Theme::Dark);

	// The monitor only makes sense to look at on a sounding unit: otherwise it always shows "all thirty-two
	// voices free". The chord is played on channel 2 - that is part 1 on a factory D-110.
	if (playing) {
		juce::AudioBuffer<float> audio(2, kBlock);
		juce::MidiBuffer on;
		for (int n : { 48, 52, 55 }) on.addEvent(juce::MidiMessage::noteOn(2, n, 0.9f), 0);
		for (int b = 0; b < 80; ++b) {
			audio.clear();
			juce::MidiBuffer midi;
			if (b == 0) midi = on;
			proc.processBlock(audio, midi);
			std::this_thread::sleep_for(std::chrono::milliseconds(11));
		}
	}

	static const char *kTabs[] = { "parts", "tone", "rhythm", "patches", "timbres",
	                               "tones", "system", "monitor", "soundbanks", "utility" };
	constexpr int kNumTabs = int(sizeof(kTabs) / sizeof(kTabs[0]));

	// Optional 5th argument: LCD colour scheme index (0 = green, see the right-click menu "LCD").
	if (argc > 5) proc.setLcdColor(std::atoi(argv[5]));

	D110EditorPane pane(proc);
	pane.setBounds(0, 0, width, height);
	pane.resized();

	int ok = 0, wanted = 0;
	for (int i = 0; i < kNumTabs; ++i) {
		if (which != "all" && which != kTabs[i]) continue;
		++wanted;
		pane.selectTab(i);
		// The component's timer does not run here - there is no message queue - so the unit's
		// memory is fetched manually, by the same refresh as in normal operation.
		pane.refreshFromInstrument();
		const juce::File out = juce::File::getCurrentWorkingDirectory()
			.getChildFile(juce::String("editor_") + kTabs[i] + ".png");
		if (writeShot(pane, out)) ++ok;
	}

	// And the whole editor at once - unit, handle strip and extended drawer - to show
	// how it looks together.
	if (which == "all" || which == "whole") {
		std::unique_ptr<D110AudioProcessorEditor> whole(
			dynamic_cast<D110AudioProcessorEditor *>(proc.createEditor()));
		if (whole != nullptr) {
			// The drawer is opened right here: in the plugin it slides out on click over a third of a
			// second, while the snapshot has to show the final position.
			whole->setExpanded(true);
			// Also opened here, purely so the snapshot shows what it looks like open - in
			// normal use this drawer defaults to closed, unlike the keyboard below.
			whole->setSequencerExpanded(true);
			// The test keyboard is open by default, so its own handle band and strip
			// (D110Keyboard::kRefH) count towards the window height same as the drawer's -
			// and the sequencer drawer, forced open just above, the same way again.
			whole->setSize(width, int((float(D110Panel::kRefH)
			                           + D110AudioProcessorEditor::kHandleRefH
			                           + D110AudioProcessorEditor::kPaneRefH
			                           + D110AudioProcessorEditor::kKeyboardHandleRefH
			                           + D110Keyboard::kRefH
			                           + D110AudioProcessorEditor::kSequencerHandleRefH
			                           + D110SequencerPanel::kRefH) * scale + 0.5f));
			whole->resized();
			// Both halves pick up the unit's state with their own timers, and there is no message queue
			// here. Without this the snapshot would come out with the indicator dark and the caption
			// "switch the unit on" on a unit that is actually running.
			whole->refreshFromInstrument();
			++wanted;
			if (writeShot(*whole, juce::File::getCurrentWorkingDirectory()
			                          .getChildFile("editor_whole.png")))
				++ok;
		}
	}

	std::printf("снято %d из %d\n", ok, wanted);
	proc.setPoweredOn(false);
	proc.releaseResources();
	return ok == wanted ? 0 : 1;
}
