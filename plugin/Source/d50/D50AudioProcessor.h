#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
// MidiMessageCollector lives here, not in juce_audio_processors - see
// PluginProcessor.h's own comment on the same include.
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <vector>

#include "../../../d50/D5_Bridge.h"
#include "../D110KeyboardHost.h"
#include "../sequencer/D110SequencerEngine.h"
#include "../sequencer/D110SequencerHost.h"

// The D-50 plugin's AudioProcessor. Unlike D110AudioProcessor this owns no
// firmware/CPU emulation at all - d50/D5_Bridge.h is Michi71's reimplemented
// LA engine (see CLAUDE.md's D-50 section and d50/UPSTREAM_README.md), and
// this class is just the JUCE-side wiring around it: MIDI in -> D5_Bridge,
// D5_Bridge's fixed 32 kHz stereo output -> resampled to the host rate.
//
// The 512 KiB PCM sample blob (d5_pcm.bin, decoded from Alan's own D-50 ROM
// dump) is never compiled into this binary - loaded from disk at runtime,
// same reasoning as the D-110 firmware ROMs never being embedded either. See
// loadPcmBlob()'s own comment for the search path.
class D50AudioProcessor : public juce::AudioProcessor, public D110KeyboardHost, public D110SequencerHost {
public:
    D50AudioProcessor();
    ~D50AudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "D-50 Emulator"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String &) override {}

    void getStateInformation(juce::MemoryBlock &destData) override;
    void setStateInformation(const void *data, int sizeInBytes) override;

    // --- D-50 specific, for the editor -------------------------------------
    bool isReady() const { return bridgeReady; }
    juce::String statusMessage() const { return status; }
    int patchCount() const { return bridgeReady ? bridge.patchCount() : 0; }
    juce::String patchNameAt(int index) const {
        return bridgeReady ? juce::String(bridge.patchNameAt(index)) : juce::String();
    }
    int currentPatch() const { return bridgeReady ? bridge.patch() : 0; }
    void setPatch(int index) {
        if (bridgeReady) bridge.selectPatch(index);
    }
    juce::String pcmWaveName(int waveNumber0to99) const { return juce::String(D5_Bridge::pcmWaveName(waveNumber0to99)); }
    int getVolumePercent() const { return volumePercent; }
    void setVolumePercent(int v);
    int getReverbPercent() const { return bridgeReady ? bridge.reverbBalance() : 0; }
    void setReverbPercent(int v) {
        if (bridgeReady) bridge.setReverb(v);
    }
    int getChorusPercent() const { return bridgeReady ? bridge.chorusBalance() : 0; }
    void setChorusPercent(int v) {
        if (bridgeReady) bridge.setChorus(v);
    }
    // Solo/mute any one of the four partials for auditioning - see
    // D5_Bridge::setUpperPartial1Mute()'s own comment. Not real D-50 panel
    // controls, not persisted with the patch: a listening aid only.
    // TVF ENV DEPTH keyfollow direction, as a one-click A/B (see
    // D5_Bridge::setTvfKeyfollowFixed()). Unlike the mutes this is NOT just a
    // listening aid - it changes what the instrument sounds like - so it does
    // persist with the rest of the state. Engine-global, hence no bridgeReady
    // guard: it is meaningful before a ROM ever loads.
    bool getTvfKeyfollowFixed() const { return D5_Bridge::tvfKeyfollowFixed(); }
    void setTvfKeyfollowFixed(bool fixed) { D5_Bridge::setTvfKeyfollowFixed(fixed); }

    bool getUpperPartial1Muted() const { return bridgeReady && bridge.upperPartial1Muted(); }
    void setUpperPartial1Muted(bool m) {
        if (bridgeReady) bridge.setUpperPartial1Mute(m);
    }
    bool getUpperPartial2Muted() const { return bridgeReady && bridge.upperPartial2Muted(); }
    void setUpperPartial2Muted(bool m) {
        if (bridgeReady) bridge.setUpperPartial2Mute(m);
    }
    bool getLowerPartial1Muted() const { return bridgeReady && bridge.lowerPartial1Muted(); }
    void setLowerPartial1Muted(bool m) {
        if (bridgeReady) bridge.setLowerPartial1Mute(m);
    }
    bool getLowerPartial2Muted() const { return bridgeReady && bridge.lowerPartial2Muted(); }
    void setLowerPartial2Muted(bool m) {
        if (bridgeReady) bridge.setLowerPartial2Mute(m);
    }

    // Per-partial PCM pitch offset (debug), -48..+48 semitones - see
    // D5_Bridge::setUpperPartial1PitchOffset()'s own comment. Same
    // listening-aid status as the mutes just above: not persisted, not a
    // real D-50 panel control.
    int getUpperPartial1PitchOffset() const { return bridgeReady ? bridge.upperPartial1PitchOffset() : 0; }
    void setUpperPartial1PitchOffset(int s) {
        if (bridgeReady) bridge.setUpperPartial1PitchOffset(s);
    }
    int getUpperPartial2PitchOffset() const { return bridgeReady ? bridge.upperPartial2PitchOffset() : 0; }
    void setUpperPartial2PitchOffset(int s) {
        if (bridgeReady) bridge.setUpperPartial2PitchOffset(s);
    }
    int getLowerPartial1PitchOffset() const { return bridgeReady ? bridge.lowerPartial1PitchOffset() : 0; }
    void setLowerPartial1PitchOffset(int s) {
        if (bridgeReady) bridge.setLowerPartial1PitchOffset(s);
    }
    int getLowerPartial2PitchOffset() const { return bridgeReady ? bridge.lowerPartial2PitchOffset() : 0; }
    void setLowerPartial2PitchOffset(int s) {
        if (bridgeReady) bridge.setLowerPartial2PitchOffset(s);
    }

    // Raw access to the sounding patch's 448 bytes (see d50/d5_engine/
    // d5_patch_map.h for what each offset means - PatchBlock enum times 64
    // gives a block's base offset, then that file's map_partial()/map_common()
    // comments give the per-byte meaning within it). Written through
    // D5_Bridge::sysexWriteTemp(), the same call a real SysEx parameter edit
    // would make - takes effect on the sounding voice immediately, matching
    // this project's own convention of editors writing through a real
    // protocol path rather than poking engine state directly (CLAUDE.md).
    int getPatchByte(int offset) const {
        return (bridgeReady && offset >= 0 && offset < D5_Bridge::kPatchBytes) ? bridge.tempPatch()[offset] : 0;
    }
    void setPatchByte(int offset, int value) {
        if (!bridgeReady) return;
        const auto v = static_cast<uint8_t>(juce::jlimit(0, 127, value));
        bridge.sysexWriteTemp(offset, &v, 1);
    }

    // Loads a real D-50 SysEx bulk dump from disk, replacing whatever bank is
    // currently active - same parser/validation as the ROM-folder auto-load
    // (D5SyxLoader), just user-triggered instead of found automatically at
    // startup. Returns a message for the caller to show (success or why not).
    juce::String importSyxBank(const juce::File &file);
    // Writes the currently-sounding 448 bytes (see getPatchByte()'s own
    // comment - includes whatever the Tone section has edited) as a single
    // DT1 SysEx message, address 02-00-00 (internal memory slot 0) - the
    // same address a real bulk dump's first patch uses, so this file can be
    // dropped straight into d50/roms/ or re-imported through the button next
    // to it. Returns false if there is no sounding patch to export yet.
    bool exportCurrentPatch(const juce::File &file) const;
    // Writes every patch in the active bank (patchCount() of them, address
    // 02-00-00 upward - the same bulk-dump shape importSyxBank()/the ROM-
    // folder auto-load already parse) - a real "save my edits" export, not
    // just the one patch: the currently SOUNDING slot is written from
    // tempPatch() (so a live Tone-tab edit is actually in the file), every
    // other slot from storedPatch() (unedited, or whatever an earlier
    // received bulk dump/overlay entry already holds for it). Returns false
    // if there is no bank loaded yet.
    bool exportBank(const juce::File &file) const;

    // --- Send to a real, physically-connected D-50 over external MIDI Out ---
    // Same shape as D110AudioProcessor's own MIDI Out picker/sender (Alan,
    // 2026-09-02: wants to A/B a patch's current bytes - including whatever
    // the Tone tab has live-edited - against his real unit, to tell a SysEx
    // decode bug from a synthesis-engine one). One processor-owned
    // juce::MidiOutput, reused by every "send" feature rather than one per
    // feature; opened/closed only from setMidiOutputDevice().
    static juce::Array<juce::MidiDeviceInfo> midiOutputs() { return juce::MidiOutput::getAvailableDevices(); }
    void setMidiOutputDevice(const juce::String &identifier);
    juce::String getMidiOutputId() const { return selOutputId; }
    bool hasExternalMidiOutput() const {
        const juce::ScopedLock lock(osMidiLock);
        return osMidiOut != nullptr;
    }
    // Writes the currently-sounding 448 bytes (see getPatchByte()'s own
    // comment) to the real D-50's TEMPORARY area (address 00-00-00, not
    // 02-00-00/internal memory - see D5_Midi.cpp's own `kTempBase` comment
    // upstream) - the same live "what's actually sounding" area our own
    // sysexWriteTemp() models, so this takes effect on the real unit at once
    // and never overwrites one of its 64 stored patches. Returns false if
    // there is no sounding patch or no MIDI Out device selected.
    bool sendPatchToExternalMidi() const;

    // --- D110KeyboardHost ----------------------------------------------------
    // The bridge is monotimbral and does not look at MIDI channel at all (unlike
    // the D-110's 9 parts on 9 channels), so channel/remap only matter for what
    // the on-screen keyboard itself displays - they never change what sounds.
    void injectTestNote(int channel, int note, float velocity, bool on) override;
    int getKeyboardMidiChannel() const override { return keyboardMidiChannel; }
    void setKeyboardMidiChannel(int channel) override { keyboardMidiChannel = channel; }
    bool getMidiRemap() const override { return midiRemap; }
    void setMidiRemap(bool remap) override { midiRemap = remap; }
    bool getKeyboardPcInputEnabled() const override { return keyboardPcInputEnabled; }
    void setKeyboardPcInputEnabled(bool enabled) override { keyboardPcInputEnabled = enabled; }
    int getKeyboardPcLayout() const override { return keyboardPcLayout; }
    void setKeyboardPcLayout(int layout) override { keyboardPcLayout = layout; }
    int getKeyboardNumOctaves() const override { return keyboardNumOctaves; }
    void setKeyboardNumOctaves(int numOctaves) override { keyboardNumOctaves = numOctaves; }
    int getKeyboardVelocity() const override { return keyboardVelocity; }
    void setKeyboardVelocity(int velocity) override { keyboardVelocity = juce::jlimit(1, 127, velocity); }

    // Normal/Big text and control size - see UiTheme.h's own d110ui::FontScale comment for why
    // actually applying it (juce::Desktop::getInstance().setGlobalScaleFactor()) is D50Editor's
    // job, Standalone builds only. Just the persisted preference here, same pattern as the
    // keyboard config above.
    bool getUiFontScaleBig() const { return uiFontScaleBig; }
    void setUiFontScaleBig(bool big) { uiFontScaleBig = big; }
    // Folded state of the editor's two FoldHandles, saved with the state (format version 8) so a
    // fold done by hand survives a relaunch. Defaults: patch edit open, sequencer folded.
    bool getPatchEditFolded() const { return patchEditFolded; }
    bool getSequencerFolded() const { return sequencerFolded; }
    void setFolds(bool patchEdit, bool sequencer) {
        patchEditFolded = patchEdit;
        sequencerFolded = sequencer;
    }
    bool isNoteActive(int note) const override {
        return note >= 0 && note < 128 && noteActiveFlags[static_cast<size_t>(note)].load(std::memory_order_relaxed);
    }

    // --- D110SequencerHost -----------------------------------------------------
    // Mono-timbral first cut (Alan, 2026-09-03): unlike the D-110's 9 tracks mapped onto 8
    // real Parts + Rhythm, the D-50 plays exactly ONE patch at a time regardless of MIDI
    // channel (see injectTestNote()'s own comment - the bridge is channel-blind, and
    // injectTestNote() itself already collapses the on-screen keyboard's broadcast onto
    // channel 1 unless MIDI Remap points it elsewhere). So every track here shares the
    // same sound; channel only matters for MIDI file export/import readability, not for
    // anything the engine does with it. A real per-track independent patch (something no
    // actual D-50 ever had) is a bigger follow-up Alan explicitly deferred - see
    // D110SequencerHost.h's own optional accessors, none of which are overridden here yet
    // (supportsProgramChange() and friends all default to false/no-op).
    d110seq::D110SequencerEngine &getSequencer() override { return sequencerEngine; }
    void exportSequencerSongs(const juce::File &file) override;
    void importSequencerSongs(const juce::File &file) override;
    void midiPanic() override;
    juce::File getLastDialogDir() const override { return lastDialogDir; }
    void setLastDialogDir(const juce::File &dir) override { lastDialogDir = dir; }

