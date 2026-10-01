// Снимок расширенного редактора в файл, без окна и без мыши.
//
// Нужен затем же, зачем в этом проекте всё меряют по картинке, а не на глаз (см.
// plugin/panel_render.cpp, снимающий ход карты памяти): посмотреть на результат до того,
// как его увидит пользователь. Рисуется НАСТОЯЩИЙ компонент - тот самый D110EditorPane,
// который стоит в плагине, - с живыми значениями из памяти работающего прибора, а не
// какой-нибудь макет.
//
// Вкладка выбирается вторым доводом; "all" снимает все девять подряд.
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
	// Поток закрывается ЯВНО, до всего остального: иначе файл дописывается уже при выходе
	// из main, и читающая сторона успевает увидеть обрезанную картинку.
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

	// Ширина окна и высота ящика - те же, что в собранном редакторе, поэтому снимок и
	// показывает то, что увидит пользователь, а не отдельно подобранный размер. Необязательный
	// третий довод переопределяет её - удобно проверить, как ЖК-индикатор ведёт себя у самой
	// нижней границы constrainer'а (900), а не только у окна по умолчанию.
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

	// Монитор имеет смысл смотреть на звучащем приборе: иначе на нём всегда «свободны все
	// тридцать два голоса». Аккорд берётся на канале 2 - это партия 1 у заводского D-110.
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

	D110EditorPane pane(proc);
	pane.setBounds(0, 0, width, height);
	pane.resized();

	int ok = 0, wanted = 0;
	for (int i = 0; i < kNumTabs; ++i) {
		if (which != "all" && which != kTabs[i]) continue;
		++wanted;
		pane.selectTab(i);
		// Таймер компонента здесь не крутится - очереди сообщений нет, - поэтому память
		// прибора берётся вручную, тем же самым обновлением, что и в работе.
		pane.refreshFromInstrument();
		const juce::File out = juce::File::getCurrentWorkingDirectory()
			.getChildFile(juce::String("editor_") + kTabs[i] + ".png");
		if (writeShot(pane, out)) ++ok;
	}

	// И весь редактор целиком - прибор, полоса-ручка и выехавший ящик, - чтобы было видно,
	// как это выглядит вместе.
	if (which == "all" || which == "whole") {
		std::unique_ptr<D110AudioProcessorEditor> whole(
			dynamic_cast<D110AudioProcessorEditor *>(proc.createEditor()));
		if (whole != nullptr) {
			// Ящик открывается прямо здесь: в плагине он выезжает по щелчку за треть
			// секунды, а снимку показывать надо конечное положение.
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
			// Обе половины забирают состояние прибора своими таймерами, а очереди сообщений
			// здесь нет. Без этого снимок вышел бы с погашенным индикатором и надписью
			// «включите прибор» на приборе, который на самом деле работает.
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
