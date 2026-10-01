#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "sequencer/D110SequencerSongsFile.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Scans raw bytes for concatenated F0...F7 SysEx messages, e.g. the contents of a bare .syx file.
static std::vector<std::vector<MT32Emu::Bit8u>> extractSysexMessagesFromRawBytes(const juce::MemoryBlock &data) {
	std::vector<std::vector<MT32Emu::Bit8u>> messages;
	const auto *bytes = static_cast<const MT32Emu::Bit8u *>(data.getData());
	const size_t len = data.getSize();

	size_t i = 0;
	while (i < len) {
		if (bytes[i] == 0xF0) {
			size_t j = i + 1;
			while (j < len && bytes[j] != 0xF7) ++j;
			if (j < len) {
				messages.emplace_back(bytes + i, bytes + j + 1);
				i = j + 1;
				continue;
			}
			break; // Unterminated trailing message - ignore it.
		}
		++i;
	}
	return messages;
}

// Pulls out just the SysEx meta-events from a standard MIDI file, in file order. Note/CC/etc.
// events are intentionally ignored - this is for bank/patch dumps distributed as .mid files,
// not for playing the file as a song.
static std::vector<std::vector<MT32Emu::Bit8u>> extractSysexMessagesFromMidiFile(const juce::File &file) {
	std::vector<std::vector<MT32Emu::Bit8u>> messages;

	juce::FileInputStream stream(file);
	if (!stream.openedOk()) return messages;

	juce::MidiFile midiFile;
	if (!midiFile.readFrom(stream)) return messages;

	for (int t = 0; t < midiFile.getNumTracks(); ++t) {
		const auto *track = midiFile.getTrack(t);
		for (int e = 0; e < track->getNumEvents(); ++e) {
			const auto &message = track->getEventPointer(e)->message;
			if (!message.isSysEx()) continue;
			const auto *raw = message.getRawData();
			messages.emplace_back(raw, raw + message.getRawDataSize());
		}
	}
	return messages;
}

// Inverse of LA32FloatWaveGenerator::getPCMSample()'s decode (amplitude = pow(2, (mag -
// 32787) / 2048.0), sign from bit 15) - see Synth::setPCMWaveSamples()'s own comment. mag 0
// decodes back to about -16 dBFS-ish silence (pow(2,-16.01)), not true zero - the format has
// no exact zero, matching the real ROM's own encoding, so genuine silence in a custom sample
// still gets the closest representable near-silent value rather than being a special case.
static MT32Emu::Bit16s encodePcmLogSample(float amplitude) {
	amplitude = juce::jlimit(-1.0f, 1.0f, amplitude);
	const bool sign = amplitude < 0.0f;
	const float mag = std::abs(amplitude);
	int log = mag <= 0.0f ? 0 : juce::roundToInt((std::log2(mag) * 2048.0f) + 32787.0f);
	log = juce::jlimit(0, 32767, log);
	return static_cast<MT32Emu::Bit16s>(sign ? (log | 0x8000) : log);
}

static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
	std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

	// 0..2 with unity as the default, so unity is the exact middle of the range and the
	// panel knob therefore rests pointing straight up - the reference photo's printed scale
	// is centred on vertical to within 0.03 degrees, and the knob draws whatever fraction of
	// its travel the parameter is at. With a 0..1 range the default of unity WAS the maximum,
	// so the knob sat hard clockwise: correct arithmetic, wrong-looking instrument.
	//
	// The consequence, stated plainly rather than avoided: the top half of the travel is
	// boost, and running it wide open is +6 dB and will clip a busy patch. That is what a
	// hardware volume control does too, and it is the player's choice; the default loudness
	// is unchanged either way. (An earlier revert to 0..1 traded the knob's rest position
	// away to prevent that, which fixed nothing that was broken.)
	//
	// Safe for existing projects: the value tree stores the real value, not a normalised
	// one, so a session saved at 1.0 still loads as unity.
	params.push_back(std::make_unique<juce::AudioParameterFloat>(
		juce::ParameterID{"masterVolume", 1}, "Master Volume",
		juce::NormalisableRange<float>(0.0f, 2.0f), 1.0f));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"reverbEnabled", 1}, "Reverb", true));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"superMode", 1}, "Super Mode (unofficial)", false));

	return {params.begin(), params.end()};
}

juce::AudioProcessor::BusesProperties D110AudioProcessor::createBuses() {
	// Bus 0 keeps its original name and stays default-on, unchanged from before individual
	// outputs existed - a project or host state that only knows this one bus is not affected
	// by there now being six more.
	BusesProperties buses;
	buses = buses.withOutput("Output", juce::AudioChannelSet::stereo(), true);
	for (int output = 1; output <= 6; ++output)
		buses = buses.withOutput("INDIVIDUAL " + juce::String(output), juce::AudioChannelSet::mono(), false);
	return buses;
}

bool D110AudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
	if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
	for (int bus = 1; bus < layouts.outputBuses.size(); ++bus) {
		const auto set = layouts.getChannelSet(false, bus);
		if (!set.isDisabled() && set != juce::AudioChannelSet::mono()) return false;
	}
	return true;
}

D110AudioProcessor::D110AudioProcessor()
	: AudioProcessor(createBuses()),
	  parameters(*this, nullptr, "PARAMS", createParameterLayout()) {
	masterVolumeParam = parameters.getRawParameterValue("masterVolume");
	reverbEnabledParam = parameters.getRawParameterValue("reverbEnabled");
	superModeParam = parameters.getRawParameterValue("superMode");

	// Reads sequencerLiveChannels rather than the core directly - see that array's own
	// comment. Set once here rather than per-block: the callback itself never changes,
	// only the array it closes over, which processBlock refreshes each block.
	sequencerLiveChannels.fill(-1);
	sequencerLiveChannels[d110seq::D110SequencerEngine::kRhythmTrack] = 10;
	sequencerEngine.setChannelSource(
		[this](int track) { return sequencerLiveChannels[static_cast<size_t>(track)]; });

	sequencerLivePrograms.fill(-1);
	sequencerLiveVolumes.fill(-1);
	sequencerLivePans.fill(-1);
	sequencerLiveInternalTone.fill(-1);
	sequencerEngine.setProgramSource(
		[this](int track) { return sequencerLivePrograms[static_cast<size_t>(track)]; });
	sequencerEngine.setVolumeSource(
		[this](int track) { return sequencerLiveVolumes[static_cast<size_t>(track)]; });
	sequencerEngine.setPanSource(
		[this](int track) { return sequencerLivePans[static_cast<size_t>(track)]; });
	sequencerEngine.setSysExPreambleSource([this](int track) { return buildTrackSysExPreamble(track); });
	sequencerEngine.setLoadedTrackSetupSink(
		[this](int track, std::vector<juce::MidiMessage> setup) { applyLoadedTrackSetup(track, std::move(setup)); });
	// The D-110 itself has no Bank Select concept - A/B is folded straight into the Program
	// Change byte (see the "No Bank Select (CC0) here" comment in processBlock()). But an
	// exported .mid file is read by other software, not just this instrument: a receiver going
	// off Roland-D110.idf's own <Patch hbank="0" lbank="0"> entries needs an explicit Bank
	// Select MSB/LSB before it'll resolve a Program Change to a patch name at all - so always
	// write bank 1/1 (musician-facing, same numbering as NonetSeqHost - raw wire byte 0/0,
	// matching hbank/lbank="0" in the .idf), regardless of which track or program.
	sequencerEngine.setBankSource([](int) { return 1; });
	sequencerEngine.setBankLsbSource([](int) { return 1; });


	tryAutoLoadRoms();
}

D110AudioProcessor::~D110AudioProcessor() {
	flushLiveNvramToDisk();
	closeSynth();
}

void D110AudioProcessor::flushLiveNvramToDisk() {
	if (!core.isRunning()) return;
	juce::MemoryBlock rams(D110CoreType::kRamSize);
	juce::MemoryBlock memcs(D110CoreType::kCardSize);
	if (core.getRam(static_cast<juce::uint8 *>(rams.getData()))
	    && core.getCardImage(static_cast<juce::uint8 *>(memcs.getData())))
		writeNvramFiles(rams, memcs);
}

void D110AudioProcessor::closeSynth() {
	// Order matters: the synth must be destroyed before the ROM images it references,
	// and the ROM images must be destroyed before the file streams backing them.
	sampleRateConverter.reset();
	synth.reset();
	if (controlROMImage != nullptr) {
		MT32Emu::ROMImage::freeROMImage(controlROMImage);
		controlROMImage = nullptr;
	}
	if (pcmROMImage != nullptr) {
		MT32Emu::ROMImage::freeROMImage(pcmROMImage);
		pcmROMImage = nullptr;
	}
	controlRomFile.reset();
	pcmRomFile.reset();
}

void D110AudioProcessor::setControlRomPath(const juce::String &path) {
	if (!juce::File(path).loadFileAsData(controlRomData)) {
		lastError = "Could not read control ROM file: " + path;
		return;
	}
	controlRomPath = path;
	openSynthIfReady();
}

void D110AudioProcessor::setPcmRomPath(const juce::String &path) {
	if (!juce::File(path).loadFileAsData(pcmRomData)) {
		lastError = "Could not read PCM ROM file: " + path;
		return;
	}
	pcmRomPath = path;
	openSynthIfReady();
}

void D110AudioProcessor::setPoweredOn(bool shouldBePoweredOn) {
	if (poweredOn.load() == shouldBePoweredOn) return;

	// Booting is the real thing: the machine starts here and the firmware comes up on the
	// panel's own display in real time, not fast-forwarded.
	if (shouldBePoweredOn) {
		const auto nvram = getNvramFolder();
		nvram.getChildFile("d110").createDirectory();

		// The very first time this plugin is ever used there is no memory at all, and a
		// D-110 with blank battery RAM boots to an empty patch - it reads as broken rather
		// than as new. So perform the documented cold start, once. After that the file is
		// there and every later run, in this host session or any other, picks up exactly
		// where the last one left off.
		const bool virgin = !getMachineNvramFolder().getChildFile("rams").existsAsFile();

		// D110CoreNative wants the plain folder the ROM files sit loose in (same one
		// getAutoRomFolder() already points every other ROM-loading path at); D110Core wants
		// a MAME rompath, which is why getMameRomPath() mirrors those same files into a
		// MAME-shaped romset directory first. Two different search conventions for the same
		// underlying files, not two different ROM sets.
#ifdef D110_NATIVE_CORE
		materializeNativeRomFiles();
		const auto romArg = getAutoRomFolder().getFullPathName().toStdString();
#else
		const auto romArg = getMameRomPath().toStdString();
#endif
		if (!core.start(romArg, nvram.getFullPathName().toStdString())) {
			powerBlocked = true;
#ifdef D110_NATIVE_CORE
			// Unlike the MAME-backed core, D110CoreNative has no process-wide "only one
			// machine" lock - every instance is fully independent, so several can run at
			// once. A false return here can only mean one thing: it couldn't read one of
			// the three ROM files it needs from romArg.
#if JUCE_ANDROID
			// No Utility tab on Android (the extended editor drawer it lives in is excluded
			// there entirely - see docs/android.md) - point at the hamburger menu's own
			// "Choose ROM files..." instead, the actual way to fix this on this platform.
			lastError = "Could not start the emulator: " + core.lastStartError()
			            + ". Check that the Control ROM, PCM ROM and character-generator ROM "
			              "are present there, or use the hamburger menu's \"Choose ROM "
			              "files...\" to pick them.";
#else
			lastError = "Could not start the emulator: " + core.lastStartError()
			            + ". Check that the Control ROM, PCM ROM and character-generator ROM "
			              "are present in the data folder shown on the Utility tab.";
#endif
#else
			// The MAME-backed core does hold a single process-wide machine slot (see
			// D110Core::sMachineLive) - a second D110Emulator instance really can't run
			// alongside a first.
			lastError = "Another Di-111 instance is already switched on. "
			            "Only one can run at a time - switch that one off first.";
#endif
			return;
		}
		powerBlocked = false;
		// The real sound-board interface, measured and fixed 2026-07-31 (see
		// docs/la32_interface.md): a wrong voice number, a MAME MCS-96 core bug (EXTINT's
		// pending-interrupt bit never clearing on take), and a too-narrow busy-value scan
		// were the three things standing between this and working audio-driven playback.
		// All three fixed; validated against a stress-chord test (30s, was 0-3s), a
		// realistic single-note test (120 notes over 60s) and the real demo song (three
		// songs back to back, panel responsive throughout). Safe to leave on unconditionally
		// - it only ever acts while the firmware is genuinely parked waiting for it.
		// La32Ramps (the real amplitude/filter envelope model - see D110CoreNative.h's own
		// comment on La32RampState) measurably fixes La32Stub's voice-starvation problem
		// (native_polyphony_stress_probe: 31/32 hardware slots stuck busy under La32Stub vs
		// 2/32 under La32Ramps after a 60-note run). A live DAW session previously hit a real
		// freeze under fast overlapping notes/chords that offline tests didn't reproduce at
		// the time; root-caused (native_ramp_edge_stress_probe.cpp - a ramp landing only
		// answers a voice's own envelope completion, never the separate per-note DISPATCH wait,
		// so a newly dispatched voice under heavy overlap could starve forever) and fixed by
		// also answering the dispatch handshake through the same channel.
		//
		// Native-only, deliberately: the same conceptual fix ported to D110Core.cpp (the
		// MAME-backed core) introduced a NEW, 100% reproducible regression there - the very
		// first note-off after every cold boot never completes (measured:
		// mame_stuck_note_repro.cpp; confirmed absent under La32Stub, present only under
		// La32Ramps, on identical boot/tone/note sequences; the new dispatch-ack code was
		// confirmed NOT to be the cause - it never even triggers in the failing runs, and
		// landed-ramp events look identical between a failing first note and a succeeding
		// later one). Root cause not yet isolated - suspected interaction between La32Ramps'
		// faster/more eager EXTINT responses and something in the MAME machine's own real-time
		// boot sequence, not reproducible on the native core's synchronous, thread-free
		// stepping. Reported by the owner: D110EmulatorNative plays cleanly, D110Emulator
		// (MAME-backed) sustains notes forever and freezes the LCD on the very first notes of
		// a session. Until root-caused, only the native core gets La32Ramps by default; the
		// MAME-backed core stays on the safe, long-proven La32Stub.
#ifdef D110_NATIVE_CORE
		core.setStuckPolicy(D110CoreType::StuckPolicy::La32Ramps);
#else
		core.setStuckPolicy(D110CoreType::StuckPolicy::La32Stub);
#endif
		if (virgin) core.factoryReset();
	} else {
		// Stopping is what makes MAME write its NVRAM out, so this is where the state
		// actually reaches disk and survives the host being closed.
		core.stop();
	}
	poweredOn = shouldBePoweredOn;
}

// Accepts both the underscore and space spellings of a plugin data folder name - some
// platforms/older builds used one or the other (see getAutoRomFolder() below) - so neither
// convention silently gets ignored. Prefers whichever actually has something in it - an
// empty folder under the "wrong" spelling (e.g. left behind by an older install, or created
// by hand while guessing at the name) must not shadow a populated one under the other
// spelling. Among two empty (or two populated) candidates, prefers the space variant; if
// neither exists at all, also returns the space variant, to create, matching this project's
// own established naming (D-110 Emulator, D-110 Data).
static bool folderIsPopulated(const juce::File &f) {
	return f.isDirectory() && f.getNumberOfChildFiles(juce::File::findFilesAndDirectories) > 0;
}

// Like folderIsPopulated(), but blind to the app's own housekeeping subfolders (nvram,
// romset) - getNvramRoot() creates "nvram" (and setPoweredOn() then "nvram/d110") inside
// whatever getAutoRomFolder() currently resolves to on EVERY power-on attempt, including a
// failed one with no ROMs present yet (prepareToPlay() always calls setPoweredOn(true), so
// this happens on a plain ROM-less launch, not just a successful one). Without this
// exclusion, that leftover folder alone makes folderIsPopulated() true forever after,
// permanently short-circuiting getAutoRomFolder()'s populated-folder gate (and therefore
// the loose-file/app-data fallbacks below it) even after real ROM files are dropped
// somewhere else the auto-scan would otherwise have found them.
static bool folderHasRoms(const juce::File &f) {
	if (!f.isDirectory()) return false;
	for (const auto &entry : juce::RangedDirectoryIterator(f, false, "*", juce::File::findFilesAndDirectories)) {
		const auto name = entry.getFile().getFileName();
		if (name.equalsIgnoreCase("nvram") || name.equalsIgnoreCase("romset")) continue;
		return true;
	}
	return false;
}

static juce::File resolveNamedFolder(const juce::File &parent, const juce::String &spaceName) {
	const auto spaced = parent.getChildFile(spaceName);
	const auto underscored = parent.getChildFile(spaceName.replaceCharacter(' ', '_'));
	const auto &isPopulated = folderIsPopulated;
	if (isPopulated(spaced)) return spaced;
	if (isPopulated(underscored)) return underscored;
	if (spaced.isDirectory()) return spaced;
	if (underscored.isDirectory()) return underscored;
	return spaced;
}

// The firmware's memory lives beside the ROMs, in the plugin's own data folder - so the
// D-110's battery RAM is simply `D-110 Data\nvram\d110\rams`.
//
// Named `mame_nvram` until 2026-08-06, matching a convention shared with this project's own
// MAME-based sibling emulators (MU-100R's `mame_nvram\mu100r\nvram`, QS300's
// `mame_nvram\qs300\ram`, etc.) - no longer apt now that D110EmulatorNative (MAME-free) is
// the default/shipping backend, and it was already inconsistent with the plain `nvram` name
// the app-data fallback below has always used. Renamed, with a one-time migration so an
// existing install's saved patches/timbres/memory card aren't silently orphaned under the old
// name.
juce::File D110AudioProcessor::getNvramRoot() {
	const auto romFolder = getAutoRomFolder();
	const auto preferred = romFolder.getChildFile("nvram");
	const auto legacyName = romFolder.getChildFile("mame_nvram");
	if (!preferred.isDirectory() && legacyName.isDirectory()) legacyName.moveFileTo(preferred);

	static const bool writable = [preferred] {
		preferred.createDirectory();
		const auto probe = preferred.getChildFile(".write-test");
		if (!probe.replaceWithText("x")) return false;
		probe.deleteFile();
		return true;
	}();
	if (writable) return preferred;

	return resolveNamedFolder(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory),
	                          "D-110 Emulator")
		.getChildFile("nvram");
}

bool D110AudioProcessor::nvramIsBesideRoms() {
	return getNvramRoot().isAChildOf(getAutoRomFolder());
}

// ONE memory, shared by every instance and persistent for good - the same arrangement as
// every other synth in this series, and the same as the instrument itself, which has one
// set of batteries and remembers what you left in it.
//
// It used to be one folder per instance, keyed by a fresh id. That was wrong twice over:
// a plugin loaded anew got a new id and therefore the factory state, so quitting the host
// and coming back lost everything, and the ids piled up as abandoned folders. Nothing was
// gained by it either, because only one instance can be switched on at a time anyway.
juce::File D110AudioProcessor::getNvramFolder() const {
	return getNvramRoot();
}

// Where MAME actually puts the two files, and therefore where a restored project's memory
// has to be written for the machine to pick it up.
juce::File D110AudioProcessor::getMachineNvramFolder() {
	return getNvramRoot().getChildFile("d110");
}

// MAME keeps each nvram_device in its own file under <nvram_directory>/<machine>/, so the
// D-110's battery RAM is "rams" and its memory card is "memcs", both raw 32 KB images.
void D110AudioProcessor::writeNvramFiles(const juce::MemoryBlock &rams,
                                         const juce::MemoryBlock &memcs) const {
	const auto dir = getMachineNvramFolder();
	dir.createDirectory();
	if (rams.getSize() > 0) dir.getChildFile("rams").replaceWithData(rams.getData(), rams.getSize());
	if (memcs.getSize() > 0) dir.getChildFile("memcs").replaceWithData(memcs.getData(), memcs.getSize());
}

juce::MemoryBlock D110AudioProcessor::readNvramFile(const juce::String &name) const {
	juce::MemoryBlock block;
	const auto file = getMachineNvramFolder().getChildFile(name);
	if (file.existsAsFile()) file.loadFileAsData(block);
	return block;
}

// The ROM files sit loose in the plugin's data folder, the same as every other synth in
// this series. The emulated control board, though, insists on MAME's own convention: a
// romset is either `d110.zip` or a directory literally named `d110`. Rather than push
// that layout onto the data folder, the plugin quietly mirrors the ROMs into a private
// working folder shaped the way the machine wants. Nothing about the data folder
// changes, and the copy is about 1.2 MB.
juce::String D110AudioProcessor::getMameRomPath() {
	const auto root = resolveNamedFolder(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory),
	                                     "D-110 Emulator").getChildFile("romset");
	const auto setDir = root.getChildFile("d110");
	setDir.createDirectory();

	// Only the sizes a D-110 chip can actually have, so nothing unrelated in the data
	// folder gets dragged along. Sub-folders are searched too, in case the user unpacked
	// the archive rather than emptying it out.
	auto isChipSize = [](juce::int64 n) {
		return n == 4096 || n == 32768 || n == 131072 || n == 524288;
	};

	const auto source = getAutoRomFolder();
	if (source.isDirectory())
		for (const auto &entry : juce::RangedDirectoryIterator(source, true, "*", juce::File::findFiles)) {
			const auto file = entry.getFile();
			if (!isChipSize(file.getSize())) continue;
			const auto target = setDir.getChildFile(file.getFileName());
			// Refresh only when it actually differs, so start-up is not a needless copy.
			if (!target.existsAsFile() || target.getSize() != file.getSize()
			    || target.getLastModificationTime() < file.getLastModificationTime())
				file.copyFileTo(target);
		}

	return root.getFullPathName();
}