private:
    bool loadPcmBlob();
    void loadPatchBank();
    void handleMidiMessage(const juce::MidiMessage &m);
    // Renders exactly numOutSamples stereo samples at hostSampleRate into
    // outL/outR, pulling as much 32 kHz audio from the bridge as the
    // resampler needs and keeping whatever it doesn't consume for next time
    // (pendingL/R) - dropping that remainder instead would throw away real,
    // already-rendered audio and glitch the pitch every block.
    void renderInternal(int numOutSamples, float *outL, float *outR);

    D5_Bridge bridge;
    std::vector<int16_t> pcmBlob;
    bool bridgeReady = false;
    juce::String status{"D-50: not loaded yet"};

    double hostSampleRate = 44100.0;
    juce::LagrangeInterpolator resamplerL, resamplerR;
    std::vector<float> pendingL, pendingR;
    std::vector<int32_t> renderScratch;

    int volumePercent = 80;

    // RPN 0 (bender range): CC101/CC100 select the parameter, CC6 supplies
    // the value; 127/127 is MIDI's "no RPN selected" null.
    int rpnMsb = 127, rpnLsb = 127;

    // On-screen keyboard support: mouse/PC-keyboard clicks arrive on the message
    // thread via injectTestNote(), queued here exactly like the D-110 plugin's own
    // osMidiCollector and drained into the real MIDI stream from processBlock().
    juce::MidiMessageCollector injectedMidi;
    int keyboardMidiChannel = 1;
    bool midiRemap = false;
    bool keyboardPcInputEnabled = false;
    int keyboardPcLayout = 0;
    int keyboardNumOctaves = 2;
    int keyboardVelocity = 100;
    bool uiFontScaleBig = false;
    bool patchEditFolded = false;
public:
    // Last editor size (project state), restored when the editor opens; 0 = never saved.
    int editorWidth = 0, editorHeight = 0;
private:
    bool sequencerFolded = true;
    std::array<std::atomic<bool>, 128> noteActiveFlags{};

    // The sequencer - see the D110SequencerHost overrides above for how it's wired in.
    // 9 tracks fixed (kNumTracks), same as the D-110 plugin's own engine instance - never
    // enables extra tracks (supportsExtraTracks() stays false, the default), see
    // D110SequencerEngine.h's own comment on why kMaxTracks exists at all (Nonet Sequencer
    // only).
    d110seq::D110SequencerEngine sequencerEngine;
    juce::File lastDialogDir;
    // Metronome click synthesis state, carried across blocks so its ~30ms decay doesn't
    // reset/glitch at a block boundary - see processBlock()'s own comment, ported verbatim
    // from D110AudioProcessor's identically-shaped code.
    int metronomeSamplesRemaining = 0;
    double metronomePhase = 0.0;
    double metronomeFreq = 1000.0;

    // See setMidiOutputDevice()'s own comment.
    std::unique_ptr<juce::MidiOutput> osMidiOut;
    juce::String selOutputId;
    juce::CriticalSection osMidiLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D50AudioProcessor)
};
