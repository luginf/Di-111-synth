// Captures the REAL plugin window to a PNG, so there is something to look at next to
// the photo of the unit, rather than judging it by constants. It draws the very
// D110AudioProcessorEditor the user sees - unlike d110_lcd_check,
// which repeats the drawing loop on its own and is therefore only good for tuning numbers.
//
// The whole window is captured, not just one panel, and here is why: the memory card no longer belongs
// to the panel. Before, it could only slide past the frame and vanish - the panel is as tall as the unit,
// 256 dots, and the card 370. Now there is a drawer under the unit, the card slides out ONTO it and stays
// fully visible there, so its travel can only be seen in the whole window.
//
// The card is moved not by a variable but by a click on the slot: this tests the whole path, from the
// mouse hit to the frame.
//
// Usage: d110_panel_render [output_dir]
#include "Source/PluginEditor.h"
#include "Source/PluginProcessor.h"

#include <cstdio>

namespace {

void save(juce::Component &c, const juce::File &out, juce::Rectangle<int> area, float scale) {
	const juce::Image shot = c.createComponentSnapshot(area, false, scale);
	juce::PNGImageFormat png;
	out.deleteFile();
	std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
	if (stream != nullptr) png.writeImageToStream(shot, *stream);
	std::printf("  %-24s %dx%d -> %s\n", out.getFileNameWithoutExtension().toRawUTF8(),
	            shot.getWidth(), shot.getHeight(), out.getFullPathName().toRawUTF8());
}

// Let the animation run: the card and the drawer are moved by their own timers, so a real
// message loop is needed, not a direct call of the handler.
void settle(int ms) {
	juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

// The panel is the window's first child, and the slot is caught by it: the slot itself is drawn on the unit.
D110Panel *panelOf(juce::Component &editor) {
	for (int i = 0; i < editor.getNumChildComponents(); ++i)
		if (auto *p = dynamic_cast<D110Panel *>(editor.getChildComponent(i)))
			return p;
	return nullptr;
}

void clickSlot(D110Panel &panel) {
	const juce::Point<float> p(D110Panel::kSlotHitX + D110Panel::kSlotHitW * 0.5f,
	                           D110Panel::kSlotHitY + D110Panel::kSlotHitH * 0.5f);
	const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), p,
	                         juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	                         &panel, &panel, juce::Time::getCurrentTime(), p,
	                         juce::Time::getCurrentTime(), 1, false);
	panel.mouseDown(e);
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::File dir = (argc > 1) ? juce::File(juce::String(argv[1]))
	                                  : juce::File::getCurrentWorkingDirectory();
	dir.createDirectory();
	std::printf("writing to %s\n", dir.getFullPathName().toRawUTF8());

	D110AudioProcessor proc;
	std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
	if (editor == nullptr) { std::printf("нет редактора\n"); return 1; }
	// Window width - the same as the plugin opens with.
	constexpr int kWidth = 1500;
	const float s = float(kWidth) / float(D110Panel::kRefW);
	editor->setSize(kWidth, int((float(D110Panel::kRefH)
	                             + D110AudioProcessorEditor::kHandleRefH) * s + 0.5f));
	settle(200);

	D110Panel *panel = panelOf(*editor);
	if (panel == nullptr) { std::printf("панель не найдена\n"); return 1; }

	// Close-up of the slot, which is also where the card will travel: from the unit down, onto the drawer.
	auto closeUp = [s] {
		return juce::Rectangle<float>(1520.0f * s, 90.0f * s, 400.0f * s, 660.0f * s)
			.toNearestInt();
	};

	save(*editor, dir.getChildFile("card_00_inserted.png"), closeUp(), 2.0f);
	save(*editor, dir.getChildFile("card_00_inserted_whole.png"), editor->getLocalBounds(), 1.0f);

	// Storyboard at equal intervals, not at three chosen points: it shows both what the
	// card looks like and how long the travel takes. A 100 ms step is roughly the rate at which
	// the eye can resolve motion. Ejecting opens the drawer itself, so the frames
	// show its travel as well.
	clickSlot(*panel);
	for (int i = 1; i <= 11; ++i) {
		settle(100);
		save(*editor, dir.getChildFile(juce::String::formatted("card_%02d_out_%dms.png", i, i * 100)),
		     closeUp(), 2.0f);
	}
	settle(600);
	save(*editor, dir.getChildFile("card_20_out_whole.png"), editor->getLocalBounds(), 1.0f);

	// --- dragging -------------------------------------------------------
	//
	// The ejected card can be grabbed with the left button and moved anywhere within the drawer.
	// Checked the same way as the slot: with real mouse events on the real
	// component, not a variable - and from the shift of its bounds you can see whether the card arrived.
	{
		D110MemoryCard *card = nullptr;
		for (int i = 0; i < editor->getNumChildComponents(); ++i)
			if (auto *c = dynamic_cast<D110MemoryCard *>(editor->getChildComponent(i))) card = c;

		if (card == nullptr) {
			std::printf("  !!! карта не найдена среди детей окна\n");
		} else {
			const auto before = card->getBounds();
			auto sendMouse = [&card](juce::Point<float> at, int what) {
				const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at,
				                         juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f,
				                         0.0f, 0.0f, 0.0f, card, card,
				                         juce::Time::getCurrentTime(), at,
				                         juce::Time::getCurrentTime(), 1, false);
				if (what == 0) card->mouseDown(e);
				else if (what == 1) card->mouseDrag(e);
				else card->mouseUp(e);
			};
			// Grabbed the middle of the card and dragged it left and down, frame by frame.
			const juce::Point<float> grab(float(card->getWidth()) * 0.5f,
			                              float(card->getHeight()) * 0.5f);
			sendMouse(grab, 0);
			for (int step = 1; step <= 8; ++step) {
				sendMouse(grab + juce::Point<float>(-90.0f * float(step), 22.0f * float(step)), 1);
				settle(40);
				if (step % 4 == 0)
					save(*editor, dir.getChildFile(juce::String::formatted("card_5%d_dragged.png",
					                                                       step / 4)),
					     editor->getLocalBounds(), 1.0f);
			}
			sendMouse(grab, 2);
			const auto after = card->getBounds();
			std::printf("  перетаскивание: %d,%d -> %d,%d (сдвиг %d,%d)\n", before.getX(),
			            before.getY(), after.getX(), after.getY(),
			            after.getX() - before.getX(), after.getY() - before.getY());
			// The card has no right to climb onto the unit: below the handle strip, and only there.
			const float s2 = float(kWidth) / float(D110Panel::kRefW);
			const int floorY = int((float(D110Panel::kRefH)
			                        + D110AudioProcessorEditor::kHandleRefH) * s2);
			std::printf("  карта ниже прибора: %s (верх %d, граница %d)\n",
			            after.getY() >= floorY ? "да" : "НЕТ", after.getY(), floorY);
		}
	}

	clickSlot(*panel);
	for (int i = 1; i <= 11; ++i) {
		settle(100);
		save(*editor, dir.getChildFile(juce::String::formatted("card_%02d_in_%dms.png", 20 + i, i * 100)),
		     closeUp(), 2.0f);
	}
	// The card must return exactly to the position it started from - otherwise the socket after
	// the return would look different from before the ejection.
	settle(1500);
	save(*editor, dir.getChildFile("card_40_seated.png"), closeUp(), 2.0f);

	std::printf("done\n");
	return 0;
}
