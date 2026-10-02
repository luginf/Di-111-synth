// Right-click (or long-press, on touch) menu for the transport's STOP and PLAY buttons, shared by the three
// sequencer views. "Panic" is the one real hard panic (D110SequencerHost::midiPanicHard()): it is deliberately
// behind a menu rather than a bare right-click, so nobody triggers it without meaning to.
#pragma once

#include "D110SequencerHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

namespace d110seq {

// `playFromStart` is only offered when the caller has one (PLAY's right-click in the retro panel has always
// meant "play from bar 1"); pass an empty function for STOP, or for a view that never had it.
inline void showTransportMenu(D110SequencerHost &host, std::function<void()> playFromStart = {}) {
	juce::PopupMenu menu;
	if (playFromStart) menu.addItem(1, "Play from bar 1");
	menu.addItem(2, "Panic");
	menu.showMenuAsync(juce::PopupMenu::Options(), [&host, playFromStart = std::move(playFromStart)](int result) {
		if (result == 1 && playFromStart) playFromStart();
		else if (result == 2) host.midiPanicHard();
	});
}

} // namespace d110seq