// A ROM whole-image (Control or PCM) dropped LOOSE right next to the plugin/binary itself -
// the VST3-colocated shared folder (e.g. `~/.vst3`, home to every OTHER plugin's own bundle
// too) or the Standalone executable's own folder - rather than inside a dedicated "D-110 Data"
// subfolder (Alan's request 2026-08-21: "permettre d'avoir les ROM ... dans le même dossier que
// le VST3/le binaire"). Deliberately non-recursive (`RangedDirectoryIterator(base, false, ...)`):
// the VST3-colocated folder in particular can hold dozens of other plugins' own bundles, and a
// RECURSIVE scan of all of that looking for ROM-shaped files would be needlessly slow (and is
// exactly what materializeNativeRomFiles()/getMameRomPath() already do safely today, but only
// because getAutoRomFolder() has only ever pointed them at a small, dedicated subfolder - never
// at a big shared one). A non-recursive listing only ever sees LOOSE FILES sitting directly in
// that top folder, never walks into another plugin's own bundle directory, so this stays cheap
// regardless of how much else is installed there. Found files get copied into `dest` (the
// dedicated "D-110 Data" folder) so every other part of the ROM/NVRAM machinery keeps working
// completely unchanged - this is purely "make loose files show up where the rest of the app
// already expects them to be", not a new place the app reads ROMs FROM at runtime.
void D110AudioProcessor::materializeLooseRomsIfNeeded(const juce::File &dest) {
	if (folderHasRoms(dest)) return;

	juce::Array<juce::File> looseBases;
#if JUCE_WINDOWS
	looseBases.add(juce::File("C:/Program Files/Common Files/VST3"));
#elif JUCE_MAC
	looseBases.add(
		juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Plug-Ins/VST3"));
#else
	looseBases.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".vst3"));
#endif
	looseBases.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());

	// Whole-image sizes (Control, PCM) plus every individual MAME-style chip dump size this
	// project already recognises elsewhere (getMameRomPath()'s own isChipSize, plus the
	// character-generator ROM's 4096) - not just whatever identifyRomData() (mt32emu's own
	// Control/PCM classifier) accepts, since booting the actual FIRMWARE also needs the
	// character-generator and BOSS reverb chips, neither of which mt32emu itself knows about.
	// A size match is enough here (not a full checksum) - anything that lands in `dest` this
	// way still has to pass the real identification/reassembly checks in tryAutoLoadRoms()/
	// tryAssembleRomsFromChipDumps() before it's actually used for anything.
	auto isKnownRomSize = [](juce::int64 n) {
		return n == 4096 || n == 32768 || n == 131072 || n == 163840 || n == 524288 || n == 1048576;
	};

	for (const auto &base : looseBases) {
		if (!base.isDirectory()) continue;
		bool copiedAnything = false;
		for (const auto &entry : juce::RangedDirectoryIterator(base, false, "*", juce::File::findFiles)) {
			const auto file = entry.getFile();
			if (!isKnownRomSize(file.getSize())) continue;
			dest.createDirectory();
			if (file.copyFileTo(dest.getChildFile(file.getFileName()))) copiedAnything = true;
		}
		if (copiedAnything) return;
	}
}

juce::File D110AudioProcessor::getAutoRomFolder() {
	// Highest priority: an explicit user override (Utility tab, "ROM FOLDER") - see
	// getCustomRomFolder()'s own comment. Trusted as-is, no populated-folder gate: if Alan
	// points this here on purpose and it's empty/wrong, that should surface as the normal
	// "ROMs not found" error rather than silently falling through to auto-discovery, which
	// would be far more confusing to debug.
	const auto custom = getCustomRomFolder();
	if (custom.isNotEmpty()) return juce::File(custom);

	// Colocated with the platform's standard shared VST3 folder (see VST3_COPY_DIR in
	// plugin/CMakeLists.txt) - makes sense for the plugin, since every DAW scans there anyway,
	// and is the default location for a fresh install (checked first, below).
#if JUCE_ANDROID
	// Android has no VST3/DAW concept at all, so the desktop "colocated with ~/.vst3" fallback
	// below is meaningless there (it used to resolve to a nonsense path under the app's private
	// internal storage, e.g. "/data/user/0/<pkg>/.vst3/D-110 Data", shown verbatim in the
	// "ROMs not found" error - confusing on a phone). Use the same fixed external-files folder
	// Main.cpp's romBringUpDir() already auto-detects/writes into instead - duplicated here as a
	// literal path since this file has no shared header with the Android-only Main.cpp; keep
	// both in sync if this ever changes.
	const auto vst3Colocated = juce::File(
		"/storage/emulated/0/Android/data/com.d110emulator.android/files/roms");
#elif JUCE_WINDOWS
	const auto vst3Colocated = resolveNamedFolder(juce::File("C:/Program Files/Common Files/VST3"), "D-110 Data");
#elif JUCE_MAC
	const auto vst3Colocated = resolveNamedFolder(
		juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Plug-Ins/VST3"),
		"D-110 Data");
#else
	const auto vst3Colocated = resolveNamedFolder(
		juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".vst3"), "D-110 Data");
#endif

	// A second, per-OS "app data" location - the same root getNvramRoot()'s own fallback and
	// the Standalone's settings file already use - because asking a Standalone user (no VST3
	// host installed, possibly no ~/.vst3 at all) to put ROMs inside a VST3-specific folder
	// makes no sense for them. Only actually used if that is where the ROMs turn out to be;
	// the VST3-colocated folder above stays the default for a fresh install either way, so
	// nothing changes for anyone already using it.
	const auto appData = resolveNamedFolder(
		juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("D-110 Emulator"),
		"D-110 Data");

	if (folderHasRoms(vst3Colocated)) return vst3Colocated;
	if (folderHasRoms(appData)) return appData;

	// Neither dedicated folder has anything yet - one last look for ROMs sitting loose right
	// next to the VST3 bundle or the Standalone binary itself, copied in if found.
	materializeLooseRomsIfNeeded(vst3Colocated);
	if (folderHasRoms(vst3Colocated)) return vst3Colocated;

	return vst3Colocated;
}

juce::String D110AudioProcessor::getCustomRomFolder() {
	const auto file = getCustomRomPathFile();
	if (!file.existsAsFile()) return {};
	return file.loadFileAsString().trim();
}

void D110AudioProcessor::setCustomRomFolder(const juce::String &path) {
	const auto file = getCustomRomPathFile();
	if (path.trim().isEmpty()) {
		file.deleteFile();
		return;
	}
	file.getParentDirectory().createDirectory();
	file.replaceWithText(path.trim());
}

juce::String D110AudioProcessor::getCustomSampleFolder() {
	const auto file = getCustomSamplePathFile();
	if (!file.existsAsFile()) return {};
	return file.loadFileAsString().trim();
}

void D110AudioProcessor::setCustomSampleFolder(const juce::String &path) {
	const auto file = getCustomSamplePathFile();
	if (path.trim().isEmpty()) {
		file.deleteFile();
		return;
	}
	file.getParentDirectory().createDirectory();
	file.replaceWithText(path.trim());
}

juce::String D110AudioProcessor::getSoundbankSourceFolder() {
	const auto file = getSoundbankSourcePathFile();
	if (!file.existsAsFile()) return {};
	return file.loadFileAsString().trim();
}

void D110AudioProcessor::setSoundbankSourceFolder(const juce::String &path) {
	const auto file = getSoundbankSourcePathFile();
	if (path.trim().isEmpty()) {
		file.deleteFile();
		return;
	}
	file.getParentDirectory().createDirectory();
	file.replaceWithText(path.trim());
}

void D110AudioProcessor::injectSoundbankPatch(int slot, const juce::uint8 *data128) {
	if (slot < 0 || slot >= D110CoreType::kNumPatches) return;
	sendAreaData(D110CoreType::kSysexPatches, slot * D110CoreType::kPatchRecord, data128,
	             D110CoreType::kPatchRecord);
}

// Name (10 bytes) then body (246 bytes, chunked - see storeToneFromPart()'s own sendToneBlock()
// a few hundred lines below, same 123-byte chunk size, "exactly like an external librarian
// would send it" per its own comment) - together the full 256-byte Tone Memory record shape.
void D110AudioProcessor::injectSoundbankTone(int slot, const juce::uint8 *data256) {
	if (slot < 0 || slot >= D110CoreType::kNumTones) return;
	const int base = slot * D110CoreType::kToneMemRecord;
	sendAreaData(D110CoreType::kSysexTones, base, data256, D110CoreType::kNameChars);

	constexpr int kChunk = 123;
	const juce::uint8 *body = data256 + D110CoreType::kNameChars;
	for (int off = 0; off < D110CoreType::kToneRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneRecord - off);
		sendAreaData(D110CoreType::kSysexTones, base + D110CoreType::kNameChars + off, body + off, len);
	}
}

// Straight into Tone Temporary (kSysexToneTemp) for `part`, no Tone Memory slot spent - the
// non-destructive, instant-audition counterpart to auditionTone(part, slot) below, for bytes
// that aren't (or aren't yet) stored anywhere.
void D110AudioProcessor::auditionToneBytes(int part, const juce::uint8 *body246) {
	if (part < 0 || part > 7) return;
	constexpr int kChunk = 123;
	for (int off = 0; off < D110CoreType::kToneRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneRecord - off);
		sendAreaData(D110CoreType::kSysexToneTemp, part * D110CoreType::kToneRecord + off,
		             body246 + off, len);
	}
}

// Same DT1 shape/chunking as injectSoundbankTone() just above, sent out the external MIDI Out
// device instead of into this emulator's own firmware - see this method's own header comment
// (PluginProcessor.h) for why sendAreaData() (core.pushMidi-only) can't be reused here.
bool D110AudioProcessor::sendSoundbankToneToExternalMidi(int slot, const juce::uint8 *data256) {
	if (slot < 0 || slot >= D110CoreType::kNumTones || data256 == nullptr) return false;

	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut == nullptr) return false;

	const int base = slot * D110CoreType::kToneMemRecord;
	auto sendChunk = [&](int offset, const juce::uint8 *bytes, int length) {
		juce::uint8 msg[D110CoreType::kMaxSysexBytes];
		const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexTones, base + offset, bytes, length, msg);
		if (n > 0) osMidiOut->sendMessageNow(juce::MidiMessage(msg, n));
	};

	sendChunk(0, data256, D110CoreType::kNameChars);
	constexpr int kChunk = 123;
	const juce::uint8 *body = data256 + D110CoreType::kNameChars;
	for (int off = 0; off < D110CoreType::kToneRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneRecord - off);
		sendChunk(D110CoreType::kNameChars + off, body + off, len);
	}
	return true;
}

// External-hardware equivalent of auditionToneBytes() - see this method's own header comment
// (PluginProcessor.h) for why it targets Tone Temporary (kSysexToneTemp), not Tone Memory.
bool D110AudioProcessor::sendSoundbankToneToExternalMidiPart(int part, const juce::uint8 *body246) {
	if (part < 0 || part > 7 || body246 == nullptr) return false;

	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut == nullptr) return false;

	const int base = part * D110CoreType::kToneRecord;
	constexpr int kChunk = 123;
	for (int off = 0; off < D110CoreType::kToneRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneRecord - off);
		juce::uint8 msg[D110CoreType::kMaxSysexBytes];
		const int n =
			D110CoreType::buildDt1Message(D110CoreType::kSysexToneTemp, base + off, body246 + off, len, msg);
		if (n > 0) osMidiOut->sendMessageNow(juce::MidiMessage(msg, n));
	}
	return true;
}

// See this method's own header comment (PluginProcessor.h) for what it's for. Same
// buildDt1Message()-to-osMidiOut technique as sendSoundbankToneToExternalMidi() above, but a
// single message (128 bytes fits without chunking) targeting Patch Memory instead of Tone
// Memory.
bool D110AudioProcessor::sendPatchToExternalMidi(int slot, const juce::uint8 *data128) {
	if (slot < 0 || slot >= D110CoreType::kNumPatches || data128 == nullptr) return false;

	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut == nullptr) return false;

	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexPatches,
	                                             slot * D110CoreType::kPatchRecord, data128,
	                                             D110CoreType::kPatchRecord, msg);
	if (n <= 0) return false;
	osMidiOut->sendMessageNow(juce::MidiMessage(msg, n));
	return true;
}

bool D110AudioProcessor::identifyRomData(const juce::MemoryBlock &data,
                                         MT32Emu::ROMInfo::Type &typeOut) {
	if (data.getSize() == 0) return false;
	MT32Emu::ArrayFile probe(static_cast<const MT32Emu::Bit8u *>(data.getData()), data.getSize());
	const MT32Emu::ROMImage *image = MT32Emu::ROMImage::makeROMImage(&probe);
	const MT32Emu::ROMInfo *info = image->getROMInfo();
	const bool recognised = info != nullptr;
	if (recognised) typeOut = info->type;
	MT32Emu::ROMImage::freeROMImage(image);
	return recognised;
}

bool D110AudioProcessor::tryAutoLoadRoms() {
	auto folder = getAutoRomFolder();
	if (!folder.isDirectory()) return false;

	// Recursive: an unpacked MAME romset sits in a sub-folder named after the set
	// (d110/), which is the layout the plugin's data folder normally has.
	for (const auto &entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles)) {
		auto file = entry.getFile();
		juce::MemoryBlock data;
		if (!file.loadFileAsData(data)) continue;

		MT32Emu::ROMInfo::Type type;
		if (!identifyRomData(data, type)) continue;

		if (type == MT32Emu::ROMInfo::Control && controlRomData.getSize() == 0) {
			controlRomData = std::move(data);
			controlRomPath = file.getFullPathName();
		} else if (type == MT32Emu::ROMInfo::PCM && pcmRomData.getSize() == 0) {
			pcmRomData = std::move(data);
			pcmRomPath = file.getFullPathName();
		}
	}

	// Nothing recognised as a whole image? The folder may still hold a MAME romset,
	// whose per-chip dumps have to be joined before mt32emu will know them. Also runs
	// whenever the BOSS ROM specifically is still missing: it is never a "whole image" as
	// far as identifyRomData() is concerned (it isn't Control or PCM), so the loop above
	// can never find it even when Control/PCM already came from whole-image files.
	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0 || bossRomData.getSize() == 0)
		tryAssembleRomsFromChipDumps(folder);

	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0) return false;
	return openSynthIfReady();
}

// A MAME d110 romset holds the D-110's chips separately, exactly as they sit on the
// board, while mt32emu wants the two images the board presents to the CPU. The joins
// below were verified byte for byte against the ROMs this plugin was already using:
//
//   Control = firmware (IC19, 32K) ++ presets (IC12, 128K)   -> SHA1 8d549f33...
//   PCM     = wave IC8 (512K)      ++ wave IC7 (512K)        -> SHA1 8eb2e385...
//
// Note the PCM order - IC8 first, then IC7. The other way round is not what mt32emu
// recognises. Chips are matched on content, so their filenames do not matter, and
// they may be loose in the folder or still inside the romset's .zip.
bool D110AudioProcessor::tryAssembleRomsFromChipDumps(const juce::File &folder) {
	struct Chip { const char *sha1; size_t size; };
	static const Chip kFirmwareV110 = { "28635510f30d6c1fb88e00da03e5b4e045c380cb", 32768 };
	static const Chip kFirmwareV106 = { "73b155fb0a8adc2362e73cb0803dafba9ccfb508", 32768 };
	static const Chip kPresets      = { "05587a0542b01625dcde37de5bb339880e47eb93", 131072 };
	static const Chip kWaveIc7      = { "6760d14900161b8715c2bfd4ebe997877087c90c", 524288 };
	static const Chip kWaveIc8      = { "9c59f50518a070461b2ec6cb4e43ee7cc1e905b6", 524288 };
	// IC6, the BOSS reverb chip's own program ROM (HG61H20R36F / BOS-007). Unlike the other
	// four chips this one is not folded into a Control/PCM image mt32emu already knows how
	// to identify - it goes to setBossReverbROM() separately, in openSynthIfReady() below.
	static const Chip kBoss         = { "17bd2887711c5c5458aba6d3be5972b2096eb450", 32768 };

	juce::MemoryBlock firmwareV110, firmwareV106, presets, waveIc7, waveIc8;

	auto consider = [&](const juce::MemoryBlock &data) {
		// mt32emu already carries a SHA1 implementation, so the chips are identified
		// with that rather than by adding another one here.
		MT32Emu::ArrayFile probe(static_cast<const MT32Emu::Bit8u *>(data.getData()), data.getSize());
		const juce::String digest(probe.getSHA1());
		auto take = [&](const Chip &chip, juce::MemoryBlock &into) {
			if (into.getSize() == 0 && data.getSize() == chip.size && digest == chip.sha1)
				into = data;
		};
		take(kFirmwareV110, firmwareV110);
		take(kFirmwareV106, firmwareV106);
		take(kPresets, presets);
		take(kWaveIc7, waveIc7);
		take(kWaveIc8, waveIc8);
		take(kBoss, bossRomData);
	};

	// Some ROM sets distribute two adjacent chips already concatenated into a single file -
	// most commonly the two PCM wave chips as one ~1MB dump, since that is the standard shape
	// PCM ROM images circulate in for MT-32/CM-32L (same physical chips this D-110 uses), quite
	// unlike MAME's own romsets, which always keep chips separate. Such a file matches neither
	// a whole-image checksum (identifyRomData(), above in tryAutoLoadRoms) nor a single chip's
	// size/SHA1 (consider(), above), so it silently went unrecognised. Splitting at every
	// boundary where the two halves' sizes match two known chips and re-running consider() on
	// each half recognises it exactly like MAME's separate dumps would - whichever order the
	// two chips were joined in, since consider() itself checks a half against every known chip
	// rather than assuming which one it must be.
	static const Chip *const kAllChips[] = {&kFirmwareV110, &kFirmwareV106, &kPresets,
	                                         &kWaveIc7,      &kWaveIc8,      &kBoss};
	auto considerConcatenated = [&](const juce::MemoryBlock &data) {
		for (const Chip *first : kAllChips) {
			if (first->size >= data.getSize()) continue;
			const size_t remainder = data.getSize() - first->size;
			for (const Chip *second : kAllChips) {
				if (second->size != remainder) continue;
				consider(juce::MemoryBlock(data.getData(), first->size));
				consider(juce::MemoryBlock(static_cast<const char *>(data.getData()) + first->size, remainder));
				return;
			}
		}
	};

	for (const auto &entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles)) {
		const auto file = entry.getFile();

		if (file.hasFileExtension("zip")) {
			juce::ZipFile zip(file);
			for (int i = 0; i < zip.getNumEntries(); ++i) {
				std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(i));
				if (stream == nullptr) continue;
				juce::MemoryBlock data;
				stream->readIntoMemoryBlock(data);
				consider(data);
				considerConcatenated(data);
			}
			continue;
		}

		juce::MemoryBlock data;
		if (file.loadFileAsData(data)) {
			consider(data);
			considerConcatenated(data);
		}
	}

	// v1.10 is the later firmware, so prefer it when both are present.
	const juce::MemoryBlock &firmware = firmwareV110.getSize() != 0 ? firmwareV110 : firmwareV106;

	if (controlRomData.getSize() == 0 && firmware.getSize() != 0 && presets.getSize() != 0) {
		juce::MemoryBlock joined(firmware);
		joined.append(presets.getData(), presets.getSize());
		MT32Emu::ROMInfo::Type type;
		if (identifyRomData(joined, type) && type == MT32Emu::ROMInfo::Control) {
			controlRomData = std::move(joined);
			controlRomPath = "(assembled from MAME chip dumps: firmware + presets)";
		}
	}

	if (pcmRomData.getSize() == 0 && waveIc7.getSize() != 0 && waveIc8.getSize() != 0) {
		juce::MemoryBlock joined(waveIc8);
		joined.append(waveIc7.getData(), waveIc7.getSize());
		MT32Emu::ROMInfo::Type type;
		if (identifyRomData(joined, type) && type == MT32Emu::ROMInfo::PCM) {
			pcmRomData = std::move(joined);
			pcmRomPath = "(assembled from MAME chip dumps: wave IC8 + IC7)";
		}
	}

	return controlRomData.getSize() != 0 && pcmRomData.getSize() != 0;
}

void D110AudioProcessor::materializeNativeRomFiles() {
	// controlRomData is always firmware (32768 bytes) followed by presets (131072 bytes) in
	// that order, whichever of the two paths above put it there - see the "Control = firmware
	// ++ presets" comment on tryAssembleRomsFromChipDumps.
	if (controlRomData.getSize() != 32768 + 131072) return;

	const auto folder = getAutoRomFolder();
	const auto firmwareFile = folder.getChildFile("d-110.v1.10.ic19.bin");
	const auto presetsFile = folder.getChildFile("r15179873-lh5310-97.ic12.bin");
	const auto *bytes = static_cast<const char *>(controlRomData.getData());
	if (!firmwareFile.existsAsFile()) firmwareFile.replaceWithData(bytes, 32768);
	if (!presetsFile.existsAsFile()) presetsFile.replaceWithData(bytes + 32768, 131072);
}

