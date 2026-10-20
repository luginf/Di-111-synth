#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Single colour palette for the hand-drawn parts of the interface - the extended editor,
// the test keyboard and the sequencer. NOT for the photographed panel itself: its
// indicators (LCD backlight, LEDs) reflect the instrument's real colours and shouldn't
// change with the theme, so D110Panel/D110MemoryCard don't reach in here.
namespace d110ui {

enum class Theme { Dark, Light };

// The user-facing choice, as opposed to Theme above (the resolved result actually painted
// with). System tracks the OS dark-mode setting live - see setThemeMode()'s own comment.
enum class ThemeMode { Dark, Light, System };

struct Palette {
	juce::Colour panelBg;   // drawer background (kEdBack, keyboard/sequencer background)
	juce::Colour box;       // field/button rectangle
	juce::Colour boxBorder; // its border
	juce::Colour label;     // labels - blue, like Roland's own silkscreen
	juce::Colour value;     // values - green, like the D-110's own LCD backlight
	juce::Colour dim;       // secondary text

	juce::Colour handleBg;
	juce::Colour handleBar;
	juce::Colour handleBarHover;
	juce::Colour handleChevron;
	juce::Colour handleChevronHover;
	juce::Colour handleLabel;

	juce::Colour keyWhite;
	juce::Colour keyWhiteHeld;
	juce::Colour keyWhiteBorder;
	juce::Colour keyBlack;
	juce::Colour keyBlackHeld;
	juce::Colour keyCaption;
	juce::Colour keyButtonFill;
	juce::Colour keyButtonText;

	juce::Colour seqActiveFill;
	juce::Colour seqActiveText;
	juce::Colour seqInactiveFill;
	juce::Colour seqInactiveText;
	juce::Colour seqMetroDownbeat;
	juce::Colour seqMetroBeat;
	juce::Colour seqTrackFilled;
	juce::Colour seqTrackEmpty;
	juce::Colour seqArmDot; // ARM button's record-style circle when armed
};

Theme getTheme();
void setTheme(Theme theme); // sets an explicit theme - equivalent to setThemeMode(Dark/Light)
const Palette &palette();

// setThemeMode(System) resolves immediately against juce::Desktop::isDarkModeActive() and
// keeps re-resolving on every live OS dark-mode change from then on (a DarkModeSettingListener
// registered internally, once, the first time System is picked) - no polling or refresh call
// needed at any call site. getTheme()/palette() above always reflect the last resolved result,
// System included, so painting code never needs to know the mode was System in the first place.
ThemeMode getThemeMode();
void setThemeMode(ThemeMode mode);

// UI text/control size preference (Alan's request, 2026-09-03: an HDPI screen with no OS-level
// scaling leaves every label/knob uncomfortably small). Normal is 1:1, unchanged from before
// this existed; Big renders everything - text AND every component's own size, since scaling
// only the font would leave labels overflowing the small fixed-size boxes this UI lays them
// into - through juce::Desktop::getGlobalScaleFactor() at kBigScaleFactor. Pure state here,
// palette()-independent; actually APPLYING it (Desktop::getInstance().setGlobalScaleFactor())
// is the caller's job, and only ever safe to do from a Standalone build (JucePlugin_Build_
// Standalone) - the Desktop's global scale factor is process-wide, and a VST3/AU instance
// shares its process with the host and every other plugin's window, so calling it there would
// visibly resize the DAW itself. D110AudioProcessorEditor/D50Editor both gate the actual call
// behind that macro; getFontScale()/setFontScale() themselves have no such restriction; they're
// just the persisted preference.
enum class FontScale { Normal, Big };
inline constexpr float kBigScaleFactor = 1.35f;
FontScale getFontScale();
void setFontScale(FontScale scale);

// Drawer text weights (2026-10-20, Alan: the drawer read as "all bold" next to the JiV-881).
// The JiV-881 uses JUCE's default sans, plain, at 13 px, with bold only on 15-18 px section
// headings; this drawer had bold everywhere at 11-12 px, which smears on FreeType. Body and
// caption text is plain; only section headings get bold. Not a global LookAndFeel default on
// purpose: that would also restyle the sequencer/keyboard drawers and the popup menus.
// Background of the sequencer drawers: near-black on the dark theme (the JiV-881's sequencer
// drawer is nearly black with bright text; this one was the drawer's blue-grey, with grey text
// on it, and read as low contrast - Alan, 2026-10-20). The light theme keeps its own background.
inline juce::Colour sequencerBackground() {
	return getTheme() == Theme::Light ? palette().panelBg : palette().panelBg.darker(0.62f);
}
inline juce::Font bodyFont(float size) { return juce::Font(juce::FontOptions(size)); }
inline juce::Font headingFont(float size) { return juce::Font(juce::FontOptions(size, juce::Font::bold)); }

// A juce::LookAndFeel driven by palette() - keeps stock JUCE components (AlertWindow,
// AudioDeviceSelectorComponent, PopupMenu, ComboBox, ...) themed consistently with the
// hand-painted parts of the UI, which read palette() directly in their own paint()
// instead of going through LookAndFeel at all. Not installed anywhere by default - an
// app opts in with juce::LookAndFeel::setDefaultLookAndFeel(&sharedLookAndFeel());
// setTheme() keeps its colours in sync automatically from then on, no separate refresh
// call needed at the call site.
class LookAndFeel : public juce::LookAndFeel_V4 {
public:
	LookAndFeel();
	void refresh();
};
LookAndFeel &sharedLookAndFeel();

} // namespace d110ui
