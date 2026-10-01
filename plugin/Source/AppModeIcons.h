#pragma once

#include "UiTheme.h"

// The Android app's three top-level views (front panel / sequencer / soundbanks) and the
// glyph each one gets on the cyclic mode-switch button. Header-only and shared, because that
// button is drawn in two places: by the app itself (panel, soundbanks and the retro sequencer
// views, which use the app's own top strip) and inside the two sequencer panels' transport rows
// (which replace that strip) - both must look the same, in the sequencer's button style.
namespace d110ui {

enum class AppMode { frontPanel = 0, sequencer = 1, soundbanks = 2 };

inline AppMode nextAppMode(AppMode m) { return AppMode((int(m) + 1) % 3); }

inline const char *appModeName(AppMode m) {
	switch (m) {
		case AppMode::frontPanel: return "Front Panel";
		case AppMode::sequencer: return "Sequencer";
		case AppMode::soundbanks: return "Soundbanks";
	}
	return "";
}

// A flat box in the sequencer's inactive-button colours - what D110SequencerPanel's
// paintToggleButton() draws behind its labels. The app's own buttons use this too, so the
// hamburger and the mode switch look identical in every view.
inline void paintSeqStyleBox(juce::Graphics &g, juce::Rectangle<float> b) {
	g.setColour(palette().seqInactiveFill);
	g.fillRect(b.reduced(2.0f));
}

inline void paintSeqStyleLabel(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label) {
	paintSeqStyleBox(g, b);
	g.setColour(palette().seqInactiveText);
	g.setFont(juce::FontOptions(juce::jlimit(8.0f, 13.0f, b.getHeight() * 0.5f)));
	g.drawText(label, b, juce::Justification::centred);
}

// Icon for `mode`, drawn inside `b` (the whole button cell, including its 2px inset).
inline void paintAppModeIcon(juce::Graphics &g, juce::Rectangle<float> b, AppMode mode) {
	auto r = b.reduced(2.0f);
	const float side = juce::jmin(r.getWidth(), r.getHeight()) * 0.58f;
	auto icon = juce::Rectangle<float>(side, side).withCentre(r.getCentre());
	g.setColour(palette().seqInactiveText);
	const float lw = juce::jmax(1.2f, side * 0.09f);
	switch (mode) {
		case AppMode::frontPanel: {
			// the hardware: a case, an LCD strip on top, a row of buttons under it
			g.drawRoundedRectangle(icon, side * 0.10f, lw);
			auto inner = icon.reduced(side * 0.14f);
			g.fillRect(inner.removeFromTop(inner.getHeight() * 0.34f));
			inner.removeFromTop(inner.getHeight() * 0.22f);
			const float d = inner.getWidth() / 5.0f;
			for (int i = 0; i < 3; ++i)
				g.fillEllipse(inner.getX() + d * (0.5f + float(i) * 1.8f), inner.getCentreY() - d * 0.3f,
				              d * 0.8f, d * 0.8f);
			break;
		}
		case AppMode::sequencer: {
			// piano-roll notes: staggered horizontal bars
			const float h = side * 0.17f;
			const float gap = (side - 4.0f * h) / 3.0f;
			const float xs[4] = { 0.00f, 0.30f, 0.12f, 0.42f };
			const float ws[4] = { 0.50f, 0.55f, 0.40f, 0.58f };
			for (int i = 0; i < 4; ++i)
				g.fillRoundedRectangle(icon.getX() + side * xs[i], icon.getY() + float(i) * (h + gap),
				                       side * ws[i], h, h * 0.3f);
			break;
		}
		case AppMode::soundbanks: {
			// a library: the classic stacked-disk "database" cylinder
			const float eh = side * 0.28f;
			auto top = juce::Rectangle<float>(icon.getX(), icon.getY(), side, eh);
			g.drawEllipse(top, lw);
			for (int i = 1; i <= 2; ++i) {
				juce::Path arc;
				const float y = icon.getY() + float(i) * (side - eh) * 0.5f;
				arc.addArc(icon.getX(), y, side, eh, juce::MathConstants<float>::pi,
				           juce::MathConstants<float>::twoPi, true);
				g.strokePath(arc, juce::PathStrokeType(lw));
			}
			g.drawLine(icon.getX(), icon.getY() + eh * 0.5f, icon.getX(), icon.getBottom() - eh * 0.5f, lw);
			g.drawLine(icon.getRight(), icon.getY() + eh * 0.5f, icon.getRight(), icon.getBottom() - eh * 0.5f,
			           lw);
			juce::Path bottom;
			bottom.addArc(icon.getX(), icon.getBottom() - eh, side, eh, juce::MathConstants<float>::pi,
			              juce::MathConstants<float>::twoPi, true);
			g.strokePath(bottom, juce::PathStrokeType(lw));
			break;
		}
	}
}

inline void paintAppModeButton(juce::Graphics &g, juce::Rectangle<float> b, AppMode mode) {
	paintSeqStyleBox(g, b);
	paintAppModeIcon(g, b, mode);
}

// Component form for the app's own top strip.
class ModeButton : public juce::Component {
public:
	std::function<void()> onClick;
	void setMode(AppMode m) {
		if (m == mode) return;
		mode = m;
		repaint();
	}
	void paint(juce::Graphics &g) override { paintAppModeButton(g, getLocalBounds().toFloat(), mode); }
	void mouseUp(const juce::MouseEvent &e) override {
		if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
	}

private:
	AppMode mode = AppMode::frontPanel;
};

// Component form of the hamburger, drawn like the sequencer's own bar-menu button.
class HamburgerButton : public juce::Component {
public:
	std::function<void()> onClick;
	void paint(juce::Graphics &g) override {
		paintSeqStyleLabel(g, getLocalBounds().toFloat(), juce::String::fromUTF8("\xe2\x98\xb0")); // U+2630
	}
	void mouseUp(const juce::MouseEvent &e) override {
		if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
	}
};

} // namespace d110ui