bool D110AudioProcessor::openSynthIfReady() {
	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0) {
		lastError = "Waiting for both Control ROM and PCM ROM to be selected.";
		return false;
	}

	lastError.clear();

	// The ROM bytes are held in memory rather than read through a FileStream, because a
	// Control or PCM image may have been assembled from a MAME romset's separate chip
	// dumps and so never exists as a file on disk in the form mt32emu wants.
	auto newControlFile = std::make_unique<MT32Emu::ArrayFile>(
		static_cast<const MT32Emu::Bit8u *>(controlRomData.getData()), controlRomData.getSize());
	auto newPcmFile = std::make_unique<MT32Emu::ArrayFile>(
		static_cast<const MT32Emu::Bit8u *>(pcmRomData.getData()), pcmRomData.getSize());

	const MT32Emu::ROMImage *newControlImage = MT32Emu::ROMImage::makeROMImage(newControlFile.get());
	const MT32Emu::ROMImage *newPcmImage = MT32Emu::ROMImage::makeROMImage(newPcmFile.get());

	if (newControlImage->getROMInfo() == nullptr) {
		lastError = "Control ROM not recognised (unexpected size/checksum).";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}
	if (newPcmImage->getROMInfo() == nullptr) {
		lastError = "PCM ROM not recognised (unexpected size/checksum).";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}

	auto newSynth = std::make_unique<MT32Emu::Synth>();
	// Must happen before open() - initReverbModels() decides right there whether to build
	// the engine's own four modes or all eight of the real chip's, and open() is what calls
	// it. Absent or the wrong size, this is silently a no-op and the engine falls back to
	// its own four modes exactly as it did before the BOSS ROM was wired in.
	if (bossRomData.getSize() != 0)
		newSynth->setBossReverbROM(static_cast<const MT32Emu::Bit8u *>(bossRomData.getData()),
		                            static_cast<MT32Emu::Bit32u>(bossRomData.getSize()));
	const bool useSuper = superModeParam != nullptr && superModeParam->load() > 0.5f;
	if (!newSynth->open(*newControlImage, *newPcmImage, kExtendedPartialCount,
						 MT32Emu::AnalogOutputMode_COARSE, useSuper)) {
		lastError = "Synth failed to open with these ROM files.";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}
	newSynth->setReverbEnabled(reverbEnabledParam == nullptr || reverbEnabledParam->load() > 0.5f);

	// Built here rather than via rebuildSampleRateConverter(), so the fully-formed engine -
	// synth AND converter together - is what gets handed over below, not the synth alone
	// followed by a gap before the converter catches up.
	auto newSampleRateConverter = std::make_unique<MT32Emu::SampleRateConverter>(
		*newSynth, currentSampleRate, MT32Emu::SamplerateConversionQuality_GOOD);

	const auto newControlRomDescription = newControlImage->getROMInfo()->description;
	const auto newPcmRomDescription = newPcmImage->getROMInfo()->description;

	// Everything above - ROM checksum/parsing, MT32Emu::Synth::open(), the sample-rate
	// converter - is the slow part, and none of it needs a lock: it's all local until here.
	// Only the handover does, because processBlock() (see synthAccessLock's declaration)
	// reads these same members under the same lock, for the whole of one block. Swap the new
	// engine in and the old one out here, then destroy the old one AFTER releasing the lock,
	// so a call arriving from the message thread (a live Super Mode toggle, see
	// superModeReopenPending in processBlock()) never makes the audio thread wait on
	// anything but the swap itself - not on freeing the engine it's replacing.
	std::unique_ptr<MT32Emu::Synth> oldSynth;
	std::unique_ptr<MT32Emu::SampleRateConverter> oldSampleRateConverter;
	std::unique_ptr<MT32Emu::ArrayFile> oldControlRomFile, oldPcmRomFile;
	const MT32Emu::ROMImage *oldControlImage = nullptr;
	const MT32Emu::ROMImage *oldPcmImage = nullptr;
	{
		const juce::ScopedLock sl(synthAccessLock);
		oldSynth = std::move(synth);
		oldSampleRateConverter = std::move(sampleRateConverter);
		oldControlRomFile = std::move(controlRomFile);
		oldPcmRomFile = std::move(pcmRomFile);
		oldControlImage = controlROMImage;
		oldPcmImage = pcmROMImage;

		controlRomFile = std::move(newControlFile);
		pcmRomFile = std::move(newPcmFile);
		controlROMImage = newControlImage;
		pcmROMImage = newPcmImage;
		synth = std::move(newSynth);
		sampleRateConverter = std::move(newSampleRateConverter);
		lastSuperModeApplied = useSuper;
		controlRomDescription = newControlRomDescription;
		pcmRomDescription = newPcmRomDescription;
		// synth->open() above just freshly decoded every PCM wave from the real ROM file,
		// wiping any custom sample from a previous session (or power cycle) - reapply while
		// still under the same lock, before anything on the audio thread can touch the new
		// synth's PCM data.
		reapplyCustomPcmWaves();
	}

	// Same order as closeSynth(): the synth before the ROM images it references, the images
	// before the file streams backing them. A side effect worth calling out: unlike the old
	// closeSynth()-first sequence, a failed reopen above (bad ROM data, synth->open()
	// failing) never reaches here, so the previous, working engine is left running rather
	// than torn down for nothing.
	oldSampleRateConverter.reset();
	oldSynth.reset();
	if (oldControlImage != nullptr) MT32Emu::ROMImage::freeROMImage(oldControlImage);
	if (oldPcmImage != nullptr) MT32Emu::ROMImage::freeROMImage(oldPcmImage);
	oldControlRomFile.reset();
	oldPcmRomFile.reset();

	return true;
}

void D110AudioProcessor::rebuildSampleRateConverter() {
	if (!synth) {
		sampleRateConverter.reset();
		return;
	}
	sampleRateConverter = std::make_unique<MT32Emu::SampleRateConverter>(
		*synth, currentSampleRate, MT32Emu::SamplerateConversionQuality_GOOD);
}

void D110AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
	currentSampleRate = sampleRate;
	interleavedScratch.resize(static_cast<size_t>(samplesPerBlock) * 2);
	midiRemapScratch.ensureSize(4096);
	portMidiScratch.ensureSize(4096);
	sequencerOutScratch.ensureSize(4096);
	sequencerClicks.reserve(64);
	osMidiCollector.reset(sampleRate);
	if (synth) rebuildSampleRateConverter();

	// Switched on by default, at the user's request: a rack unit you have to reach for and
	// flip on every time you open the project is a step nobody wants between loading a
	// session and hearing it. setPoweredOn(true) is idempotent - already-on is a no-op - so
	// calling it on every prepareToPlay (a host may call this more than once) is harmless,
	// and it fails exactly as a manual click would if the ROMs are missing or another
	// instance already holds the machine: poweredOn stays false, the panel shows POWER up,
	// and the reason is on the right-click menu, same as before.
	setPoweredOn(true);

	// A note reaching the firmware is not the same event as a note reaching the engine
	// directly: it goes processBlock -> D110CoreType::pushMidi()'s ring buffer -> the machine
	// thread's own 3125Hz timer, shifted in exactly as a real MIDI cable would, then the
	// firmware's own dispatch code before the sound is actually audible. Measured
	// (plugin/note_latency_probe.cpp, isolated single notes, no MIDI congestion): 0-18ms,
	// close to uniformly spread rather than clustered around one number - the audio thread
	// and the machine thread are genuinely two different clocks, and this plugin was
	// reporting zero latency to the host regardless, so a DAW's own delay compensation
	// never accounted for any of it.
	//
	// setLatencySamples() cannot make the jitter itself disappear - PDC shifts everything
	// else by a FIXED amount, and the true delay here varies note to note. What it can fix
	// is the SYSTEMATIC part: without it, this plugin's audio was always emitted late
	// relative to a perfectly compensated track, never early. Reporting one block's worth
	// centres that jitter on the grid instead of trailing behind it - one block because
	// that quantisation (host hands us MIDI once per callback; the firmware's clock is not
	// the audio thread's clock) is the one piece of this delay that is not itself random.
	setLatencySamples(samplesPerBlock);

#ifdef D110_HAVE_JACK_MIDI
	// Standalone only - see JackMidiInput.h's own comment on why. Done here rather than in
	// the constructor on the same once-only pattern setPoweredOn(true) above already uses,
	// for a host that calls prepareToPlay() more than once. A plain literal rather than
	// JucePlugin_Name: that macro only exists for the actual plugin-wrapper targets (VST3/
	// Standalone), not for the plain add_executable probes that also link this file.
	if (!jackMidiSetupAttempted) {
		jackMidiSetupAttempted = true;
		if (wrapperType == wrapperType_Standalone) {
			jackMidiIn = std::make_unique<JackMidiInput>(
				"D-110 Emulator", [this](const juce::MidiMessage &m) { handleIncomingMidiMessage(nullptr, m); });
		}
	}
#endif
}

void D110AudioProcessor::releaseResources() {
}

