// Where the app keeps its own files (custom ROM path, soundbank database, NVRAM fallback...).
// Renamed from "D-110 Emulator" to "Di-111" along with the app itself (2026-10-01); the old
// folder stays a fallback, per item, so nothing an existing install already saved is orphaned.
#pragma once

#include <juce_core/juce_core.h>

namespace d110appdata {

inline juce::File newRoot() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Di-111");
}

inline juce::File legacyRoot() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("D-110 Emulator");
}

// `relativePath` under the new root, unless only the legacy root has it. Reads and writes both
// go through this, so an item that exists only in the old folder keeps being used (and updated)
// there until it is created under the new one.
inline juce::File resolve(const juce::String &relativePath) {
	const auto fresh = newRoot().getChildFile(relativePath);
	if (fresh.exists()) return fresh;
	const auto legacy = legacyRoot().getChildFile(relativePath);
	return legacy.exists() ? legacy : fresh;
}

} // namespace d110appdata
