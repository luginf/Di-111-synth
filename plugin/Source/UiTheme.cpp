#include "UiTheme.h"

namespace d110ui {

namespace {

Theme currentTheme = Theme::Dark;
ThemeMode currentMode = ThemeMode::Dark;

// Re-resolves currentTheme from currentMode - the only place System gets turned into an
// actual Dark/Light. Kept separate from setThemeMode() itself so the live OS-change listener
// below can call straight into it without re-registering anything.
void applyResolvedTheme();

// Registered lazily (only once System is picked at all) rather than unconditionally at
// startup - juce::Desktop::getInstance() touches the message manager, which isn't guaranteed
// constructed yet this early on every platform/host.
struct SystemThemeWatcher : public juce::DarkModeSettingListener {
	void darkModeSettingChanged() override {
		if (currentMode == ThemeMode::System) applyResolvedTheme();
	}
};
SystemThemeWatcher &systemThemeWatcher() {
	static SystemThemeWatcher watcher;
	return watcher;
}
bool systemThemeWatcherRegistered = false;

// Dark palette - the same numbers that used to be scattered across PluginEditor.cpp and
// D110SequencerPanel.cpp as local kEdXxx constants/literals; the values didn't change, just
// gathered into one place.
const Palette kDark {
	juce::Colour(0xff323e44), // panelBg (same blue-grey as the JiV-881 default theme)
	juce::Colour(0xff263238), // box
	juce::Colour(0xff55626a), // boxBorder
	juce::Colour(0xff6fa8dc), // label
	juce::Colour(0xff8ede4a), // value
	juce::Colour(0xff93a0a7), // dim

	juce::Colour(0xff1d1d22), // handleBg
	juce::Colour(0xff2a343a), // handleBar
	juce::Colour(0xff3c4a52), // handleBarHover
	juce::Colour(0xff8a8a94), // handleChevron
	juce::Colour(0xffd0d0d8), // handleChevronHover
	juce::Colour(0xff6a6a74), // handleLabel

	juce::Colour(0xffe8e8ec), // keyWhite
	juce::Colour(0xff6ab81f), // keyWhiteHeld
	juce::Colour(0xff0a0a0c), // keyWhiteBorder
	juce::Colour(0xff1a1a1e), // keyBlack
	juce::Colour(0xff3f7a10), // keyBlackHeld
	juce::Colour(0xff6a6a74), // keyCaption
	juce::Colour(0xff3a464d), // keyButtonFill
	juce::Colour(0xff8a8a94), // keyButtonText

	juce::Colour(0xff6ab81f), // seqActiveFill
	juce::Colour(0xff0a0a0c), // seqActiveText
	juce::Colour(0xff3a464d), // seqInactiveFill
	juce::Colour(0xffb8b8c0), // seqInactiveText
	juce::Colour(0xffd98a1f), // seqMetroDownbeat
	juce::Colour(0xff4fb81f), // seqMetroBeat
	juce::Colour(0xff3f7a10), // seqTrackFilled
	juce::Colour(0xff3a464d), // seqTrackEmpty
	juce::Colour(0xffe0392a), // seqArmDot
};

// Light palette - the same meaning for every field (labels blue, values green, like the
// instrument's own LCD backlight), carried over onto a light background instead of a dark
// one. The accent green for a pressed key/active sequencer button is deliberately the same
// in both palettes - it's the "on" colour, not a theme colour.
const Palette kLight {
	juce::Colour(0xfff2f3f5), // panelBg
	juce::Colour(0xffffffff), // box
	juce::Colour(0xffc7cbd1), // boxBorder
	juce::Colour(0xff1f5fa8), // label
	juce::Colour(0xff2f8a1f), // value
	juce::Colour(0xff8a8f98), // dim

	juce::Colour(0xffe6e8eb), // handleBg
	juce::Colour(0xffd6d9de), // handleBar
	juce::Colour(0xffc3c7ce), // handleBarHover
	juce::Colour(0xff5a5f68), // handleChevron
	juce::Colour(0xff23262c), // handleChevronHover
	juce::Colour(0xff6f7480), // handleLabel

	juce::Colour(0xfffbfbfc), // keyWhite
	juce::Colour(0xff6ab81f), // keyWhiteHeld
	juce::Colour(0xff9a9ea6), // keyWhiteBorder
	juce::Colour(0xff2c2f36), // keyBlack
	juce::Colour(0xff3f7a10), // keyBlackHeld
	juce::Colour(0xff6f7480), // keyCaption
	juce::Colour(0xffd6d9de), // keyButtonFill
	juce::Colour(0xff3d4148), // keyButtonText

	juce::Colour(0xff6ab81f), // seqActiveFill
	juce::Colour(0xff0a0a0c), // seqActiveText
	juce::Colour(0xffd6d9de), // seqInactiveFill
	juce::Colour(0xff3d4148), // seqInactiveText
	juce::Colour(0xffb8701a), // seqMetroDownbeat
	juce::Colour(0xff3f9a1a), // seqMetroBeat
	juce::Colour(0xff3f7a10), // seqTrackFilled
	juce::Colour(0xffd6d9de), // seqTrackEmpty
	juce::Colour(0xffd0311f), // seqArmDot
};

void applyResolvedTheme() {
	currentTheme = currentMode == ThemeMode::System
	                   ? (juce::Desktop::getInstance().isDarkModeActive() ? Theme::Dark : Theme::Light)
	                   : (currentMode == ThemeMode::Light ? Theme::Light : Theme::Dark);
	sharedLookAndFeel().refresh();
}

} // namespace

Theme getTheme() { return currentTheme; }
void setTheme(Theme theme) { setThemeMode(theme == Theme::Light ? ThemeMode::Light : ThemeMode::Dark); }

ThemeMode getThemeMode() { return currentMode; }
void setThemeMode(ThemeMode mode) {
	currentMode = mode;
	if (mode == ThemeMode::System && !systemThemeWatcherRegistered) {
		juce::Desktop::getInstance().addDarkModeSettingListener(&systemThemeWatcher());
		systemThemeWatcherRegistered = true;
	}
	applyResolvedTheme();
}

const Palette &palette() { return currentTheme == Theme::Light ? kLight : kDark; }

LookAndFeel::LookAndFeel() { refresh(); }

void LookAndFeel::refresh() {
	const auto &pal = palette();

	setColour(juce::ResizableWindow::backgroundColourId, pal.panelBg);
	setColour(juce::DocumentWindow::textColourId, pal.value);

	setColour(juce::AlertWindow::backgroundColourId, pal.panelBg);
	setColour(juce::AlertWindow::textColourId, pal.value);
	setColour(juce::AlertWindow::outlineColourId, pal.boxBorder);

	setColour(juce::TextButton::buttonColourId, pal.box);
	setColour(juce::TextButton::buttonOnColourId, pal.seqActiveFill);
	setColour(juce::TextButton::textColourOffId, pal.value);
	setColour(juce::TextButton::textColourOnId, pal.seqActiveText);

	setColour(juce::ComboBox::backgroundColourId, pal.box);
	setColour(juce::ComboBox::textColourId, pal.value);
	setColour(juce::ComboBox::outlineColourId, pal.boxBorder);
	setColour(juce::ComboBox::arrowColourId, pal.value);

	setColour(juce::PopupMenu::backgroundColourId, pal.box);
	setColour(juce::PopupMenu::textColourId, pal.value);
	setColour(juce::PopupMenu::highlightedBackgroundColourId, pal.seqActiveFill);
	setColour(juce::PopupMenu::highlightedTextColourId, pal.seqActiveText);

	setColour(juce::TextEditor::backgroundColourId, pal.box);
	setColour(juce::TextEditor::textColourId, pal.value);
	setColour(juce::TextEditor::outlineColourId, pal.boxBorder);
	setColour(juce::TextEditor::focusedOutlineColourId, pal.label);

	setColour(juce::Label::textColourId, pal.value);
	setColour(juce::ToggleButton::textColourId, pal.value);

	setColour(juce::Slider::thumbColourId, pal.seqActiveFill);
	setColour(juce::Slider::trackColourId, pal.boxBorder);
	setColour(juce::Slider::backgroundColourId, pal.box);

	setColour(juce::ScrollBar::thumbColourId, pal.boxBorder);

	// setColour() above only affects future painting - it doesn't by itself touch
	// anything already on screen. Without this, a dialog that was already open at the
	// moment the theme was toggled (Options itself, since that's exactly where the
	// toggle lives) would keep its stale chrome colours until closed and reopened.
	// sendLookAndFeelChange() is the standard JUCE way to push a LookAndFeel colour
	// change out to a live component tree - repaints it and recurses into every child.
	for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
		if (auto *tlw = juce::TopLevelWindow::getTopLevelWindow(i)) tlw->sendLookAndFeelChange();
}

LookAndFeel &sharedLookAndFeel() {
	static LookAndFeel laf;
	return laf;
}

namespace {
FontScale currentFontScale = FontScale::Normal;
}

FontScale getFontScale() { return currentFontScale; }
void setFontScale(FontScale scale) { currentFontScale = scale; }

} // namespace d110ui