void D110AudioProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
	juce::ScopedNoDenormals noDenormals;
	const int numSamples = buffer.getNumSamples();
	buffer.clear();

	// Rechannelize host-fed external MIDI (a DAW routing a real controller to this plugin's
	// MIDI input, in VST3) onto the on-screen keyboard's own selected channel - Alan's own USB
	// keyboard is hardwired to channel 1, so this is what lets him record other sequencer
	// tracks/parts just by changing the on-screen keyboard's channel, without a controller
	// that can actually transmit on other channels. With midiRemap off, this passes through
	// unchanged - there is no single target channel to remap to. Only channel-voice messages
	// (getChannel() > 0) are touched, matching the same idiom the sequencer's own
	// channelForTrack rechannelling already uses. This has to run before anything below adds
	// the on-screen keyboard's OWN notes (injectTestNote, merged in from osMidiCollector just
	// below) into midiMessages - those are already explicitly channelized (or looped across
	// all 16 channels when midiRemap is off) at the point of injection and must not be touched
	// here. The Standalone app's own directly-opened MIDI port is handled the same way, but
	// earlier - see handleIncomingMidiMessage(), which rechannelizes before a message ever
	// reaches osMidiCollector in the first place.
	if (midiRemap) {
		// Member scratch buffers (midiRemapScratch/portMidiScratch/sequencerOutScratch, pre-sized
		// in prepareToPlay) rather than locals: a local juce::MidiBuffer allocates on the audio
		// thread every block. swapWith() exchanges storage, so both sides keep their capacity.
		midiRemapScratch.clear();
		for (const auto meta : midiMessages) {
			auto msg = meta.getMessage();
			if (msg.getChannel() > 0) msg.setChannel(keyboardMidiChannel);
			midiRemapScratch.addEvent(msg, meta.samplePosition);
		}
		midiMessages.swapWith(midiRemapScratch);
	}

	// Anything that arrived on the directly-opened port joins the host's own stream here,
	// so from this point on there is one queue and the rest of the method cannot tell the
	// two apart - which is right, since the hardware cannot either.
	{
		portMidiScratch.clear();
		osMidiCollector.removeNextBlockOfMessages(portMidiScratch, numSamples);
		for (const auto meta : portMidiScratch)
			midiMessages.addEvent(meta.getMessage(), meta.samplePosition);
	}

	// --- D-20-style sequencer: capture into the armed track, then layer in playback -----
	// Done here, ahead of the synth/power guard just below, so the transport's own clock
	// keeps ticking regardless of power state - matching real hardware, where a sequencer
	// isn't gated by anything else. Recorded and played-back notes are added straight into
	// midiMessages, so the streaming loop further down (and the firmware it feeds) cannot
	// tell them apart from host MIDI - same reasoning as the osMidiCollector merge above.
	// sequencerClicks is filled here but mixed into the OUTPUT buffer much further down,
	// after the synth has actually rendered into it.
	// Member (capacity kept across blocks), cleared here rather than a fresh local vector.
	sequencerClicks.clear();
	{
		// Refreshed once per block, not once per note - see sequencerLiveChannels' comment.
		if (core.isRunning()) {
			if (sequencerRamScratch.size() != static_cast<size_t>(D110CoreType::kRamSize))
				sequencerRamScratch.resize(static_cast<size_t>(D110CoreType::kRamSize));
			if (core.getRam(sequencerRamScratch.data())) {
				// Rhythm's own record (index kRhythmTrack) is included now too, for its LEVEL/PAN
				// (2026-08-21, Alan's request: a fixed default Volume for the rhythm track) - its
				// channel entry is still skipped below and stays fixed at 10, unrelated to this.
				for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t) {
					if (t != d110seq::D110SequencerEngine::kRhythmTrack) {
						const size_t chAddr = static_cast<size_t>(D110CoreType::kRamSystem + 13 + t);
						const int raw = chAddr < sequencerRamScratch.size()
						                    ? static_cast<int>(sequencerRamScratch[chAddr])
						                    : -1;
						// 0..15 = channel 1..16; 16 = the part is OFF, and anything else means
						// the read didn't land where expected - both fall back to the engine's
						// own default (channelForTrack) by staying -1.
						sequencerLiveChannels[static_cast<size_t>(t)] = (raw >= 0 && raw <= 15) ? raw + 1 : -1;
					}

					// TimbreTemp field 0 = TONE GROUP (0=preset A, 1=preset B, 2=internal tone
					// memory, 3=rhythm - see D110EditorPane's own "TONE GROUP" column), field 1
					// = TONE, the number within that group (0-63) - see sequencerLivePrograms'
					// own comment for how these two fold into the single Program Change value a
					// real Program Change on this part's channel would actually have to be to
					// reproduce what's playing right now.
					const size_t timbreTempAddr = static_cast<size_t>(D110CoreType::kRamTimbreTemp)
					                             + static_cast<size_t>(t) * D110CoreType::kTimbreTempRecord;
					const int group = timbreTempAddr < sequencerRamScratch.size()
					                       ? static_cast<int>(sequencerRamScratch[timbreTempAddr])
					                       : -1;
					const int tone = timbreTempAddr + 1 < sequencerRamScratch.size()
					                      ? static_cast<int>(sequencerRamScratch[timbreTempAddr + 1])
					                      : -1;
					// Only preset A/B are reachable by a plain Program Change at all (see
					// setTrackProgram()'s own comment) - group 2/3 (internal tone memory,
					// rhythm) have no such number, so they report "no hint", same as an
					// unreadable byte would.
					sequencerLivePrograms[static_cast<size_t>(t)] =
						(tone >= 0 && tone <= 63 && (group == 0 || group == 1)) ? (group == 1 ? 64 + tone : tone)
						                                                        : -1;

					// Same record, fields 8/9 (LEVEL/PAN) - what captureLivePatchIntoTracks()
					// reads to fill in the sequencer's own per-track Volume/Pan.
					sequencerLiveVolumes[static_cast<size_t>(t)] =
						timbreTempAddr + 8 < sequencerRamScratch.size()
							? static_cast<int>(sequencerRamScratch[timbreTempAddr + 8])
							: -1;
					sequencerLivePans[static_cast<size_t>(t)] =
						timbreTempAddr + 9 < sequencerRamScratch.size()
							? static_cast<int>(sequencerRamScratch[timbreTempAddr + 9])
							: -1;

					// group 2 (Internal) is exactly the case sequencerLivePrograms above can't
					// give a Program Change hint for - snapshot the Tone Memory slot it's
					// actually using instead, for buildTrackSysExPreamble() to embed into a MIDI
					// export in place of a Program Change a real unit could never reach either.
					const size_t toneAddr = static_cast<size_t>(D110CoreType::kRamTones)
					                       + static_cast<size_t>(tone) * D110CoreType::kToneMemRecord;
					if (group == 2 && tone >= 0 && tone <= 63
					    && toneAddr + D110CoreType::kToneMemRecord <= sequencerRamScratch.size()) {
						sequencerLiveInternalTone[static_cast<size_t>(t)] = tone;
						std::memcpy(sequencerLiveToneMemory[static_cast<size_t>(t)].data(),
						            sequencerRamScratch.data() + toneAddr, D110CoreType::kToneMemRecord);
					} else {
						sequencerLiveInternalTone[static_cast<size_t>(t)] = -1;
					}
				}
			}
		}

		const double beatsPerSample = (sequencerEngine.getTempo() / 60.0) / currentSampleRate;
		const double windowStartBeats = sequencerEngine.getPositionBeats();
		const int armed = sequencerEngine.isRecording() ? sequencerEngine.getArmedTrack() : -1;
		if (armed >= 0) {
			const int armedChannel = sequencerEngine.channelForTrack(armed);
			for (const auto meta : midiMessages) {
				const auto &msg = meta.getMessage();
				if (msg.isNoteOnOrOff() && msg.getChannel() == armedChannel)
					sequencerEngine.captureEvent(
						msg, windowStartBeats + static_cast<double>(meta.samplePosition) * beatsPerSample);
			}
		}

		// Step recording doesn't care about beat position (see stepNoteOn()'s own comment), only
		// which notes were played and released - same armed-channel gating as real-time capture
		// above, just routed to the step API instead. Notes stay in midiMessages either way, so
		// they're still audible through the firmware while being entered, same as a real-time take.
		const int stepArmed = sequencerEngine.isStepRecording() ? sequencerEngine.getArmedTrack() : -1;
		if (stepArmed >= 0) {
			const int armedChannel = sequencerEngine.channelForTrack(stepArmed);
			for (const auto meta : midiMessages) {
				const auto &msg = meta.getMessage();
				if (msg.getChannel() != armedChannel) continue;
				if (msg.isNoteOn()) sequencerEngine.stepNoteOn(msg.getNoteNumber(), msg.getVelocity());
				else if (msg.isNoteOff()) sequencerEngine.stepNoteOff(msg.getNoteNumber());
			}
		}

		// Rendered into its own buffer first, rather than straight into midiMessages, so the
		// notes the sequencer actually played (as opposed to host/keyboard/thru input already
		// sitting in midiMessages) can be told apart and also reach the direct MIDI Out port
		// below - the first step towards the sequencer driving external gear on its own.
		juce::MidiBuffer &sequencerOut = sequencerOutScratch;
		sequencerOut.clear();

		// Per-Part Program Change/Bank/Volume/Pan override (see getTrackProgram()/getTrackBank()/
		// getTrackVolume()/getTrackPan()), added ahead of renderInto() below so it lands at
		// sample 0, on the PLAY/REC edge (precount included - same reasoning as
		// NonetSeqHost::advance(), the D-110's own patch should be settled before any note this
		// block might also render) - OR right now, regardless of transport state, if
		// resyncProgramChanges() asked for it (Alan's "the live sound and the sequencer's stored
		// settings have drifted apart, force them back in sync" escape hatch, 2026-08-19) - the
		// audio thread is the only place allowed to touch the firmware/sound-engine bridge, so
		// that method can only flag the request, this is where it's actually acted on.
		//
		// No Bank Select (CC0) here - the D-110 predates that convention and its firmware
		// simply doesn't implement it (confirmed against selectTimbreForPart(), the TIMBRES
		// tab's own proven-correct mechanism, which never sends one either). Its 128 Timbre
		// Memory slots are reached purely by Program Change value, split into two pages of 64
		// Roland's own panel/manual call "A" and "B" - BANK folds straight into that same
		// number instead: BANK 1/PROGRAM 1-128 addresses a slot directly (the flat numbering
		// the TIMBRES tab itself uses), BANK 2/PROGRAM 1-64 addresses page B's own 1-64 (the
		// "B31" naming Alan reads off the instrument), and anything past 127 just clamps.
		//
		// Volume/Pan go out as real Part LEVEL/PAN SysEx (sendTimbreTempParam, fields 8/9 -
		// exactly what the PARTS tab's own LEVEL/PAN columns write), not MIDI CC7/CC10 - see
		// D110SequencerHost.h's own comment on why.
		const bool nowSequencerPlaying = sequencerEngine.isPlaying();
		const bool doProgramResync = sequencerResyncRequested.exchange(false);
		if ((nowSequencerPlaying && !wasSequencerPlayingForProgramSend) || doProgramResync) {
			// Rhythm (index kRhythmTrack) is included for Volume/Pan only, not Program/Bank -
			// it has no Program Change equivalent (see supportsProgramChangeForTrack()), but its
			// own TimbreTemp record's LEVEL/PAN is exactly as real as any melodic Part's (2026-08-21,
			// Alan's request: a fixed default Volume for the rhythm track).
			for (int t = 0; t <= d110seq::D110SequencerEngine::kRhythmTrack; ++t) {
				if (t < d110seq::D110SequencerEngine::kRhythmTrack) {
					const int program = sequencerEngine.getTrackProgram(t);
					if (program >= 0) {
						const int channel = sequencerEngine.channelForTrack(t);
						const int bank = sequencerEngine.getTrackBank(t);
						const int rawProgram = juce::jlimit(0, 127, (bank - 1) * 64 + program);
						sequencerOut.addEvent(juce::MidiMessage::programChange(channel, rawProgram), 0);
					}
				}
				const int volume = sequencerEngine.getTrackVolume(t);
				if (volume >= 0) sendTimbreTempParam(t, 8, static_cast<juce::uint8>(volume));
				const int pan = sequencerEngine.getTrackPan(t);
				if (pan >= 0) sendTimbreTempParam(t, 9, static_cast<juce::uint8>(pan));
			}
		}
		wasSequencerPlayingForProgramSend = nowSequencerPlaying;

		sequencerEngine.renderInto(sequencerOut, numSamples, currentSampleRate,
		                            sequencerEngine.getMetronomeEnabled() ? &sequencerClicks : nullptr);
		for (const auto meta : sequencerOut)
			midiMessages.addEvent(meta.getMessage(), meta.samplePosition);

		// Sent straight from the audio thread rather than queued for a background one: a real
		// MIDI write is a handful of bytes, and every backend this targets (ALSA sequencer on
		// Linux, CoreMIDI, WinMM) hands it off without blocking on note-rate traffic like this.
		const juce::ScopedLock midiOutLock(osMidiLock);
		if (osMidiOut != nullptr && sequencerOut.getNumEvents() > 0)
			osMidiOut->sendBlockOfMessagesNow(sequencerOut);
	}

	// Held for the rest of this block: openSynthIfReady() (below) can run concurrently on the
	// message thread and swaps `synth`/`sampleRateConverter` out from under us when Super
	// Mode is toggled live. It only ever costs this an O(1) pointer handover, never the
	// (slow) rebuild itself - see synthAccessLock's declaration.
	const juce::ScopedLock sl(synthAccessLock);

	if (!synth || !sampleRateConverter || !poweredOn.load()) {
		return;
	}

	// Super Mode can only be applied when the synth is (re)opened, not live. Rebuilding it -
	// ROM checksum/parsing plus a full MT32Emu::Synth::open() - is far too slow for the audio
	// thread's deadline: doing it right here, synchronously, is what actually produced the
	// xrun-then-the-host-kills-the-plugin failure this comment used to just warn about. Only
	// the mismatch is detected here; the rebuild itself runs on the message thread via
	// openSynthIfReady(), guarded by superModeReopenPending so a run already in flight isn't
	// queued a second time. This block, and any others before it lands, keep rendering with
	// whichever engine is currently installed - a toggle takes a few blocks to catch up
	// rather than landing instantly, which is the same "not live" limitation this always had.
	const bool wantSuper = superModeParam->load() > 0.5f;
	if (wantSuper != lastSuperModeApplied && !superModeReopenPending.exchange(true)) {
		std::weak_ptr<char> weakLife = lifeToken;
		juce::MessageManager::callAsync([this, weakLife] {
			if (weakLife.expired()) return; // the processor was destroyed before this ran
			openSynthIfReady();
			superModeReopenPending = false;
		});
	}

	synth->setReverbEnabled(reverbEnabledParam->load() > 0.5f);

	// Drain anything queued from the UI thread (SysEx bank imports, patch-browsing program
	// changes, stub-button LCD messages) and actually apply it here on the audio thread, since
	// Synth's MIDI queue only tolerates a single writer thread.
	// The three *ToSend vectors are members, cleared at the end of this block: swapping a
	// member (empty, capacity retained) with the pending queue hands the queue an already
	// allocated buffer back, so steady state never allocates or frees on the audio thread -
	// a fresh local vector would free its storage on every block.
	auto &pendingImportsToSend = pendingImportsToSendScratch;
	auto &pendingShortMessagesToSend = pendingShortMessagesToSendScratch;
	auto &pendingPanicBytesToSend = pendingPanicBytesToSendScratch;
	bool forceReleaseStuckVoicesNow = false;
	{
		const juce::ScopedLock slEngine(engineActionLock);
		pendingImportsToSend.swap(pendingSysexImports);
		pendingShortMessagesToSend.swap(pendingShortMessages);
		pendingPanicBytesToSend.swap(pendingPanicBytes);
		forceReleaseStuckVoicesNow = pendingForceReleaseStuckVoices;
		pendingForceReleaseStuckVoices = false;
	}
	if (!pendingPanicBytesToSend.empty())
		core.pushMidi(pendingPanicBytesToSend.data(), static_cast<int>(pendingPanicBytesToSend.size()));
	// midiPanic()'s own requests - see D110CoreNative::releaseStuckNoteContexts()/
	// resetVoiceSlotTable()'s own comments. Only safe here, on the audio thread, same reason
	// every other `core.` touch above is queued rather than called straight from midiPanic()
	// itself (the message thread).
	if (forceReleaseStuckVoicesNow) core.releaseStuckNoteContexts();
	// resetVoiceSlotTable() repeats for a short window, not just once - see its own comment for
	// why a single call loses a race with the firmware's own delayed response to the panic's
	// own CC64/CC123 bytes still being paced out above.
	if (pendingSlotTableResetSamplesRemaining.load(std::memory_order_relaxed) > 0) {
		core.resetVoiceSlotTable();
		pendingSlotTableResetSamplesRemaining.fetch_sub(buffer.getNumSamples(), std::memory_order_relaxed);
	}
	// These two queues deliberately go through the engine's timestamped queue rather than the
	// "immediate" forms the bridge below uses: their source is a user action on the panel,
	// seconds away from any note, and overtaking by one block settles nothing here. The bridge
	// is different - there a parameter and a note arrive in the same millisecond, and their
	// order matters.
	for (auto &message : pendingImportsToSend) {
		synth->playSysex(message.data(), static_cast<MT32Emu::Bit32u>(message.size()));
		// And to the control board, which is the half that actually owns this data. Sending
		// it only to the sound engine looked like it worked and did nothing: the firmware
		// never learned of the new patches, so the next RAM mirror overwrote them with the
		// firmware's unchanged state and the display never moved either. Pushed from here
		// rather than from importSysexBank because the ring has one producer by design,
		// and that producer is this thread.
		core.pushMidi(message.data(), static_cast<int>(message.size()));
	}
	for (auto message : pendingShortMessagesToSend)
		synth->playMsg(message);
	pendingImportsToSend.clear();
	pendingShortMessagesToSend.clear();
	pendingPanicBytesToSend.clear();

	// The bridge: whenever the control board's own parameter memory changes - because a
	// button was pressed on the panel - the core hands us the Roland exclusive message
	// the hardware would have used, and the LA engine takes it natively. This is what
	// makes an edit made on the panel audible.
	//
	// playSysexNow, not playSysex: the latter puts the message into the engine's own timestamped
	// queue, applied only when rendering reaches it - whereas the notes below go through
	// playMsgOnPart, which acts IMMEDIATELY. Notes were overtaking the parameters they depend
	// on. At the start of the demo song this cost two hits: the firmware loaded the rhythm map
	// and almost at once struck keys 25 and 27, but the engine applied the hits before the map,
	// saw timbre 127 (OFF) there and silently dropped them ("Attempted to play unmapped key").
	// Here both paths act at queue-drain time, and the order of this loop - parameters, then
	// notes - becomes real.
	{
		MT32Emu::Bit8u sysex[D110CoreType::kMaxSysexBytes];
		while (const int len = core.popSysex(sysex))
			synth->playSysexNow(sysex, static_cast<MT32Emu::Bit32u>(len));
	}

	// The other half of the same idea, for notes rather than parameters: the firmware tells
	// us which note it started on which part, and the engine plays it. This is what makes
	// the instrument's OWN demo songs audible - it generates those internally from ROM and
	// never transmits them, so before this they moved the part indicators in silence. It
	// also means host notes are voiced by the same path the panel is showing, with the
	// firmware's own key ranges, part assignment and voice allocation applied, instead of
	// the two halves being fed separately and drifting apart.
	//
	// playMsgOnPart addresses the part directly (0-7 voice, 8 rhythm), which is exactly
	// what the firmware hands over, so no channel-to-part mapping has to be re-derived here.
	// --- diagnostic instrumentation (2026-08-07), gated by debugModeEnabled (Utility tab ->
	// DEBUG) - off by default so nothing runs or hits disk on the audio thread unless a user
	// deliberately turns it on to help chase a real issue. Originally written to find the
	// note-dropout bug (see project_note_dropout_root_cause memory) and kept, rather than
	// deleted, in case a similar issue needs the same kind of measurement again. Two earlier
	// versions of the tracking below got the measurement wrong in opposite directions: v1 only
	// logged FAILURES (busy>0 while activePartials==0), which made a roughly-50%-of-notes
	// problem look like a permanent stuck state (successes were never written down at all);
	// v2 fixed that but checked the GLOBAL activePartials count, which meant ANY other
	// part/note still ringing at the same moment masked a specific note's own failure. This
	// version tracks each individual note-on PER PART (right where ev.part is already known,
	// below) and checks THAT part's own bit in enginePartStates() a few blocks later, so one
	// part's success can no longer hide another's failure.
	{
		D110CoreType::NoteEvent ev;
		while (core.popNoteEvent(ev)) {
			if (ev.part > 8) continue;
			synth->playMsgOnPart(ev.part, ev.on ? 0x9 : 0x8, ev.note, ev.velocity);
			if (debugModeEnabled && ev.on) diagPendingChecks.push_back({ ev.part, 3 });
		}
	}
	if (debugModeEnabled && core.isRunning()) {
		const uint32_t partStates = enginePartStates();
		for (size_t i = 0; i < diagPendingChecks.size();) {
			if (--diagPendingChecks[i].checksLeft <= 0) {
				++diagAttempts;
				if ((partStates >> diagPendingChecks[i].part) & 1u) ++diagHits; else ++diagMisses;
				diagPendingChecks.erase(diagPendingChecks.begin() + static_cast<long>(i));
			} else {
				++i;
			}
		}

		const juce::int64 nowMs = juce::Time::getMillisecondCounter();
		if (diagAttempts > 0 && nowMs - diagLastFlushMs > 3000) {
			diagLastFlushMs = nowMs;
			juce::File logFile = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
			                          .getChildFile("d110_diagnostic_log.txt");
			juce::String line;
			line << juce::Time::getCurrentTime().toString(true, true, true, true)
			     << "  TALLY (per-part)  attempts=" << diagAttempts << " hits=" << diagHits
			     << " misses=" << diagMisses << " ("
			     << juce::String(diagAttempts > 0 ? 100.0 * diagMisses / diagAttempts : 0.0, 1)
			     << "% missed)  enginePartStates=0x" << juce::String::toHexString(int(partStates))
			     << "  abortFallbackCount=" << juce::int64(engineAbortFallbackCount())
			     << "  serialOverrunCount=" << juce::int64(core.serialOverrunCount()) << juce::newLine;
			MidiLogEntry log[16];
			const int n = getMidiLog(log, 16);
			for (int i = 0; i < n; ++i)
				line << "    recent MIDI: status=0x" << juce::String::toHexString(log[i].status)
				     << " data1=" << int(log[i].data1) << " data2=" << int(log[i].data2) << " size="
				     << log[i].size << juce::newLine;
			logFile.appendText(line);
		}
	}
	// --- end diagnostic instrumentation ---

	if (static_cast<int>(interleavedScratch.size()) < numSamples * 2)
		interleavedScratch.resize(static_cast<size_t>(numSamples) * 2);

	const float masterVolume = masterVolumeParam->load();
	const int numOutChannels = buffer.getNumChannels();

	// The six INDIVIDUAL buses are disabled by default (createBuses()), so most hosts and
	// most projects never pay for this at all: MULTI_OUTPUT_STREAMS below only gets built,
	// and renderD110MultiOutput() only gets called, when a host has actually connected at
	// least one of them. Otherwise this takes the exact code path it always did.
	bool multiOutputActive = false;
	float *individualChannel[6] = {};
	for (int output = 0; output < 6; ++output) {
		auto bus = getBusBuffer(buffer, false, output + 1);
		if (bus.getNumChannels() > 0) {
			individualChannel[output] = bus.getWritePointer(0);
			multiOutputActive = true;
		}
	}

	auto midiIterator = midiMessages.cbegin();
	const auto midiEnd = midiMessages.cend();
	int samplePos = 0;

	while (samplePos < numSamples) {
		while (midiIterator != midiEnd && (*midiIterator).samplePosition <= samplePos) {
			handleIncomingMidiMessage((*midiIterator).getMessage());
			++midiIterator;
		}

		int nextEventSample = numSamples;
		if (midiIterator != midiEnd)
			nextEventSample = juce::jmin(numSamples, (*midiIterator).samplePosition);

		const int samplesToRender = nextEventSample - samplePos;
		if (samplesToRender > 0) {
			if (multiOutputActive) {
				MT32Emu::D110MultiOutputStreams streams = {};
				streams.mixStereo = interleavedScratch.data();
				for (int output = 0; output < 6; ++output)
					streams.individualMono[output] =
						individualChannel[output] != nullptr ? individualChannel[output] + samplePos : nullptr;
				synth->renderD110MultiOutput(streams, static_cast<MT32Emu::Bit32u>(samplesToRender));
			} else {
				sampleRateConverter->getOutputSamples(interleavedScratch.data(),
													   static_cast<unsigned int>(samplesToRender));
			}
			float *left = buffer.getWritePointer(0, samplePos);
			float *right = numOutChannels > 1 ? buffer.getWritePointer(1, samplePos) : nullptr;
			// DAC saturation first, and only then the volume knob - that is the order they sit in on the
			// unit: the DAC is sixteen-bit and physically cannot output more than full scale, and the
			// volume control is analogue and comes after it.
			//
			// The saturation itself belongs to the engine: Synth::clipSampleEx(Bit32s) pins the voice
			// sum to +-32767. But the same function has a float overload, and it DELIBERATELY does
			// nothing - `return sampleEx;`. Our build goes down the float path, so the DAC model never
			// reached the output, and the dense passages of the demo song went about 1.5 dB over full
			// scale. That is a departure from the unit, not its character: on "Macho Memory" 69 samples
			// out of 6,614,016 went over full scale, one thousandth of a percent - the DAC would have
			// clipped them inaudibly.
			//
			// The panel knob runs 0..2 with unity gain exactly at the middle, where it sits by default,
			// so at rest the output can no longer exceed full scale. Anything right of the middle is
			// gain the user asked for, like a fader on a desk, not the unit's behaviour.
			for (int i = 0; i < samplesToRender; ++i) {
				const float l = juce::jlimit(-1.0f, 1.0f,
				                            interleavedScratch[static_cast<size_t>(i) * 2]);
				const float r = juce::jlimit(-1.0f, 1.0f,
				                            interleavedScratch[static_cast<size_t>(i) * 2 + 1]);
				left[i] = l * masterVolume;
				if (right != nullptr) right[i] = r * masterVolume;
			}
		}
		samplePos = nextEventSample;
	}

	// Any remaining events exactly at the end of the block still need to be delivered so
	// they're not lost before the next processBlock call.
	while (midiIterator != midiEnd) {
		handleIncomingMidiMessage((*midiIterator).getMessage());
		++midiIterator;
	}

	// Sequencer metronome: mixed straight into the output rather than routed through the
	// firmware/LA32 engine, since it's a transport aid and not part of the instrument's own
	// sound - deliberately outside the DAC-saturation/VOLUME-knob scaling above. State
	// (phase, remaining samples) is carried in member variables because a click's short
	// decay can outlast the block it started in.
	// Skipped entirely when the metronome is routed through the rhythm channel instead (see
	// D110SequencerEngine::getMetronomeUseChannel10()) - that real MIDI note IS the click
	// sound in that mode, and mixing this synthesized one in too would just double it.
	//
	// The metronomeSamplesRemaining > 0 half of this condition matters even on blocks with
	// no NEW click (sequencerClicks empty): the click's ~30ms decay is longer than a typical
	// audio block (512 samples is ~11ms at 44.1kHz), so it almost always outlives the block
	// it started in. Gating this purely on "a click started this block" - the previous
	// version - meant the decay's countdown simply froze on every block that had no new
	// click of its own, then resumed (and finished, all at once) whenever the NEXT beat's
	// click came in and reopened this branch - i.e. the tail end of one beat's click played
	// back-to-back with the very start of the next beat's, audible as the first beat's sound
	// also sounding on the second beat. Rendering (and counting down) on every block once a
	// click is in progress, not just blocks that start one, is what actually lets it decay
	// smoothly across block boundaries instead of pausing and dumping the rest late.
	if ((!sequencerClicks.empty() || metronomeSamplesRemaining > 0) && !sequencerEngine.getMetronomeUseChannel10()) {
		constexpr double kClickSeconds = 0.03;
		const int clickTotalSamples = juce::jmax(1, static_cast<int>(currentSampleRate * kClickSeconds));
		const float volume = sequencerEngine.getMetronomeVolume();
		int cursor = 0;
		auto ringUpTo = [&](int endSample) {
			for (int i = cursor; i < endSample; ++i) {
				if (metronomeSamplesRemaining <= 0) continue;
				const float amp = static_cast<float>(metronomeSamplesRemaining) / static_cast<float>(clickTotalSamples);
				const float s = std::sin(metronomePhase) * amp * 0.25f * volume;
				metronomePhase += juce::MathConstants<double>::twoPi * metronomeFreq / currentSampleRate;
				for (int ch = 0; ch < numOutChannels; ++ch) buffer.addSample(ch, i, s);
				--metronomeSamplesRemaining;
			}
		};
		for (const auto &click : sequencerClicks) {
			ringUpTo(click.samplePosition);
			cursor = click.samplePosition;
			metronomeSamplesRemaining = clickTotalSamples;
			metronomeFreq = click.downbeat ? 1500.0 : 1000.0;
			metronomePhase = 0.0;
		}
		ringUpTo(numSamples);
	}

#ifdef D110_NATIVE_CORE
	// The MAME-backed D110Core advances on its own real-time thread regardless of whether
	// processBlock() is even being called, which is exactly the two-clocks problem this whole
	// port exists to remove - so nothing in the original code ever had to step it forward from
	// here. D110CoreNative has no thread of its own: it only advances when explicitly told to,
	// and this is that call. Placed at the END of the block, after every host MIDI event above
	// has already reached core.pushMidi() (inside forwardMidiToFirmware(), called from
	// handleIncomingMidiMessage()) - so this step is what actually processes everything this
	// block just queued, and the results (popSysex/popNoteEvent, drained at the TOP of this
	// function) surface on the NEXT processBlock call. That's the measured, fixed one-block
	// latency (native_note_latency_probe.cpp: 11.61ms, zero jitter across 15 trials) -
	// setLatencySamples(samplesPerBlock) in prepareToPlay() already reports and compensates
	// for exactly this.
	core.runForSeconds(double(numSamples) / currentSampleRate);
#endif
}

void D110AudioProcessor::handleIncomingMidiMessage(const juce::MidiMessage &message) {
	// On-screen keyboard's remote-activity LEDs - see D110KeyboardHost::isNoteActive(). This
	// runs once per event in the merged stream (see processBlock()'s own comment on why this
	// one function already sees everything: host MIDI, the Standalone port, the on-screen
	// keyboard's own notes, and sequencer playback), so a single check here covers all of
	// them without needing a separate hook at each of those sources.
	if (message.isNoteOnOrOff()) {
		const int note = message.getNoteNumber();
		if (note >= 0 && note < 128) remoteNoteActive[static_cast<size_t>(note)].store(message.isNoteOn());
	}

	// The control board gets its own copy first. On the real instrument one MIDI cable
	// feeds both the CPU and the voice circuitry; here the CPU is MAME and the voice
	// circuitry is mt32emu, so both have to be told. Without this the firmware never
	// learns a note was played - which is why the top LCD row did not light the playing
	// parts, and why the display drifted away from the host's program changes.
	forwardMidiToFirmware(message);

	// And into the log the extended editor's MONITOR tab shows: exactly what the unit received,
	// before any parsing.
	logIncomingMidi(message);

	// Echoed to a real MIDI Out port too, if one's been opened (setMidiOutputDevice) - Alan's
	// request, 2026-08-22 (Android): drive an external hardware synth off whatever the D-110
	// itself is playing, from any of the sources this same function already merges (on-screen
	// keyboard, a USB MIDI controller, file/sequencer playback, a DAW host). osMidiOut is null
	// unless a host explicitly opens one, so this is a no-op everywhere it isn't wired up -
	// same direct-from-the-audio-thread send the sequencer's own output and midiPanic() already
	// use (a MIDI write is a handful of bytes; every backend here hands it off without
	// blocking), just guarded by the same lock since a UI action can change osMidiOut
	// concurrently with this running.
	{
		const juce::ScopedLock midiOutLock(osMidiLock);
		if (osMidiOut != nullptr) osMidiOut->sendMessageNow(message);
	}

	if (!synth) return;

	if (message.isSysEx()) {
		synth->playSysex(message.getRawData(), static_cast<MT32Emu::Bit32u>(message.getRawDataSize()));
		return;
	}

	const auto *raw = message.getRawData();
	const auto size = message.getRawDataSize();
	if (size <= 0) return;

	// When the firmware is voicing the notes, it must not be done twice. The firmware has
	// already been handed this message above; it will allocate a voice and report the note
	// back through popNoteEvent(), which is where it reaches the engine. Sending it here as
	// well would play every note twice - once with the firmware's own key range, transpose
	// and part assignment applied, once without. Everything that is NOT a note still goes
	// straight through, because the engine needs controllers, bend and program changes in
	// its own right.
	if (forwardNotes.load(std::memory_order_relaxed) && core.isRunning() &&
	    (message.isNoteOnOrOff() || message.isAftertouch()))
		return;

	const juce::uint8 status = raw[0];
	const juce::uint8 data1 = size > 1 ? raw[1] : 0;
	const juce::uint8 data2 = size > 2 ? raw[2] : 0;

	const MT32Emu::Bit32u msg = static_cast<MT32Emu::Bit32u>(status)
		| (static_cast<MT32Emu::Bit32u>(data1) << 8)
		| (static_cast<MT32Emu::Bit32u>(data2) << 16);
	synth->playMsg(msg);
}

// ---- MIDI ports opened directly, beside the host's own routing ---------------

void D110AudioProcessor::setMidiInputDevice(const juce::String &id) {
	if (osMidiIn) { osMidiIn->stop(); osMidiIn.reset(); }
	selInputId = {};
	if (id.isEmpty()) return;
	osMidiIn = juce::MidiInput::openDevice(id, this);
	if (osMidiIn) { osMidiIn->start(); selInputId = id; }
}

void D110AudioProcessor::setMidiOutputDevice(const juce::String &id) {
	std::unique_ptr<juce::MidiOutput> opened;
	if (id.isNotEmpty()) opened = juce::MidiOutput::openDevice(id);
	const juce::ScopedLock lock(osMidiLock);
	osMidiOut = std::move(opened);
	selOutputId = osMidiOut ? id : juce::String();
}

void D110AudioProcessor::handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m) {
	// Handed to the collector rather than acted on here: this runs on the OS MIDI thread,
	// and everything downstream - the engine's single-writer queue and the control board's
	// serial input - is fed from processBlock.
	//
	// Rechannelized onto the on-screen keyboard's own selected channel first - see
	// processBlock()'s own comment (the equivalent rechannelling for a DAW-hosted external
	// controller) for why. This is the Standalone app's directly-opened MIDI port, the other
	// place real external MIDI enters.
	if (midiRemap && m.getChannel() > 0) {
		auto msg = m;
		msg.setChannel(keyboardMidiChannel);
		osMidiCollector.addMessageToQueue(msg);
		return;
	}
	osMidiCollector.addMessageToQueue(m);
}

void D110AudioProcessor::injectTestNote(int channel, int note, float velocity, bool on) {
	auto message = on ? juce::MidiMessage::noteOn(channel, note, velocity)
	                   : juce::MidiMessage::noteOff(channel, note, velocity);
	// addMessageToQueue times messages against Time::getMillisecondCounter()'s base, same as
	// the real MidiInput callback above stamps its own messages before handing them here.
	message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
	osMidiCollector.addMessageToQueue(message);
}

void D110AudioProcessor::injectMidiMessage(const juce::MidiMessage &message) {
	auto stamped = message;
	stamped.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
	osMidiCollector.addMessageToQueue(stamped);
}

void D110AudioProcessor::forwardMidiToFirmware(const juce::MidiMessage &message) {
	if (!core.isRunning()) return;

	// Clock and active sensing arrive constantly and say nothing the panel can show. At
	// 3125 bytes a second they would crowd out the messages that matter, so they stop
	// here. Everything else - notes, controllers, program changes, bend, exclusive - goes
	// through, because the firmware is meant to be the authority on what it is playing.
	const auto *raw = message.getRawData();
	const int size = message.getRawDataSize();
	if (size <= 0) return;
	if (raw[0] == 0xF8 || raw[0] == 0xFE) return;

	if (!forwardNotes.load(std::memory_order_relaxed)
	    && (message.isNoteOnOrOff() || message.isAftertouch() || message.isChannelPressure()))
		return;

	// See D110CoreType::hintRhythmKey(): a handful of rhythm timbres make the firmware's own
	// note-completion write carry a small placeholder instead of the real key, and this is
	// the one place that still knows what the real key was. Channel 10 is the factory
	// rhythm channel, the same assumption the rest of this project already makes (rhythm
	// probes, docs) rather than something tracked per chanAssign here.
	//
	// Gated to the three measured-broken timbres (64, 65, 66) specifically - NOT every
	// rhythm hit. Queuing a hint for every drum, working ones included, was the bug a
	// session found by ear: the queue is a plain FIFO with no link back to which key it was
	// for, so an ordinary kick or snare played earlier sat in it and later got handed to an
	// unrelated hi-hat hit whose own completion happened to need a substitute, printing the
	// wrong drum onto that key until the backlog cleared. Checking the CURRENT Rhythm Setup
	// tembr keeps the queue holding only entries a broken completion could plausibly be
	// asking for.
	if (message.isNoteOn() && message.getChannel() == 10) {
		const int note = message.getNoteNumber();
		if (note >= D110CoreType::kRhythmFirstKey && note < D110CoreType::kRhythmFirstKey + D110CoreType::kNumRhythmKeys) {
			std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
			if (core.getRam(ram.data())) {
				const size_t at = size_t(D110CoreType::kRamRhythmTemp)
				                + size_t(note - D110CoreType::kRhythmFirstKey) * D110CoreType::kRhythmRecord;
				const uint8_t tembr = ram[at];
				if (tembr == 64 || tembr == 65 || tembr == 66)
					core.hintRhythmKey(juce::uint8(note));
			}
		}
	}

	core.pushMidi(static_cast<const juce::uint8 *>(raw), size);
}

juce::AudioProcessorEditor *D110AudioProcessor::createEditor() {
	return new D110AudioProcessorEditor(*this);
}

D110AudioProcessor::LcdSnapshot D110AudioProcessor::getLcdSnapshot() const {
	LcdSnapshot snapshot;

	// Both rows start as spaces, so anything shorter than 16 characters is padded rather than
	// leaving whatever the previous, longer message put there.
	for (int line = 0; line < LcdSnapshot::kLines; ++line)
		for (int col = 0; col < LcdSnapshot::kCols; ++col)
			snapshot.text[line][col] = ' ';

	auto write = [&snapshot](int line, int col, const char *s) {
		for (int i = 0; s[i] != '\0' && col + i < LcdSnapshot::kCols; ++i)
			snapshot.text[line][col + i] = static_cast<juce::uint8>(s[i]);
	};

	if (!synth) {
		write(0, 0, "D-110  No ROMs");
		write(1, 0, "loaded");
		return snapshot;
	}

	// The lamp comes off the firmware's own SO register when the control board is running -
	// bit 0 of 0x0200, which is what drives it on the hardware. mt32emu's getDisplayState()
	// is only the fallback for a powered-off or not-yet-booted machine: it answers a
	// different question (has a MIDI message just arrived, is any voice part sounding)
	// that merely looks similar.
	if (core.midiLampValid()) {
		snapshot.midiLedOn = core.midiLampOn();
	} else {
		char unusedBuffer[21] = {};
		snapshot.midiLedOn = synth->getDisplayState(unusedBuffer, false);
	}

	// Row 1, exactly as the real unit draws it (see docs/lcd_reference.png): the eight Part slots
	// and the Rhythm slot, then the mode word. A Part that currently has a partial playing in a
	// non-releasing phase has its number REPLACED by the full-block character - the hardware's own
	// convention, documented in munt's Display.cpp, not a dimmed digit.
	const MT32Emu::Bit32u partStates = synth->getPartStates();
	for (int i = 0; i < 8; ++i)
		snapshot.text[0][i] = (partStates & (1u << i)) ? LcdSnapshot::kActivePartBlock
		                                               : static_cast<juce::uint8>('1' + i);
	snapshot.text[0][8] = (partStates & (1u << 8)) ? LcdSnapshot::kActivePartBlock
	                                               : static_cast<juce::uint8>('R');
	write(0, 10, "RomPly");

	// Row 2: "<part>:<patch name>", again matching the photographed screen.
	const int part = selectedPartIndex.load();
	snapshot.text[1][0] = static_cast<juce::uint8>('1' + part);
	snapshot.text[1][1] = ':';
	if (const char *name = synth->getPatchName(static_cast<MT32Emu::Bit8u>(part)))
		write(1, 2, juce::String(name).trim().toRawUTF8());

	return snapshot;
}

bool D110AudioProcessor::readEngineMemory(juce::uint32 packedAddress, juce::uint32 length,
                                          juce::uint8 *out) {
	if (synth == nullptr) return false;
	synth->readMemory(static_cast<MT32Emu::Bit32u>(packedAddress),
	                  static_cast<MT32Emu::Bit32u>(length),
	                  static_cast<MT32Emu::Bit8u *>(out));
	return true;
}

float D110AudioProcessor::getMasterVolume() const {
	if (auto *p = parameters.getParameter("masterVolume"))
		return p->getValue();
	return 1.0f;
}

void D110AudioProcessor::setMasterVolume(float newValue) {
	if (auto *p = parameters.getParameter("masterVolume")) {
		p->beginChangeGesture();
		p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, newValue));
		p->endChangeGesture();
	}
}

void D110AudioProcessor::resetDisplayToMainMode() {
	if (synth) synth->setMainDisplayMode();
}

// Pitch calibration for a custom PCM wave, resampled to the LA32 engine's own native rate
// (kPcmSampleRate in loadCustomPcmWave() below) - see MT32Emu::Synth::setPCMWavePitchOffset()'s
// own comment for the concept.
//
// 2026-08-27: briefly changed to 57344, derived on paper from LA32WaveGenerator.cpp's own
// comment ("pcmSampleStep = EXP2F(pitch/4096 + 3)", the fixed-point renderer this project
// actually runs by default - RendererType_BIT16S, nothing in plugin/Source overrides it) and
// seemingly backed by this file's own pcm_pitch_calibration_probe.cpp (measured 953-987 Hz for
// a 1000 Hz test tone, vs. 902-911 Hz for 20480). Alan's own listening test immediately after
// contradicted this: 57344 played back "quasi silencieux" without loop and "un son bizarre,
// très éloigné de l'original" with loop - both symptoms of a pitch that's actually much too
// HIGH (a one-shot sample racing through its whole stored length almost instantly then sitting
// silent for the rest of the note; looped, the same fast race repeating audibly as a buzz) -
// the opposite conclusion from the probe's own numbers. Reverted to 20480 on the strength of
// that direct listening test, which is a more trustworthy signal than either paper derivation
// or this probe - the probe's methodology has at least one already-observed unexplained bug
// (identical readings across unrelated Tone-parameter changes earlier the same session), so
// its measurements should not be trusted over a real A/B listen until that's actually found.
// If revisiting: verify by ear first, and treat any formula/measurement that disagrees with a
// direct listening test as the thing that's wrong, not the other way around.
constexpr MT32Emu::Bit16u kNeutralPcmPitchOffset = 20480;

void D110AudioProcessor::reapplyCustomPcmWaves() {
	if (!synth) return;
	for (const auto &entry : customPcmWaves) {
		std::vector<MT32Emu::Bit16s> backup(entry.second.size());
		synth->getPCMWaveSamples(static_cast<MT32Emu::Bit32u>(entry.first), backup.data(),
		                         static_cast<MT32Emu::Bit32u>(backup.size()));
		factoryPcmWaveBackup[entry.first] = std::move(backup);
		MT32Emu::Bit16u pitchBackup = 0;
		synth->getPCMWavePitchOffset(static_cast<MT32Emu::Bit32u>(entry.first), pitchBackup);
		factoryPcmWavePitchBackup[entry.first] = pitchBackup;
		MT32Emu::Bit32u infoAddr = 0, infoLen = 0;
		bool factoryLoop = true;
		synth->getPCMWaveInfo(static_cast<MT32Emu::Bit32u>(entry.first), infoAddr, infoLen, factoryLoop);
		factoryPcmWaveLoopBackup[entry.first] = factoryLoop;
		synth->setPCMWaveSamples(static_cast<MT32Emu::Bit32u>(entry.first), entry.second.data(),
		                         static_cast<MT32Emu::Bit32u>(entry.second.size()));
		synth->setPCMWavePitchOffset(static_cast<MT32Emu::Bit32u>(entry.first), kNeutralPcmPitchOffset);
		const auto loopIt = customPcmWaveLoops.find(entry.first);
		synth->setPCMWaveLoop(static_cast<MT32Emu::Bit32u>(entry.first),
		                      loopIt != customPcmWaveLoops.end() ? loopIt->second : factoryLoop);
	}
}

bool D110AudioProcessor::loadCustomPcmWave(int waveIndex, const juce::File &audioFile) {
	if (!synth) {
		lastImportMessage = "Switch the instrument on first.";
		return false;
	}
	MT32Emu::Bit32u addr = 0, len = 0;
	bool loop = false;
	if (!synth->getPCMWaveInfo(static_cast<MT32Emu::Bit32u>(waveIndex), addr, len, loop)) {
		lastImportMessage = "Invalid PCM wave slot.";
		return false;
	}

	juce::AudioFormatManager formatManager;
	formatManager.registerBasicFormats();
	std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(audioFile));
	if (reader == nullptr) {
		lastImportMessage = "Could not read audio file: " + audioFile.getFileName();
		return false;
	}

	// Mixed to mono (the D-110's PCM partials are single-channel) by simple averaging.
	const auto numSourceFrames = static_cast<int>(juce::jmin<juce::int64>(reader->lengthInSamples, 1 << 24));
	juce::AudioBuffer<float> source(static_cast<int>(reader->numChannels), juce::jmax(1, numSourceFrames));
	reader->read(&source, 0, numSourceFrames, 0, true, true);
	std::vector<float> mono(static_cast<size_t>(numSourceFrames), 0.0f);
	for (int ch = 0; ch < source.getNumChannels(); ++ch) {
		const float *src = source.getReadPointer(ch);
		for (int i = 0; i < numSourceFrames; ++i) mono[static_cast<size_t>(i)] += src[i];
	}
	if (source.getNumChannels() > 1) {
		const float scale = 1.0f / static_cast<float>(source.getNumChannels());
		for (auto &s : mono) s *= scale;
	}

	// Resample to the LA32 engine's own native rate - D110CoreType::kMidiBytesPerSecond's
	// neighbourhood constant is unrelated; the actual figure is MT32Emu::SAMPLE_RATE (32000),
	// matching extract_pcm.py's own SAMPLE_RATE in ../LA-16/pcm_waves/.
	constexpr double kPcmSampleRate = 32000.0;
	const double ratio = reader->sampleRate / kPcmSampleRate;
	std::vector<float> resampled;
	if (numSourceFrames > 0 && ratio > 0.0) {
		resampled.resize(static_cast<size_t>(std::ceil(numSourceFrames / ratio)) + 4, 0.0f);
		juce::LagrangeInterpolator interpolator;
		const int produced = interpolator.process(ratio, mono.data(), resampled.data(),
		                                          static_cast<int>(resampled.size()));
		resampled.resize(static_cast<size_t>(juce::jmax(0, produced)));
	}

	// Fit to the slot's own fixed length (from the real control ROM's wave table, unchanged) -
	// silence-padded if shorter, simply cut off if longer. No time-stretching: matching length
	// exactly is the user's job if that matters for their use, same as the LA-16 tribute site's
	// own "capped" PCM slot loading.
	std::vector<MT32Emu::Bit16s> encoded(len, 0);
	const size_t toCopy = juce::jmin(resampled.size(), static_cast<size_t>(len));
	for (size_t i = 0; i < toCopy; ++i) encoded[i] = encodePcmLogSample(resampled[i]);
	// encodePcmLogSample(0.0f) for the silence-padded tail: mag<=0 branch -> log=0 -> the same
	// near-silent (not exact zero) value the format uses everywhere else, not a hard cut.
	for (size_t i = toCopy; i < encoded.size(); ++i) encoded[i] = encodePcmLogSample(0.0f);

	if (customPcmWaves.count(waveIndex) == 0) {
		std::vector<MT32Emu::Bit16s> backup(len, 0);
		synth->getPCMWaveSamples(static_cast<MT32Emu::Bit32u>(waveIndex), backup.data(), len);
		factoryPcmWaveBackup[waveIndex] = std::move(backup);
		MT32Emu::Bit16u pitchBackup = 0;
		synth->getPCMWavePitchOffset(static_cast<MT32Emu::Bit32u>(waveIndex), pitchBackup);
		factoryPcmWavePitchBackup[waveIndex] = pitchBackup;
		// Starting loop state is the factory wave's own - matches the pre-existing behaviour
		// Alan observed ("reprend la valeur de loop d'origine"); setCustomPcmWaveLoop() is the
		// explicit override on top of that. factoryPcmWaveLoopBackup keeps the ORIGINAL value
		// specifically for restoreFactoryPcmWave(), separate from customPcmWaveLoops (the
		// current, possibly-overridden setting) since the two can differ once toggled.
		factoryPcmWaveLoopBackup[waveIndex] = loop;
		customPcmWaveLoops[waveIndex] = loop;
	}
	synth->setPCMWaveSamples(static_cast<MT32Emu::Bit32u>(waveIndex), encoded.data(),
	                         static_cast<MT32Emu::Bit32u>(encoded.size()));
	// Without this, a replacement plays back pitch-shifted by whatever the ORIGINAL factory
	// wave's own calibration was - see kNeutralPcmPitchOffset's own comment. This is the fix
	// for Alan's "sounds like a high-pitched whistle" report, 2026-08-27.
	synth->setPCMWavePitchOffset(static_cast<MT32Emu::Bit32u>(waveIndex), kNeutralPcmPitchOffset);
	customPcmWaves[waveIndex] = std::move(encoded);
	customPcmWaveNames[waveIndex] = audioFile.getFileNameWithoutExtension();

	lastImportMessage = "Loaded custom sample into PCM wave " + juce::String(waveIndex + 1)
		+ " from " + audioFile.getFileName();
	return true;
}

bool D110AudioProcessor::setCustomPcmWavePitchOffset(int waveIndex, int pitchOffset) {
	if (!synth) return false;
	return synth->setPCMWavePitchOffset(static_cast<MT32Emu::Bit32u>(waveIndex),
	                                    static_cast<MT32Emu::Bit16u>(pitchOffset));
}

bool D110AudioProcessor::setCustomPcmWaveLoop(int waveIndex, bool loop) {
	if (!synth || customPcmWaves.count(waveIndex) == 0) return false;
	customPcmWaveLoops[waveIndex] = loop;
	return synth->setPCMWaveLoop(static_cast<MT32Emu::Bit32u>(waveIndex), loop);
}

bool D110AudioProcessor::restoreFactoryPcmWave(int waveIndex) {
	const auto it = factoryPcmWaveBackup.find(waveIndex);
	if (it == factoryPcmWaveBackup.end() || !synth) return false;
	synth->setPCMWaveSamples(static_cast<MT32Emu::Bit32u>(waveIndex), it->second.data(),
	                         static_cast<MT32Emu::Bit32u>(it->second.size()));
	const auto pitchIt = factoryPcmWavePitchBackup.find(waveIndex);
	if (pitchIt != factoryPcmWavePitchBackup.end()) {
		synth->setPCMWavePitchOffset(static_cast<MT32Emu::Bit32u>(waveIndex), pitchIt->second);
		factoryPcmWavePitchBackup.erase(pitchIt);
	}
	const auto loopIt = factoryPcmWaveLoopBackup.find(waveIndex);
	if (loopIt != factoryPcmWaveLoopBackup.end()) {
		synth->setPCMWaveLoop(static_cast<MT32Emu::Bit32u>(waveIndex), loopIt->second);
		factoryPcmWaveLoopBackup.erase(loopIt);
	}
	factoryPcmWaveBackup.erase(it);
	customPcmWaves.erase(waveIndex);
	customPcmWaveNames.erase(waveIndex);
	customPcmWaveLoops.erase(waveIndex);
	lastImportMessage = "Restored factory PCM wave " + juce::String(waveIndex + 1) + ".";
	return true;
}

void D110AudioProcessor::importSysexBank(const juce::File &file) {
	std::vector<std::vector<MT32Emu::Bit8u>> messages;

	if (file.hasFileExtension("mid") || file.hasFileExtension("smf")) {
		messages = extractSysexMessagesFromMidiFile(file);
	}

	if (messages.empty()) {
		// Either a genuine .syx file, or a .mid JUCE couldn't parse - fall back to scanning raw bytes.
		juce::MemoryBlock raw;
		if (file.loadFileAsData(raw)) messages = extractSysexMessagesFromRawBytes(raw);
	}

	if (messages.empty()) {
		lastImportMessage = "No SysEx data found in: " + file.getFileName();
		return;
	}

	{
		const juce::ScopedLock sl(engineActionLock);
		for (auto &message : messages) pendingSysexImports.push_back(std::move(message));
	}
	size_t bytes = 0;
	for (const auto &m : messages) bytes += m.size();
	// It goes into the control board down an emulated MIDI cable at the real 31250 baud,
	// so a big bank genuinely takes a few seconds to land, exactly as on the hardware.
	const int seconds = int(double(bytes) / D110CoreType::kMidiBytesPerSecond + 0.5);
	lastImportMessage = "Sending " + juce::String(messages.size()) + " SysEx message(s) from "
		+ file.getFileName()
		+ (seconds >= 2 ? " (about " + juce::String(seconds) + "s at MIDI speed)" : juce::String());
}

namespace {
// "D1SN" + format version, so a stray file (or a future incompatible layout) is refused
// rather than fed to the instrument as garbage.
constexpr char kSnapshotMagic[4] = { 'D', '1', 'S', 'N' };
constexpr juce::uint8 kSnapshotVersion = 1;
} // namespace

void D110AudioProcessor::exportMemorySnapshot(const juce::File &file) {
	// Same rule as getStateInformation: while the machine is running the file on disk is
	// stale (MAME/the native core only flush NVRAM out on power-off), so the live image in
	// the core is authoritative whenever it's available, and the file is the fallback.
	juce::MemoryBlock rams;
	if (core.isRunning()) {
		rams.setSize(D110CoreType::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(rams.getData()))) rams.reset();
	}
	if (rams.getSize() == 0) rams = readNvramFile("rams");

	if (rams.getSize() != size_t(D110CoreType::kRamSize)) {
		lastImportMessage = "Nothing to snapshot yet - switch the instrument on at least once first.";
		return;
	}

	juce::MemoryBlock memcs;
	if (core.isRunning()) {
		memcs.setSize(D110CoreType::kCardSize);
		if (!core.getCardImage(static_cast<juce::uint8 *>(memcs.getData()))) memcs.reset();
	}
	if (memcs.getSize() == 0) memcs = readNvramFile("memcs");
	memcs.setSize(size_t(D110CoreType::kCardSize), true); // zero-pad if the card was never imaged

	juce::MemoryBlock out;
	out.append(kSnapshotMagic, sizeof(kSnapshotMagic));
	const juce::uint8 header[3] = { kSnapshotVersion,
	                                juce::uint8(core.cardInserted() ? 1 : 0),
	                                juce::uint8(core.cardWriteProtect() ? 1 : 0) };
	out.append(header, sizeof(header));
	out.append(rams.getData(), rams.getSize());
	out.append(memcs.getData(), memcs.getSize());

	if (!file.replaceWithData(out.getData(), out.getSize())) {
		lastImportMessage = "Could not write snapshot: " + file.getFullPathName();
		return;
	}

	lastImportMessage = "Saved memory snapshot: " + file.getFileName();
}

void D110AudioProcessor::importMemorySnapshot(const juce::File &file) {
	constexpr int kHeaderSize = 4 + 1 + 1 + 1;
	juce::MemoryBlock data;
	if (!file.loadFileAsData(data)
	    || data.getSize() != size_t(kHeaderSize) + size_t(D110CoreType::kRamSize) + size_t(D110CoreType::kCardSize)
	    || memcmp(data.getData(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0
	    || static_cast<const juce::uint8 *>(data.getData())[4] != kSnapshotVersion) {
		lastImportMessage = "Not a D-110 memory snapshot: " + file.getFileName();
		return;
	}

	const auto *bytes = static_cast<const juce::uint8 *>(data.getData());
	const bool cardInserted = bytes[5] != 0;
	const bool cardWriteProtect = bytes[6] != 0;
	juce::MemoryBlock rams(bytes + kHeaderSize, size_t(D110CoreType::kRamSize));
	juce::MemoryBlock memcs(bytes + kHeaderSize + D110CoreType::kRamSize, size_t(D110CoreType::kCardSize));

	// The core only reads NVRAM off disk at power-on, so a snapshot loaded while the
	// instrument is already running needs a power cycle to actually take - done here, rather
	// than left for the user, so "load snapshot" is felt immediately.
	const bool wasOn = core.isRunning();
	if (wasOn) setPoweredOn(false);
	writeNvramFiles(rams, memcs);
	core.setCardInserted(cardInserted);
	core.setCardWriteProtect(cardWriteProtect);
	if (wasOn) setPoweredOn(true);

	lastImportMessage = "Loaded memory snapshot: " + file.getFileName();
}

namespace {
// One region of the documented map, built as one or more DT1 messages (buildDt1Message caps
// a single message's data at kMaxSysexBytes - 12 bytes, which is smaller than a 246-byte Tone
// Temporary record or a 256-byte Tone record, so a region longer than that is split across
// several messages - the same thing kMirrorRegions/D110_RHYTHM already do for Rhythm Setup,
// and just as harmless here: each DT1 carries its own address, so the firmware does not care
// how many messages a region arrived as).
// `sysexBaseAddress` is the region's fixed base (e.g. kSysexToneTemp) and never changes per
// record - exactly as sendToneTempParam/sendPatchMemoryParam/etc. already call buildDt1Message
// elsewhere in this file. The record's position is folded into `sysexOffsetBase` instead, which
// buildDt1Message adds in the SEVEN-BIT packed address space (see its own comment): the base
// address's hex digits are NOT plain-integer-addable, since a byte offset crossing 0x80 has to
// carry into the next seven-bit digit rather than the next hex nibble.
void appendDt1Region(juce::MemoryBlock &out, juce::uint32 sysexBaseAddress, int sysexOffsetBase,
                     const juce::uint8 *ram, int ramOffset, int length) {
	constexpr int kChunk = 200;
	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	for (int sent = 0; sent < length; sent += kChunk) {
		const int n = std::min(kChunk, length - sent);
		const int written = D110CoreType::buildDt1Message(sysexBaseAddress, sysexOffsetBase + sent,
		                                                   ram + ramOffset + sent, n, msg);
		if (written > 0) out.append(msg, size_t(written));
	}
}
} // namespace

void D110AudioProcessor::exportSysexBank(const juce::File &file) {
	juce::MemoryBlock ramsBlock;
	if (core.isRunning()) {
		ramsBlock.setSize(D110CoreType::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(ramsBlock.getData()))) ramsBlock.reset();
	}
	if (ramsBlock.getSize() == 0) ramsBlock = readNvramFile("rams");
	if (ramsBlock.getSize() != size_t(D110CoreType::kRamSize)) {
		lastImportMessage = "Nothing to export yet - switch the instrument on at least once first.";
		return;
	}
	const auto *ram = static_cast<const juce::uint8 *>(ramsBlock.getData());

	juce::MemoryBlock out;

	for (int i = 0; i < D110CoreType::kNumPatches; ++i)
		appendDt1Region(out, D110CoreType::kSysexPatches, i * D110CoreType::kPatchRecord,
		                ram, D110CoreType::kRamPatches + i * D110CoreType::kPatchRecord,
		                D110CoreType::kPatchRecord);

	appendDt1Region(out, D110CoreType::kSysexTimbreTemp, 0, ram, D110CoreType::kRamTimbreTemp,
	                D110CoreType::kNumParts * D110CoreType::kTimbreTempRecord);

	appendDt1Region(out, D110CoreType::kSysexRhythmTemp, 0, ram, D110CoreType::kRamRhythmTemp,
	                D110CoreType::kNumRhythmKeys * D110CoreType::kRhythmRecord);

	// Rhythm has no Tone Temporary record of its own, so only the 8 voice parts.
	for (int i = 0; i < 8; ++i)
		appendDt1Region(out, D110CoreType::kSysexToneTemp, i * D110CoreType::kToneRecord,
		                ram, D110CoreType::kRamToneTemp + i * D110CoreType::kToneRecord,
		                D110CoreType::kToneRecord);

	for (int i = 0; i < D110CoreType::kNumTimbres; ++i)
		appendDt1Region(out, D110CoreType::kSysexTimbres, i * D110CoreType::kTimbreRecord,
		                ram, D110CoreType::kRamTimbres + i * D110CoreType::kTimbreRecord,
		                D110CoreType::kTimbreRecord);

	// Only the 23 documented System Area bytes - the rest of the RAM up to Tone Memory is
	// undocumented firmware working state, not patch/tone data (see kMirrorRegions' own
	// comment on the same boundary).
	appendDt1Region(out, D110CoreType::kSysexSystem, 0, ram, D110CoreType::kRamSystem, 23);

	for (int i = 0; i < D110CoreType::kNumTones; ++i)
		appendDt1Region(out, D110CoreType::kSysexTones, i * D110CoreType::kToneMemRecord,
		                ram, D110CoreType::kRamTones + i * D110CoreType::kToneMemRecord,
		                D110CoreType::kToneMemRecord);

	if (!file.replaceWithData(out.getData(), out.getSize())) {
		lastImportMessage = "Could not write SysEx bank: " + file.getFullPathName();
		return;
	}

	lastImportMessage = "Saved SysEx bank: " + file.getFileName();
}

void D110AudioProcessor::selectNextPart() {
	selectedPartIndex = (selectedPartIndex.load() + 1) % 8;
}

void D110AudioProcessor::selectPreviousPart() {
	selectedPartIndex = (selectedPartIndex.load() + 7) % 8;
}

void D110AudioProcessor::stepPatch(int direction) {
	const int part = selectedPartIndex.load();
	int newProgram = (currentProgramPerPart[static_cast<size_t>(part)] + direction) % 128;
	if (newProgram < 0) newProgram += 128;
	currentProgramPerPart[static_cast<size_t>(part)] = newProgram;

	// Default factory chanAssign maps Part i to MIDI channel nibble i+1 (Part 1 -> channel 2, etc).
	const int channelNibble = part + 1;
	const MT32Emu::Bit32u message = static_cast<MT32Emu::Bit32u>(0xC0 | channelNibble)
		| (static_cast<MT32Emu::Bit32u>(newProgram) << 8);

	const juce::ScopedLock sl(engineActionLock);
	pendingShortMessages.push_back(message);
}

void D110AudioProcessor::midiPanic() {
	std::vector<juce::uint8> firmwareBytes;
	std::vector<MT32Emu::Bit32u> engineMessages;
	firmwareBytes.reserve(16 * 2 * 3);
	engineMessages.reserve(16 * 2);
	for (int channel = 0; channel < 16; ++channel) {
		const juce::uint8 status = juce::uint8(0xB0 | channel);
		for (juce::uint8 controller : { juce::uint8(64), juce::uint8(123) }) { // hold pedal off, all notes off
			firmwareBytes.push_back(status);
			firmwareBytes.push_back(controller);
			firmwareBytes.push_back(0);
			engineMessages.push_back(MT32Emu::Bit32u(status) | (MT32Emu::Bit32u(controller) << 8));
		}
	}

	{
		const juce::ScopedLock sl(engineActionLock);
		pendingPanicBytes.insert(pendingPanicBytes.end(), firmwareBytes.begin(), firmwareBytes.end());
		pendingShortMessages.insert(pendingShortMessages.end(), engineMessages.begin(), engineMessages.end());
		// Alan's report, 2026-08-30: CC64/CC123 above only ever reach the firmware's NORMAL
		// release path, which depends on envelope-stage pacing this emulation doesn't implement
		// (see D110CoreNative::releaseStuckNoteContexts()/resetVoiceSlotTable()'s own comments) -
		// a note stuck on that path never gets released by them alone, and neither does the
		// Monitor tab's LA32 voice-slot grid. Both applied on the audio thread, next
		// processBlock() onward - see there.
		pendingForceReleaseStuckVoices = true;
	}
	// 1.5s of samples at whatever rate is currently set - comfortably longer than the ~30ms the
	// 96 CC64/CC123 bytes above take to reach the firmware's own MIDI IN at its 3125 bytes/s
	// pace, plus room for its own (still-buggy) response to that to run its course - see
	// resetVoiceSlotTable()'s own comment for why a single call can't just win that race
	// outright. getSampleRate() is safe to read from the message thread here: it only ever
	// changes via prepareToPlay(), and a stale/mid-update read costs at most a slightly
	// off-length repeat window, never a crash.
	pendingSlotTableResetSamplesRemaining.store(int(getSampleRate() * 1.5), std::memory_order_relaxed);

	// A stuck note is far more annoying on real external gear than in the internal engine -
	// there's no "stop the plugin" to fall back on - so panic reaches the direct MIDI Out port
	// too, not just the firmware/sound engine above.
	const juce::ScopedLock midiOutLock(osMidiLock);
	if (osMidiOut != nullptr) {
		juce::MidiBuffer panicOut;
		for (int channel = 0; channel < 16; ++channel) {
			panicOut.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 64, 0), 0);
			panicOut.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 123, 0), 0);
		}
		osMidiOut->sendBlockOfMessagesNow(panicOut);
	}

	// CC 123 above is an "all notes off" CONTROLLER message, not a literal note-off per note -
	// isNoteActive()'s array only ever gets touched by real note-on/off (see
	// handleIncomingMidiMessage(const juce::MidiMessage&)), so without this, the keyboard's
	// remote-activity LEDs would stay lit forever after STOP/panic instead of clearing along
	// with everything else. Safe to write here, on the message thread: plain atomics, same as
	// every other writer of this array.
	for (auto &a : remoteNoteActive) a.store(false);
}

// ---- the extended editor -----------------------------------------------------
//
// Every one of these builds a Roland "Data set 1" and hands it to the CONTROL BOARD's own
// MIDI input, which is the same door an external editor knocks on. Nothing goes to the
// sound engine from here: the firmware writes its memory, and the mirror
// (D110CoreType::emitRegionSysex) carries that across on its next snapshot - so a drawer edit
// and a panel edit are the same event, and the instrument's own display shows both.
//
// That the firmware accepts them, and where each one lands, is MEASURED - see
// plugin/editor_write_probe.cpp, which sends one write per area and reports which byte of
// the battery RAM moved. Mem Protect, which is ON from the factory, does not block them.

void D110AudioProcessor::sendAreaData(juce::uint32 sysexAddress, int offset,
                                      const juce::uint8 *data, int length) {
	if (!core.isRunning() || data == nullptr || length <= 0) return;
	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	const int n = D110CoreType::buildDt1Message(sysexAddress, offset, data, length, msg);
	if (n > 2) {
		msg[2] = firmwareDeviceId(); // not covered by the checksum
		core.pushMidi(msg, n);
	}
}

// Measured (plugin/channel_import_probe.cpp): the firmware silently ignores any DT1 whose device
// ID differs from its Exclu Unit#. That byte is battery RAM, so it survives restarts - and a
// Display Message SysEx left pending at shutdown makes the next boot reset it to 0, after which
// every editor write (hard-coded 0x10 before this) was dropped for good.
juce::uint8 D110AudioProcessor::firmwareDeviceId() const {
	if (!core.isRunning()) return 0x10;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return 0x10;
	const uint8_t id = ram[size_t(D110CoreType::kRamUnitNumber)];
	return id <= 31 ? id : juce::uint8(0x10);
}

void D110AudioProcessor::sendTimbreTempParam(int part, int field, juce::uint8 value) {
	if (part < 0 || part >= D110CoreType::kNumParts) return;
	if (field < 0 || field >= D110CoreType::kTimbreTempRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexTimbreTemp, part * D110CoreType::kTimbreTempRecord + field, &v, 1);
}

void D110AudioProcessor::sendToneTempParam(int part, int offset, juce::uint8 value) {
	if (part < 0 || part > 7 || offset < 0 || offset >= D110CoreType::kToneRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexToneTemp, part * D110CoreType::kToneRecord + offset, &v, 1);
}

void D110AudioProcessor::sendRhythmParam(int slot, int field, juce::uint8 value) {
	if (slot < 0 || slot >= D110CoreType::kNumRhythmKeys) return;
	if (field < 0 || field >= D110CoreType::kRhythmRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexRhythmTemp, slot * D110CoreType::kRhythmRecord + field, &v, 1);
}

void D110AudioProcessor::sendSystemParam(int field, juce::uint8 value) {
	// 23 bytes, as Roland describes them: tune, reverb, partial reserve, channel map and volume.
	// On the D-110 the last one is a physical knob and the firmware never fills it, so the
	// editor does not offer it; the limit here is still set by the area's length, not by what
	// the editor draws.
	if (field < 0 || field > 22) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexSystem, field, &v, 1);
}

void D110AudioProcessor::sendTimbreMemoryParam(int slot, int field, juce::uint8 value) {
	if (slot < 0 || slot >= D110CoreType::kNumTimbres) return;
	if (field < 0 || field >= D110CoreType::kTimbreRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexTimbres, slot * D110CoreType::kTimbreRecord + field, &v, 1);
}

void D110AudioProcessor::sendPatchMemoryParam(int patch, int field, juce::uint8 value) {
	if (patch < 0 || patch >= D110CoreType::kNumPatches) return;
	if (field < 0 || field >= D110CoreType::kPatchRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexPatches, patch * D110CoreType::kPatchRecord + field, &v, 1);
}

// A real D-110's Program Change can only ever reach the 128 factory-fixed Timbre Memory slots
// (each one a fixed, unchangeable Tone group/number pair) - an Internal tone, built by hand in
// the TONE tab, was never addressable that way even on real hardware. The only way to make a
// receiving unit play one is the same one an external librarian would use: pour the 256-byte
// Tone Memory record straight into its own memory via DT1, then point the part's own Timbre
// Temp at (group 2, that slot) directly - which is what Program Change does under the hood for
// group 0/1, just done by hand here since there's no shortcut number for group 2. See
// sequencerLiveInternalTone/sequencerLiveToneMemory's own comment for where the two inputs
// come from (a block-refresh snapshot, same one sequencerLivePrograms already relies on).
//
// Also always writes a channel-assign message for a melodic track (Alan's report, 2026-08-29):
// the note events themselves are written on whatever channel channelForTrack() resolves for
// this track (the exporting instrument's OWN current SYSTEM-page channel map, or its factory-
// default fallback) - a receiving unit with a DIFFERENT channel map on its own SYSTEM page
// would have those notes land on the wrong Part, exactly what he hit loading an export with a
// customised map onto real hardware still on factory defaults. Writing this unconditionally,
// not just alongside an Internal-tone dump, is what actually fixes it - the common case (no
// custom tone at all) still needs its channel asserted just as much.
std::vector<std::vector<juce::uint8>> D110AudioProcessor::buildTrackSysExPreamble(int track) const {
	std::vector<std::vector<juce::uint8>> out;
	if (track < 0 || track >= static_cast<int>(sequencerLiveInternalTone.size())) return out;

	juce::uint8 msg[D110CoreType::kMaxSysexBytes];

	if (track != d110seq::D110SequencerEngine::kRhythmTrack) {
		// SYSTEM area, offset 13+track, one byte - same field sequencerLiveChannels is refreshed
		// from (kRamSystem+13+t): 0-15 = channel 1-16. Rhythm has no such byte - its channel is
		// fixed at 10 by convention, never part of this map (see sequencerLiveChannels' own
		// comment) - see channelForTrack()'s own comment for why the fallback below never
		// throws away part of the encoding
		const int channel = juce::jlimit(1, 16, sequencerEngine.channelForTrack(track));
		const juce::uint8 channelByte = static_cast<juce::uint8>(channel - 1);
		const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexSystem, 13 + track, &channelByte, 1, msg);
		if (n > 2) out.emplace_back(msg + 1, msg + n - 1); // drop the F0/F7 createSysExMessage() re-adds
	}

	const int tone = sequencerLiveInternalTone[static_cast<size_t>(track)];
	if (tone < 0 || tone > 63) return out;

	const auto &bytes = sequencerLiveToneMemory[static_cast<size_t>(track)];
	// Same 244-byte-per-message ceiling sendToneBlock() already works around for the (bigger)
	// Tone Temporary Area, same chunk size too, for one less magic number in the codebase.
	constexpr int kChunk = 123;
	for (int off = 0; off < D110CoreType::kToneMemRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneMemRecord - off);
		const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexTones,
		                                            tone * D110CoreType::kToneMemRecord + off,
		                                            bytes.data() + off, len, msg);
		if (n > 2) out.emplace_back(msg + 1, msg + n - 1);
	}

	const juce::uint8 groupTone[2] = { 2, static_cast<juce::uint8>(tone) };
	const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexTimbreTemp,
	                                            track * D110CoreType::kTimbreTempRecord, groupTone, 2, msg);
	if (n > 2) out.emplace_back(msg + 1, msg + n - 1);
	return out;
}

// Same write as sendTimbreTempParam(), but queued behind whatever the import has already put in
// osMidiCollector (SysEx, Program Changes) instead of jumping ahead of it: a Program Change
// reloads the Part's LEVEL/PAN from Timbre Memory, so a Volume/Pan written first is wiped out.
void D110AudioProcessor::queueTimbreTempParam(int part, int field, juce::uint8 value) {
	if (part < 0 || part >= D110CoreType::kNumParts) return;
	if (field < 0 || field >= D110CoreType::kTimbreTempRecord) return;
	const juce::uint8 v = value & 0x7f;
	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexTimbreTemp,
	                                            part * D110CoreType::kTimbreTempRecord + field, &v, 1, msg);
	if (n <= 2) return;
	msg[2] = firmwareDeviceId();
	auto sysex = juce::MidiMessage::createSysExMessage(msg + 1, n - 2);
	sysex.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
	osMidiCollector.addMessageToQueue(sysex);
}

void D110AudioProcessor::applyLoadedTrackSetup(int track, std::vector<juce::MidiMessage> setup) {
	// Program Change and the Internal-tone SysEx preamble are replayed as plain live MIDI,
	// through osMidiCollector - confirmed reliable (Program Change and the Tone Memory dump
	// both land correctly against real files, verified by direct RAM read).
	//
	// Volume (CC7) and Pan (CC10) are NOT replayed as live MIDI - a real regression shipped
	// under that theory earlier the same day (2026-08-21) and was reverted once Alan confirmed
	// against his own real DAW session that live CC7 has NO audible effect on this instrument
	// AT ALL, full stop - not "attenuates via the sound engine on top of the Timbre's own
	// LEVEL" as first assumed, just genuinely inert, on real hardware and in this emulation
	// both. The D-110 simply never implements MIDI Channel Volume/Pan as their own concept -
	// its only real "how loud/where panned is this Part" values are the Timbre's own LEVEL/PAN
	// fields (TimbreTemp offsets 8/9, exactly what the PARTS tab edits), so restoring them on
	// reimport has to write there directly - sendTimbreTempParam(), the same call the PARTS
	// tab's own LEVEL/PAN columns use, address-based rather than channel-based (which also
	// sidesteps a still-unexplained, separate bug where live CC10 replay works for some
	// channels and not others - see project_reimport_volume_pan_channel_bug memory). Scaled
	// back from the wire's 0-127 down to the D-110's own 0-100/0-14 ranges.
	// The engine rewrote each Program Change's channel to the track's channel AS OF THE LOAD,
	// i.e. before this file's own SysEx has remapped the channel map (a file authored with
	// Part N on channel N lands on a factory unit whose Part N still listens on N+1). The
	// remap is queued ahead of the Program Change below, so the PC has to target the channel
	// the file's own map write (SYSTEM offset 13+track) is about to establish.
	int mapChannel = -1;
	if (track != d110seq::D110SequencerEngine::kRhythmTrack) {
		for (const auto &message : setup) {
			if (!message.isSysEx()) continue;
			const auto *d = message.getSysExData();
			if (message.getSysExDataSize() >= 8 && d[0] == 0x41 && d[2] == 0x16 && d[3] == 0x12
			    && d[4] == 0x10 && d[5] == 0x00 && d[6] == 13 + track && d[7] <= 15)
				mapChannel = d[7] + 1;
		}
	}
	for (auto message : setup) {
		if (mapChannel > 0 && message.getChannel() > 0) message.setChannel(mapChannel);
		if (message.isController() && message.getControllerNumber() == 7) {
			const int level = juce::jlimit(0, 100, juce::roundToInt(message.getControllerValue() * 100.0f / 127.0f));
			queueTimbreTempParam(track, 8, static_cast<juce::uint8>(level));
			continue;
		}
		if (message.isController() && message.getControllerNumber() == 10) {
			const int pan = juce::jlimit(0, 14, juce::roundToInt(message.getControllerValue() * 14.0f / 127.0f));
			queueTimbreTempParam(track, 9, static_cast<juce::uint8>(pan));
			continue;
		}
		if (message.isSysEx()) {
			// Only a genuine Roland D-110 DT1 write (our own SysEx preamble) gets replayed -
			// some DAWs (confirmed: Alan's own export) insert their own GM/GS
			// initialisation SysEx (Universal Non-Realtime "GM System On", Roland GS Reset -
			// different manufacturer/model header) at the start of a track, unrelated to this
			// instrument and never meant to reach it. Forwarding those to the firmware anyway
			// was a real bug found the same day (2026-08-21) - checked against the exact
			// header buildDt1Message() itself writes (Roland/device ID/D-110 model/DT1).
			const auto *data = message.getSysExData();
			const int size = message.getSysExDataSize();
			const bool isD110Dt1 = size >= 4 && data[0] == 0x41 && data[1] == 0x10 && data[2] == 0x16
			                        && data[3] == 0x12;
			if (!isD110Dt1) continue;
			// Display Message (0x200000) is cosmetic, and harmful here: a message still pending
			// at shutdown makes the firmware's next boot reset its Exclu Unit# (see
			// firmwareDeviceId()), which used to leave every later write ignored.
			if (size >= 7 && data[4] == 0x20 && data[5] == 0x00 && data[6] == 0x00) continue;
			// Files are authored for the factory unit number 17; the firmware only answers its own.
			std::vector<juce::uint8> patched(data, data + size);
			patched[1] = firmwareDeviceId();
			auto timed = juce::MidiMessage::createSysExMessage(patched.data(), int(patched.size()));
			timed.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
			osMidiCollector.addMessageToQueue(timed);
			continue;
		}
		auto timed = message;
		timed.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
		osMidiCollector.addMessageToQueue(timed);
	}
}

void D110AudioProcessor::sendName(juce::uint32 sysexAddress, int offset,
                                  const juce::String &name) {
	juce::uint8 data[D110CoreType::kNameChars];
	for (int i = 0; i < D110CoreType::kNameChars; ++i) {
		const juce::juce_wchar ch = (i < name.length()) ? name[i] : ' ';
		data[i] = juce::uint8((ch >= 32 && ch < 127) ? ch : ' ');
	}
	sendAreaData(sysexAddress, offset, data, D110CoreType::kNameChars);
}

void D110AudioProcessor::sendDisplayMessage(const juce::String &text) {
	// Twenty characters - what Roland's command carries. The D-110's display is sixteen wide,
	// so it just won't show the last four; that doesn't make the message wrong, and truncating
	// it here would mean sending something other than what the real unit sends.
	juce::uint8 data[20];
	for (int i = 0; i < 20; ++i) {
		const juce::juce_wchar ch = (i < text.length()) ? text[i] : ' ';
		data[i] = juce::uint8((ch >= 32 && ch < 127) ? ch : ' ');
	}
	sendAreaData(D110CoreType::kSysexDisplay, 0, data, 20);
}

juce::String D110AudioProcessor::displayMessageSysexHex(const juce::String &text) {
	juce::uint8 data[20];
	for (int i = 0; i < 20; ++i) {
		const juce::juce_wchar ch = (i < text.length()) ? text[i] : ' ';
		data[i] = juce::uint8((ch >= 32 && ch < 127) ? ch : ' ');
	}
	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexDisplay, 0, data, 20, msg);
	juce::StringArray bytes;
	for (int i = 0; i < n; ++i) bytes.add(juce::String::toHexString(int(msg[i])).paddedLeft('0', 2).toUpperCase());
	return bytes.joinIntoString(" ");
}

int D110AudioProcessor::liveChannelForPart(int part) const {
	if (part < 0 || part > 7 || !core.isRunning()) return -1;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return -1;
	const int chan = ram[(size_t)D110CoreType::kRamSystem + 13 + (size_t)part];
	return chan > 15 ? -1 : chan + 1; // 0-15 (off) -> 1-16, or -1 if the part is off
}

void D110AudioProcessor::selectTimbreForPart(int part, int timbre) {
	if (part < 0 || part > 7 || timbre < 0 || timbre > 127) return;
	if (!core.isRunning()) return;

	// The channel is taken from the firmware's own map (System Area, chanAssign), not computed
	// by the factory formula "part N on channel N+1": that map can be changed, and the formula
	// would then send the Program Change to the wrong place.
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	const int chan = ram[(size_t)D110CoreType::kRamSystem + 13 + (size_t)part];
	if (chan > 15) return;   // part is OFF - nowhere to send it

	const juce::uint8 bytes[2] = { juce::uint8(0xC0 | chan), juce::uint8(timbre & 0x7f) };
	core.pushMidi(bytes, 2);
	// A Program Change is an ordinary MIDI message, so it must also reach the sound engine, as
	// it would from an external keyboard.
	const juce::ScopedLock sl(engineActionLock);
	pendingShortMessages.push_back(juce::uint32(bytes[0]) | (juce::uint32(bytes[1]) << 8));
}

// Stored 0-indexed (the dialog shows 1-128) - see the processBlock() send site's own comment
// on how this combines with getTrackBank() into the actual raw Program Change byte for the
// D-110 specifically (BANK 2 adds 64, since that's how its two Timbre Memory pages actually
// work - there's no MIDI Bank Select on this instrument at all).
int D110AudioProcessor::getTrackProgram(int track) const {
	if (!supportsProgramChangeForTrack(track)) return -1;
	return sequencerEngine.getTrackProgram(track);
}

void D110AudioProcessor::setTrackProgram(int track, int program) {
	if (!supportsProgramChangeForTrack(track)) return;
	sequencerEngine.setTrackProgram(track, program);
}

int D110AudioProcessor::getTrackBank(int track) const {
	if (!supportsProgramChangeForTrack(track)) return 1;
	return sequencerEngine.getTrackBank(track);
}

void D110AudioProcessor::setTrackBank(int track, int bank) {
	if (!supportsProgramChangeForTrack(track)) return;
	sequencerEngine.setTrackBank(track, bank);
}

int D110AudioProcessor::getTrackVolume(int track) const {
	if (!supportsTrackVolumePanForTrack(track)) return -1;
	return sequencerEngine.getTrackVolume(track);
}

void D110AudioProcessor::setTrackVolume(int track, int volume) {
	if (!supportsTrackVolumePanForTrack(track)) return;
	sequencerEngine.setTrackVolume(track, volume);
}

int D110AudioProcessor::getTrackPan(int track) const {
	if (!supportsTrackVolumePanForTrack(track)) return -1;
	return sequencerEngine.getTrackPan(track);
}

void D110AudioProcessor::setTrackPan(int track, int pan) {
	if (!supportsTrackVolumePanForTrack(track)) return;
	sequencerEngine.setTrackPan(track, pan);
}

// Pull direction - see D110SequencerHost.h's own comment on resyncProgramChanges() being the
// other way around. sequencerLivePrograms is already the raw 0-127 value a real Program Change
// on this part's channel would have to be to reproduce what's playing (see that array's own
// comment): group A (0) folds to 0-63, group B (1) folds to 64-127. Rather than re-deriving a
// (bank, program) pair that could reconstruct that raw value - several would, since Bank
// 1/Program 65-128 and Bank 2/Program 1-64 address the same slots, see promptForTrackProgram's
// own comment - this always writes back Bank 1, since Bank 1/Program 1-128 alone already
// addresses the full range directly (the plain TIMBRES tab numbering), the simplest of the
// several equally-correct (bank, program) pairs that would work.
void D110AudioProcessor::captureLivePatchIntoTracks() {
	// Rhythm (index kRhythmTrack) included for Volume/Pan only, same reasoning as the
	// PLAY-edge send in processBlock() - see that loop's own comment.
	for (int t = 0; t <= d110seq::D110SequencerEngine::kRhythmTrack; ++t) {
		if (t < d110seq::D110SequencerEngine::kRhythmTrack) {
			const int program = sequencerLivePrograms[static_cast<size_t>(t)];
			if (program >= 0) {
				setTrackProgram(t, program);
				setTrackBank(t, 1);
			}
		}
		const int volume = sequencerLiveVolumes[static_cast<size_t>(t)];
		if (volume >= 0) setTrackVolume(t, volume);
		const int pan = sequencerLivePans[static_cast<size_t>(t)];
		if (pan >= 0) setTrackPan(t, pan);
	}
}

// sequencerLivePrograms already tracks each Part's live sound as a raw 0-127 Program Change
// value once per block, for MIDI-file export - see that array's own comment. Reused here as
// the Program Change dialog's pre-fill: whatever's playing right now, so Alan doesn't have to
// know or guess the number. -1 (internal tone memory or rhythm) means there's no such number
// to hint - see setTrackProgram()'s own comment on why.
int D110AudioProcessor::getTrackProgramHint(int track) const {
	if (!supportsProgramChangeForTrack(track)) return -1;
	return sequencerLivePrograms[static_cast<size_t>(track)];
}

int D110AudioProcessor::getTrackVolumeHint(int track) const {
	if (!supportsTrackVolumePanForTrack(track)) return -1;
	return sequencerLiveVolumes[static_cast<size_t>(track)];
}

int D110AudioProcessor::getTrackPanHint(int track) const {
	if (!supportsTrackVolumePanForTrack(track)) return -1;
	return sequencerLivePans[static_cast<size_t>(track)];
}

bool D110AudioProcessor::hasSoundSnapshot(int slot) const {
	if (slot < 0 || slot >= d110seq::D110SequencerEngine::kNumSongSlots) return false;
	return songSoundSnapshots[static_cast<size_t>(slot)].getSize() > 0;
}

// Captures the instrument's whole memory the same way exportMemorySnapshot() does (live RAM
// if the instrument is on, otherwise whatever the NVRAM file already holds), keyed to one of
// the sequencer's own 4 song slots instead of a file on disk.
void D110AudioProcessor::storeSoundSnapshotForSlot(int slot) {
	if (slot < 0 || slot >= d110seq::D110SequencerEngine::kNumSongSlots) return;

	juce::MemoryBlock rams;
	if (core.isRunning()) {
		rams.setSize(D110CoreType::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(rams.getData()))) rams.reset();
	}
	if (rams.getSize() == 0) rams = readNvramFile("rams");

	if (rams.getSize() != size_t(D110CoreType::kRamSize)) {
		lastImportMessage = "Nothing to store yet - switch the instrument on at least once first.";
		return;
	}
	songSoundSnapshots[static_cast<size_t>(slot)] = std::move(rams);
	lastImportMessage = "Stored the current sounds in Slot " + juce::String(slot + 1) + ".";
}

// Same power-cycle-and-replace-NVRAM approach as importMemorySnapshot() - the core only reads
// NVRAM off disk at power-on, so the new memory needs a power cycle to actually take, done
// here rather than left for the user. Near-instant (bounded by the emulated boot sequence,
// not a MIDI-speed transfer), but still a real, audible/visible reboot - which is exactly why
// this is only ever called from an explicit menu action (D110SequencerPanel's song-slot
// right-click menu), never automatically just from switching which song slot is selected.
// The memory card's own CONTENT is deliberately left untouched (writeNvramFiles only
// writes memcs when given a non-empty block) - which sounds a song used is not something
// the card should change to reflect. Its physical inserted/write-protect state, though, is
// runtime bus state that core.start() (inside setPoweredOn(true)) always resets on a fresh
// boot rather than reading back from anywhere - importMemorySnapshot() re-asserts it from
// its own saved header for the same reason. There's no such header here, so this instead
// just carries whatever was already true across the power cycle, the same way the card's
// content itself is left alone.
void D110AudioProcessor::loadSoundSnapshotForSlot(int slot) {
	if (slot < 0 || slot >= d110seq::D110SequencerEngine::kNumSongSlots) return;
	const auto &rams = songSoundSnapshots[static_cast<size_t>(slot)];
	if (rams.getSize() != size_t(D110CoreType::kRamSize)) {
		lastImportMessage = "Slot " + juce::String(slot + 1) + " has no stored sounds yet.";
		return;
	}

	const bool wasOn = core.isRunning();
	const bool wasCardInserted = wasOn && core.cardInserted();
	const bool wasCardWriteProtect = wasOn && core.cardWriteProtect();
	if (wasOn) setPoweredOn(false);
	writeNvramFiles(rams, juce::MemoryBlock());
	if (wasOn) setPoweredOn(true);
	if (wasOn) {
		core.setCardInserted(wasCardInserted);
		core.setCardWriteProtect(wasCardWriteProtect);
	}

	lastImportMessage = "Loaded Slot " + juce::String(slot + 1) + "'s stored sounds.";
}

// A patch is selected by BUTTON PRESSES, not by writing memory: on the D-110 a patch is not
// one parameter but a whole layout, and the firmware itself unpacks it into the temporary
// areas. Writing it on the firmware's behalf would mean keeping a second, private
// implementation of patch switching that one day diverges from the real one; pressing the
// buttons asks the program responsible for it on the unit to do it, and gets its display,
// its mirror and its behaviour into the bargain.
//
// The current patch number lives in RAM at 0x2DB9 (0..63); Bank+ moves it by 8, Number+ by 1
// - measured by plugin/editor_write_probe.cpp. Hence the path to any patch: at most seven
// Bank+ and seven Number+ presses.
void D110AudioProcessor::selectPatch(int patch) {
	if (patch < 0 || patch >= D110CoreType::kNumPatches) return;
	if (!core.isRunning() || patchSteps > 0) return;

	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	const int current = ram[(size_t)D110CoreType::kRamPatchNumber];
	if (current < 0 || current >= D110CoreType::kNumPatches) return;

	patchQueue.clear();
	// First to the patch-select page - from there Bank and Number mean the patch number and
	// nothing else. Always pressed, even if the patch is already the right one: the user clicked
	// a patch and is entitled to see it on the display.
	patchQueue.push_back(D110CoreType::buttonIndex(0, 6));   // Patch

	// The travel is SIGNED, and downward uses the down-arrow buttons. Wrap-around arithmetic
	// does not work here, and that is measured: the unit's Bank+ does NOT roll over from the
	// eighth bank to the first, it stops. Computing "five forward instead of three back" asked
	// for patch I-14 and left the unit at I-84 - exactly where the bank ran out
	// (plugin/editor_test.cpp).
	const int bankStep = (patch / 8) - (current / 8);
	const int numberStep = (patch % 8) - (current % 8);
	const int bankButton = D110CoreType::buttonIndex(bankStep >= 0 ? 0 : 1, 2);     // Bank+ / Bank-
	const int numberButton = D110CoreType::buttonIndex(numberStep >= 0 ? 0 : 1, 1); // Number+ / Number-
	for (int i = 0; i < std::abs(bankStep); ++i) patchQueue.push_back(bankButton);
	for (int i = 0; i < std::abs(numberStep); ++i) patchQueue.push_back(numberButton);

	patchSteps = int(patchQueue.size());
	patchPhase = 0;
	// 60 ms per half-press: two phases per button, i.e. an eighth of a second per press -
	// faster than a hand does it, and noticeably slower than the scan matrix, which is read
	// every firmware frame.
	startTimer(60);
}

void D110AudioProcessor::timerCallback() {
	if (patchQueue.empty()) {
		patchSteps = 0;
		stopTimer();
		return;
	}
	const int button = patchQueue.front();
	if (patchPhase == 0) {
		core.setButton(button, true);
		patchPhase = 1;
		return;
	}
	core.setButton(button, false);
	patchPhase = 0;
	patchQueue.erase(patchQueue.begin());
	patchSteps = int(patchQueue.size());
	if (patchQueue.empty()) stopTimer();
}

int D110AudioProcessor::currentPatchNumber() const {
	if (!core.isRunning()) return -1;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return -1;
	const int n = ram[(size_t)D110CoreType::kRamPatchNumber];
	return (n >= 0 && n < D110CoreType::kNumPatches) ? n : -1;
}

// A patch edit, audible at once. Patch memory is always written, and if the patch being
// edited is the one the unit is playing, the same value also goes to the live area - where
// the unit actually takes its sound from.
//
// The patch record layout is measured (docs/sysex_address_map.md): name 0-9, reverb 10-12,
// partial reserve 13-21, channel map 22-30, then eight 12-byte part records from 31. Their
// live twins differ: for the parts it is Timbre Temporary, for reverb, reserve and channels
// the System area.
void D110AudioProcessor::editPatchField(int patch, int field, juce::uint8 value) {
	sendPatchMemoryParam(patch, field, value);
	if (patch != currentPatchNumber()) return;

	if (field >= 31 && field < 31 + 8 * 12) {
		const int part = (field - 31) / 12;
		const int offset = (field - 31) % 12;

		// "Which tone the part plays" is a PAIR of bytes, group and number, and it must be carried
		// as a pair. Carrying one byte leaves each side with its own: b01 in the patch record, a01
		// in the live area - the number agrees, the group does not, and the drawer and the display
		// name different tones. Measured: groups pulled apart, number turned, result patch (1,0)
		// against live area (0,0) (plugin/editor_test.cpp, section 5).
		if (offset == 0 || offset == 1) {
			std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
			if (!core.getRam(ram.data())) return;
			const size_t at = size_t(D110CoreType::kRamPatches)
			                + size_t(patch) * D110CoreType::kPatchRecord + 31 + size_t(part) * 12;
			// The pair's second byte is taken from the patch record itself: the copy in RAM does not yet
			// know about the edit that just left as an exclusive message, so the byte being edited is
			// substituted by hand.
			juce::uint8 pair[2] = { ram[at], ram[at + 1] };
			pair[offset] = value & 0x7f;
			sendAreaData(D110CoreType::kSysexTimbreTemp, part * D110CoreType::kTimbreTempRecord, pair, 2);
			return;
		}
		sendTimbreTempParam(part, offset, value);
		return;
	}
	// The reverb type is not carried to the engine (eight types do not map onto four modes), but
	// it does go to the firmware: the value on the unit's display must change.
	if (field >= 10 && field <= 12) { sendSystemParam(field - 9, value); return; }
	if (field >= 13 && field <= 21) { sendSystemParam(field - 9, value); return; }
	if (field >= 22 && field <= 30) { sendSystemParam(field - 9, value); return; }
}

// A whole tone is 246 bytes, and one Roland exclusive message holds 244, so it goes in two
// pieces, exactly as an external librarian would send it.
static void sendToneBlock(D110AudioProcessor &proc, juce::uint32 address, int offset,
                          const uint8_t *from) {
	constexpr int kChunk = 123;
	for (int off = 0; off < D110CoreType::kToneRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneRecord - off);
		proc.sendAreaData(address, offset + off, from + off, len);
	}
}

void D110AudioProcessor::auditionTone(int part, int slot) {
	if (part < 0 || part > 7 || slot < 0 || slot >= D110CoreType::kNumTones) return;
	if (!core.isRunning()) return;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	sendToneBlock(*this, D110CoreType::kSysexToneTemp, part * D110CoreType::kToneRecord,
	              ram.data() + D110CoreType::kRamTones + slot * D110CoreType::kToneMemRecord);
}

void D110AudioProcessor::storeToneFromPart(int part, int slot) {
	if (part < 0 || part > 7 || slot < 0 || slot >= D110CoreType::kNumTones) return;
	if (!core.isRunning()) return;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	sendToneBlock(*this, D110CoreType::kSysexTones, slot * D110CoreType::kToneMemRecord,
	              ram.data() + D110CoreType::kRamToneTemp + part * D110CoreType::kToneRecord);
}

void D110AudioProcessor::logIncomingMidi(const juce::MidiMessage &message) {
	const auto *raw = message.getRawData();
	const int size = message.getRawDataSize();
	if (size <= 0) return;
	// Clock ticks and active sensing come as a stream - hundreds a second - and would push
	// everything meaningful out of the log.
	if (raw[0] == 0xF8 || raw[0] == 0xFE) return;

	MidiLogEntry e;
	e.status = raw[0];
	e.data1 = size > 1 ? raw[1] : 0;
	e.data2 = size > 2 ? raw[2] : 0;
	e.size = juce::uint16(juce::jmin(size, 65535));
	const juce::uint32 w = midiLogWrite.load(std::memory_order_relaxed);
	midiLog[(size_t)(w % kMidiLogSize)] = e;
	midiLogWrite.store(w + 1, std::memory_order_release);
}

int D110AudioProcessor::getMidiLog(MidiLogEntry *out, int max) const {
	const juce::uint32 w = midiLogWrite.load(std::memory_order_acquire);
	const int have = int(juce::jmin<juce::uint32>(w, kMidiLogSize));
	const int n = juce::jmin(have, max);
	for (int i = 0; i < n; ++i)
		out[i] = midiLog[(size_t)((w - 1 - juce::uint32(i)) % kMidiLogSize)];
	return n;
}

// Compressing before base64 is worth the few lines: the firmware's 32 KB battery RAM is
// mostly repeated patch and timbre records, so it packs down hard, and a project file
// should not carry ~88 KB of text per instance for something this repetitive.
static juce::String packBlock(const juce::MemoryBlock &raw) {
	if (raw.getSize() == 0) return {};
	juce::MemoryOutputStream compressed;
	{
		juce::GZIPCompressorOutputStream gzip(compressed);
		gzip.write(raw.getData(), raw.getSize());
	}
	return juce::Base64::toBase64(compressed.getData(), compressed.getDataSize());
}

static juce::MemoryBlock unpackBlock(const juce::String &text) {
	juce::MemoryBlock out;
	if (text.isEmpty()) return out;
	juce::MemoryOutputStream compressed;
	if (!juce::Base64::convertFromBase64(compressed, text)) return out;
	juce::MemoryInputStream source(compressed.getData(), compressed.getDataSize(), false);
	juce::GZIPDecompressorInputStream gzip(source);
	juce::MemoryOutputStream expanded;
	expanded.writeFromInputStream(gzip, -1);
	out.append(expanded.getData(), expanded.getDataSize());
	return out;
}

void D110AudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
	auto state = parameters.copyState();
	std::unique_ptr<juce::XmlElement> xml(state.createXml());
	xml->setAttribute("controlRomPath", controlRomPath);
	xml->setAttribute("pcmRomPath", pcmRomPath);
	// Ports are stored by identifier, not by name: names repeat across machines and change
	// when a device is renamed, whereas the identifier is what openDevice actually wants.
	xml->setAttribute("midiIn", selInputId);
	xml->setAttribute("midiOut", selOutputId);

	// The firmware's own memory - its patches, its timbres, its edits - so that a project
	// recalls the sounds it was saved with instead of whatever the shared folder happens
	// to hold now.
	//
	// Standalone deliberately skips this round trip: there is no "project" there beyond this
	// one settings blob, which JUCE only re-saves on a clean window close - so a cached copy
	// from days ago could, and did, silently overwrite fresher on-disk RAM the next time the
	// app launched (setStateInformation runs unconditionally at startup, before the user does
	// anything). The shared nvram files are already the single, always-current source of
	// truth for Standalone (written on POWER OFF and, now, on quit - see
	// flushLiveNvramToDisk()), so there is nothing for this blob to usefully add there.
	// Kept for VST3/AU/etc, where a saved host project genuinely should carry the
	// instrument's exact state with it.
	if (wrapperType != wrapperType_Standalone) {
		// While the machine is running the FILE on disk is stale: MAME only writes its NVRAM
		// out when the machine exits. So take the live image straight from the core, and fall
		// back to the file only when powered off.
		juce::MemoryBlock rams;
		if (core.isRunning()) {
			rams.setSize(D110CoreType::kRamSize);
			if (!core.getRam(static_cast<juce::uint8 *>(rams.getData()))) rams.reset();
		}
		if (rams.getSize() == 0) rams = readNvramFile("rams");

		// The card follows the same rule as the battery-backed RAM: while the machine is running
		// the file on disk is stale and the live contents sit in the core. The core is also what
		// serves an ejected card, whose slot in the machine's shared memory is currently all 0xFF.
		juce::MemoryBlock memcs;
		if (core.isRunning()) {
			memcs.setSize(D110CoreType::kCardSize);
			if (!core.getCardImage(static_cast<juce::uint8 *>(memcs.getData()))) memcs.reset();
		}
		if (memcs.getSize() == 0) memcs = readNvramFile("memcs");

		xml->setAttribute("nvramRams", packBlock(rams));
		xml->setAttribute("nvramMemcs", packBlock(memcs));
	}
	// The socket and the write-protect switch are state of the unit, just like memory contents:
	// a project saved with the card out must open with the card out.
	xml->setAttribute("cardInserted", core.cardInserted() ? 1 : 0);
	xml->setAttribute("cardWriteProtect", core.cardWriteProtect() ? 1 : 0);

	// The on-screen test keyboard's own config - see D110AudioProcessor::getKeyboardMidiChannel()
	// and friends for why this lives on the processor rather than the UI component.
	xml->setAttribute("kbChannel", keyboardMidiChannel);
	// "kbMidiRemap" since 2026-08-25 - see setStateInformation()'s own comment for the
	// legacy "kbOmni" fallback this replaces (same underlying setting, inverted sense).
	xml->setAttribute("kbMidiRemap", midiRemap ? 1 : 0);
	xml->setAttribute("kbPcInput", keyboardPcInput ? 1 : 0);
	xml->setAttribute("kbPcLayout", keyboardPcLayout);
	xml->setAttribute("kbNumOctaves", keyboardNumOctaves);

	// Utility tab's THEME toggle - see getUiThemeLight().
	xml->setAttribute("uiThemeLight", uiThemeLight ? 1 : 0);
	// See getUiThemeFollowSystem().
	xml->setAttribute("uiThemeFollowSystem", uiThemeFollowSystem ? 1 : 0);
	// See getUiFontScaleBig().
	xml->setAttribute("uiFontScaleBig", uiFontScaleBig ? 1 : 0);
	// See getSequencerRetroMode().
	xml->setAttribute("sequencerRetroMode", sequencerRetroMode ? 1 : 0);
	xml->setAttribute("sequencerGridMode", sequencerGridMode ? 1 : 0);
	xml->setAttribute("gridRowHeight", gridRowHeight);
	// See getCompactPanelMode().
	xml->setAttribute("compactPanelMode", compactPanelMode ? 1 : 0);
	// See D110SequencerHost::getRetroKeyBindings().
	xml->setAttribute("retroKeyBindings", retroKeyBindings);
	// See D110SequencerHost::getRetroLcdCompactMode().
	xml->setAttribute("retroLcdCompactMode", retroLcdCompactMode ? 1 : 0);
	// See getLastDialogDir().
	xml->setAttribute("lastDialogDir", lastDialogDir.getFullPathName());
	// Editor drawer's own height, drag-resized via the keyboard handle band - see
	// getEditorPaneRefH().
	xml->setAttribute("editorPaneRefH", editorPaneRefH);
	// Keyboard/sequencer drawers' own heights - see getKeyboardPaneRefH()/getSequencerPaneRefH().
	xml->setAttribute("keyboardPaneRefH", keyboardPaneRefH);
	xml->setAttribute("sequencerPaneRefH", sequencerPaneRefH);

	// The D-20-style sequencer's own tracks and transport settings, packed the same way
	// the firmware NVRAM is above - so a project brings the whole sequencer back exactly
	// as left, not just the instrument. The playhead position and armed/record/play state
	// are deliberately NOT saved: like most sequencers, a reopened project starts stopped
	// at bar 1 rather than mid-transport.
	// Metronome, precount, record mode and loop/punch read as workspace preferences rather
	// than song content (see D110SequencerEngine::newSong()'s own comment), so they're saved
	// once, not per slot.
	xml->setAttribute("seqMetronome", sequencerEngine.getMetronomeEnabled() ? 1 : 0);
	xml->setAttribute("seqMetronomeMode", static_cast<int>(sequencerEngine.getMetronomeMode()));
	xml->setAttribute("seqMetronomeChannel10", sequencerEngine.getMetronomeUseChannel10() ? 1 : 0);
	xml->setAttribute("seqMetronomeRecordOnly", sequencerEngine.getMetronomeRecordOnly() ? 1 : 0);
	xml->setAttribute("seqMetronomeVolume", double(sequencerEngine.getMetronomeVolume()));
	xml->setAttribute("seqPrecountBars", sequencerEngine.getPrecountBars());
	xml->setAttribute("seqRecordMode", static_cast<int>(sequencerEngine.getRecordMode()));
	xml->setAttribute("seqQuantizeMode", static_cast<int>(sequencerEngine.getQuantizeMode()));
	xml->setAttribute("seqStepGrid", static_cast<int>(sequencerEngine.getStepDuration()));
	xml->setAttribute("seqStepDotted", sequencerEngine.getStepDotted() ? 1 : 0);
	xml->setAttribute("seqLoopMode", static_cast<int>(sequencerEngine.getLoopMode()));
	xml->setAttribute("seqPunchIn", sequencerEngine.getPunchIn());
	xml->setAttribute("seqPunchOut", sequencerEngine.getPunchOut());

	// The per-track Program Change/Bank/Volume/Pan override used to be saved here as a
	// workspace preference (pcTrack<n>/bankTrack<n>/volTrack<n>/panTrack<n>) - it's per-slot
	// data now (2026-08-21), saved by writeSongsXml() below along with everything else that's
	// part of a song. An OLD project's pcTrack0../bankTrack0.. etc. attributes are simply no
	// longer read by setStateInformation() - see its own comment on why that's fine.

	// Per-song sound snapshot - see hasSoundSnapshot()/storeSoundSnapshotForSlot(). Unlike
	// the per-track override just above, this genuinely IS song content (that's the whole
	// point of it), so it's keyed per slot, same as the tracks themselves - but only written
	// when a slot actually has one stored, so a project that never uses the feature doesn't
	// carry 4 empty base64 blobs around.
	for (int s = 0; s < d110seq::D110SequencerEngine::kNumSongSlots; ++s) {
		const auto &snap = songSoundSnapshots[static_cast<size_t>(s)];
		if (snap.getSize() > 0) xml->setAttribute("soundSnapshotSlot" + juce::String(s), snap.toBase64Encoding());
	}

	// Tempo, time signature and the 9 tracks ARE song content, so all kNumSongSlots songs
	// are saved, not just the one currently selected - see writeSequencerSongsXml()'s own
	// comment. "seqCurrentSlot" being present at all is what tells setStateInformation this
	// is the new, slotted format rather than an older single-song project - see its own
	// comment.
	writeSequencerSongsXml(*xml);

	// Custom PCM wave samples (see loadCustomPcmWave()) - instrument-wide, not per-song, since
	// they replace what a wave NUMBER sounds like everywhere it's referenced, the same way
	// swapping a real ROM chip would be. Sparse (only customized slots are listed) rather than
	// one attribute per possible wave index, since almost every project will customize few if
	// any of the 256 slots.
	{
		juce::StringArray indices;
		for (const auto &entry : customPcmWaves) {
			indices.add(juce::String(entry.first));
			juce::MemoryBlock raw(entry.second.data(), entry.second.size() * sizeof(MT32Emu::Bit16s));
			xml->setAttribute("pcmWave" + juce::String(entry.first), packBlock(raw));
			const auto nameIt = customPcmWaveNames.find(entry.first);
			if (nameIt != customPcmWaveNames.end())
				xml->setAttribute("pcmWaveName" + juce::String(entry.first), nameIt->second);
			const auto loopIt = customPcmWaveLoops.find(entry.first);
			if (loopIt != customPcmWaveLoops.end())
				xml->setAttribute("pcmWaveLoop" + juce::String(entry.first), loopIt->second ? 1 : 0);
		}
		if (!indices.isEmpty()) xml->setAttribute("pcmWaveIndices", indices.joinIntoString(","));
	}

	copyXmlToBinary(*xml, destData);
}

// Both of these just delegate to D110SequencerSongsFile.h now - kept as thin wrappers
// (rather than updating every call site) since that's the smaller diff, and the doc
// comment on their declaration explaining why the plugin's own state save and the
// standalone .midiseq file share this logic still applies unchanged.
void D110AudioProcessor::writeSequencerSongsXml(juce::XmlElement &xml) const {
	d110seq::writeSongsXml(sequencerEngine, xml);
}

void D110AudioProcessor::readSequencerSongsXml(const juce::XmlElement &xml) {
	d110seq::readSongsXml(sequencerEngine, xml);
}

void D110AudioProcessor::exportSequencerSongs(const juce::File &file) {
	lastImportMessage = d110seq::exportSongsFile(sequencerEngine, file);
}

void D110AudioProcessor::importSequencerSongs(const juce::File &file) {
	lastImportMessage = d110seq::importSongsFile(sequencerEngine, file);
}

void D110AudioProcessor::setStateInformation(const void *data, int sizeInBytes) {
	std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
	if (xml == nullptr || !xml->hasTagName(parameters.state.getType())) return;

	parameters.replaceState(juce::ValueTree::fromXml(*xml));

	const auto crp = xml->getStringAttribute("controlRomPath");
	const auto prp = xml->getStringAttribute("pcmRomPath");
	if (crp.isNotEmpty() && prp.isNotEmpty()) {
		controlRomPath = crp;
		pcmRomPath = prp;
		openSynthIfReady();
	}

	// The forwardNotes attribute of old projects is deliberately ignored. While it was a menu
	// toggle it could be saved switched off; now there is no toggle, and a restored "off" would
	// leave the instrument without part indicators and without the firmware's own key ranges,
	// with no way to switch it back on.
	forwardNotes = true;

	// Reopening by identifier: if the device is not present on this machine the call simply
	// finds nothing and the plugin stays on host MIDI alone, which is the right outcome for
	// a project carried to a different computer.
	setMidiInputDevice(xml->getStringAttribute("midiIn"));
	setMidiOutputDevice(xml->getStringAttribute("midiOut"));

	// Put the project's own firmware memory back, while the machine is certainly not
	// running - the plugin always comes up powered off, and MAME reads these files once at
	// start. Hitting POWER then boots the instrument the project was saved with.
	//
	// This overwrites the shared memory, and that is the intent: there is one instrument,
	// so loading a project sets it to what that project had, exactly as loading a bank into
	// the hardware would. A plugin loaded WITHOUT a project touches none of this and simply
	// finds the memory as it was last left.
	//
	// Standalone never wrote this attribute to begin with (see getStateInformation) - guarded
	// here too in case an older settings file still has one cached, so it can't clobber the
	// shared nvram files with a stale snapshot on launch, which is what caused real data loss.
	if (wrapperType != wrapperType_Standalone) {
		const auto rams = unpackBlock(xml->getStringAttribute("nvramRams"));
		const auto memcs = unpackBlock(xml->getStringAttribute("nvramMemcs"));
		if (rams.getSize() > 0 || memcs.getSize() > 0)
			writeNvramFiles(rams, memcs);
	}

	// A project older than this feature never ejected the card, so by default it is in place.
	// Its contents already sit in the file above, and the core picks them up at power-on even
	// with the card out - see D110CoreType::osdApplyCard.
	core.setCardInserted(xml->getIntAttribute("cardInserted", 1) != 0);
	core.setCardWriteProtect(xml->getIntAttribute("cardWriteProtect", 0) != 0);

	// The on-screen test keyboard's own config - a project saved before this existed simply
	// has none of these attributes, and every getAttribute call below already has this
	// instance's own current (default) value as its fallback.
	setKeyboardMidiChannel(xml->getIntAttribute("kbChannel", keyboardMidiChannel));
	// "kbMidiRemap" (2026-08-25) replaced "kbOmni" - same setting, inverted sense (Omni=on
	// meant no remap). A project saved before the rename only has the old attribute; fall
	// back to it, inverted, rather than silently reverting such a project to today's default.
	if (xml->hasAttribute("kbMidiRemap"))
		setMidiRemap(xml->getIntAttribute("kbMidiRemap", midiRemap ? 1 : 0) != 0);
	else
		setMidiRemap(xml->getIntAttribute("kbOmni", midiRemap ? 0 : 1) == 0);
	setKeyboardPcInputEnabled(xml->getIntAttribute("kbPcInput", keyboardPcInput ? 1 : 0) != 0);
	setKeyboardPcLayout(xml->getIntAttribute("kbPcLayout", keyboardPcLayout));
	setKeyboardNumOctaves(xml->getIntAttribute("kbNumOctaves", keyboardNumOctaves));

	setUiThemeLight(xml->getIntAttribute("uiThemeLight", uiThemeLight ? 1 : 0) != 0);
	setUiThemeFollowSystem(xml->getIntAttribute("uiThemeFollowSystem", uiThemeFollowSystem ? 1 : 0) != 0);
	setUiFontScaleBig(xml->getIntAttribute("uiFontScaleBig", uiFontScaleBig ? 1 : 0) != 0);
	setSequencerRetroMode(xml->getIntAttribute("sequencerRetroMode", sequencerRetroMode ? 1 : 0) != 0);
	if (!sequencerRetroMode)
		setSequencerGridMode(xml->getIntAttribute("sequencerGridMode", sequencerGridMode ? 1 : 0) != 0);
	gridRowHeight = xml->getIntAttribute("gridRowHeight", gridRowHeight);
	setCompactPanelMode(xml->getIntAttribute("compactPanelMode", compactPanelMode ? 1 : 0) != 0);
	setRetroKeyBindings(xml->getStringAttribute("retroKeyBindings", retroKeyBindings));
	setRetroLcdCompactMode(xml->getIntAttribute("retroLcdCompactMode", retroLcdCompactMode ? 1 : 0) != 0);
	setLastDialogDir(juce::File(xml->getStringAttribute("lastDialogDir", lastDialogDir.getFullPathName())));
	setEditorPaneRefH(float(xml->getDoubleAttribute("editorPaneRefH", double(editorPaneRefH))));
	setKeyboardPaneRefH(float(xml->getDoubleAttribute("keyboardPaneRefH", double(keyboardPaneRefH))));
	setSequencerPaneRefH(float(xml->getDoubleAttribute("sequencerPaneRefH", double(sequencerPaneRefH))));

	// The sequencer's own tracks and transport settings - see the matching comment in
	// getStateInformation. A project saved before this existed simply has none of these
	// attributes, and every getAttribute call below already has the engine's own default
	// as its fallback, so an old project loads with a fresh, empty sequencer rather than
	// an error.
	sequencerEngine.setMetronomeEnabled(xml->getIntAttribute("seqMetronome", 1) != 0);
	sequencerEngine.setMetronomeMode(static_cast<d110seq::MetronomeMode>(
	    xml->getIntAttribute("seqMetronomeMode", static_cast<int>(d110seq::MetronomeMode::both))));
	sequencerEngine.setMetronomeUseChannel10(xml->getIntAttribute("seqMetronomeChannel10", 0) != 0);
	sequencerEngine.setMetronomeRecordOnly(xml->getIntAttribute("seqMetronomeRecordOnly", 0) != 0);
	sequencerEngine.setMetronomeVolume(float(xml->getDoubleAttribute("seqMetronomeVolume", 1.0)));
	// "seqPrecountBars" is the current attribute; "seqPrecount" (a plain 0/1 toggle) is
	// what an older project saved before precount became a 0/1/2-bar cycle - read as 1
	// bar when it was on, matching that toggle's old meaning.
	if (xml->hasAttribute("seqPrecountBars"))
		sequencerEngine.setPrecountBars(xml->getIntAttribute("seqPrecountBars", 1));
	else
		sequencerEngine.setPrecountBars(xml->getIntAttribute("seqPrecount", 1) != 0 ? 1 : 0);
	sequencerEngine.setRecordMode(static_cast<d110seq::RecordMode>(
		xml->getIntAttribute("seqRecordMode", static_cast<int>(d110seq::RecordMode::replaceRange))));
	sequencerEngine.setQuantizeMode(static_cast<d110seq::QuantizeMode>(
		xml->getIntAttribute("seqQuantizeMode", static_cast<int>(d110seq::QuantizeMode::hard))));
	sequencerEngine.setStepDuration(static_cast<d110seq::QuantizeGrid>(
		xml->getIntAttribute("seqStepGrid", static_cast<int>(d110seq::QuantizeGrid::eighth))));
	sequencerEngine.setStepDotted(xml->getIntAttribute("seqStepDotted", 0) != 0);
	sequencerEngine.setPunchIn(xml->getIntAttribute("seqPunchIn", sequencerEngine.getPunchIn()));
	sequencerEngine.setPunchOut(xml->getIntAttribute("seqPunchOut", sequencerEngine.getPunchOut()));
	sequencerEngine.setLoopMode(static_cast<d110seq::LoopMode>(
		xml->getIntAttribute("seqLoopMode", static_cast<int>(d110seq::LoopMode::off))));

	// pcTrack<n>/bankTrack<n>/volTrack<n>/panTrack<n> (a workspace-wide override, one value
	// shared by all 4 songs) are no longer read - see getStateInformation()'s own comment. A
	// project saved before this changed (2026-08-21) simply loads with that override cleared;
	// readSongsXml() below is what now restores it, per slot, from that project's own
	// (already-per-slot) seqProgram<slot><track> etc. attributes if it was saved after the
	// change, or leaves it at "off" (Track's own default) if the project predates it.
	for (int s = 0; s < d110seq::D110SequencerEngine::kNumSongSlots; ++s) {
		songSoundSnapshots[static_cast<size_t>(s)] = {};
		if (xml->hasAttribute("soundSnapshotSlot" + juce::String(s))) {
			juce::MemoryBlock block;
			if (block.fromBase64Encoding(xml->getStringAttribute("soundSnapshotSlot" + juce::String(s)))
			    && block.getSize() == size_t(D110CoreType::kRamSize))
				songSoundSnapshots[static_cast<size_t>(s)] = std::move(block);
		}
	}

	// "seqCurrentSlot" only exists in projects saved after the 4-song-slot feature - an
	// older project has a single, unslotted song under the plain "seqTempo"/"seqTrack0"/...
	// names, which lands in slot 0 here; slots 1-3 simply stay empty.
	if (xml->hasAttribute("seqCurrentSlot")) {
		readSequencerSongsXml(*xml);
	} else {
		sequencerEngine.setTempo(xml->getDoubleAttribute("seqTempo", sequencerEngine.getTempo()));
		sequencerEngine.setTimeSignature(
			xml->getIntAttribute("seqTimeSigNum", sequencerEngine.getTimeSigNumerator()),
			xml->getIntAttribute("seqTimeSigDen", sequencerEngine.getTimeSigDenominator()));
		for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t) {
			const juce::String suffix(t);
			const auto trackBytes = unpackBlock(xml->getStringAttribute("seqTrack" + suffix));
			sequencerEngine.trackFromBytes(t, trackBytes.getData(), trackBytes.getSize());
			sequencerEngine.setTrackMuted(t, xml->getIntAttribute("seqMute" + suffix, 0) != 0);
			sequencerEngine.setTrackSoloed(t, xml->getIntAttribute("seqSolo" + suffix, 0) != 0);
			sequencerEngine.quantizeTrack(
				t, static_cast<d110seq::QuantizeGrid>(xml->getIntAttribute("seqQuantize" + suffix, 0)));
		}
	}

	// Custom PCM wave samples - see getStateInformation()'s own comment. Populates
	// customPcmWaves from the saved (gzip+base64) blobs; reapplyCustomPcmWaves() then either
	// applies them right away (if the synth's already open, e.g. reloading state into a live
	// instance) or is a no-op here and gets picked up by openSynthIfReady()'s own call to it
	// the next time the instrument powers on.
	customPcmWaves.clear();
	customPcmWaveNames.clear();
	customPcmWaveLoops.clear();
	factoryPcmWaveBackup.clear();
	for (const auto &token : juce::StringArray::fromTokens(xml->getStringAttribute("pcmWaveIndices"), ",", "")) {
		if (token.trim().isEmpty()) continue;
		const int waveIndex = token.getIntValue();
		const auto raw = unpackBlock(xml->getStringAttribute("pcmWave" + juce::String(waveIndex)));
		if (raw.getSize() == 0 || (raw.getSize() % sizeof(MT32Emu::Bit16s)) != 0) continue;
		const auto *samples = static_cast<const MT32Emu::Bit16s *>(raw.getData());
		customPcmWaves[waveIndex].assign(samples, samples + raw.getSize() / sizeof(MT32Emu::Bit16s));
		const auto nameAttr = "pcmWaveName" + juce::String(waveIndex);
		if (xml->hasAttribute(nameAttr)) customPcmWaveNames[waveIndex] = xml->getStringAttribute(nameAttr);
		const auto loopAttr = "pcmWaveLoop" + juce::String(waveIndex);
		if (xml->hasAttribute(loopAttr)) customPcmWaveLoops[waveIndex] = xml->getIntAttribute(loopAttr) != 0;
	}
	reapplyCustomPcmWaves();
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
	return new D110AudioProcessor();
}
