#include "D50Editor.h"

#include <iterator>

#include "../DotMatrixFont.h"
#include "../UiTheme.h"

#if JucePlugin_Build_Standalone
// StandaloneFilterWindow.h assumes AudioProcessorPlayer/AudioDeviceSelectorComponent
// (both juce_audio_utils) are already visible - true when it's built as part of the
// juce_audio_plugin_client module's own unity build, not when pulled in on its own here.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace {
// Byte offsets into the 448-byte patch - see d50/d5_engine/d5_patch_map.h's
// PatchBlock enum and map_common()/map_partial()/patch_from_bytes() comments,
// which every table below mirrors exactly (same idiom as D110EditorPane's own
// kWg/kPitchEnv/kTvf/kTva tables in PluginEditor.cpp - a name/offset/max row
// per parameter rather than a hand-built widget per parameter). Only the
// UPPER tone is exposed - lower-tone editing (relevant only in Dual/Split key
// mode) is a follow-up, not built yet.
constexpr int kBlockSize = 64;
constexpr int kUpperP1Base = 0 * kBlockSize;
constexpr int kUpperP2Base = 1 * kBlockSize;
constexpr int kUpperCommonBase = 2 * kBlockSize;
constexpr int kLowerP1Base = 3 * kBlockSize;
constexpr int kLowerP2Base = 4 * kBlockSize;
constexpr int kLowerCommonBase = 5 * kBlockSize;
constexpr int kPatchBase = 6 * kBlockSize;
constexpr int kStructureOffset = 10;  // panel 1..7, stored 0..6
constexpr int kBalanceOffset = 47;    // Partial Balance, 0..100

// Each tone (upper AND lower) has its OWN name, ten characters at the very
// start of its own common block (bytes 0-9, right before Structure at byte
// 10) - separate from the patch's own name (patch block, D50AudioProcessor::
// patchNameAt()). Not documented anywhere this port had read before Alan
// checked a real unit's LCD against tone 4 of the real PN-D50-00 factory
// card (2026-09-02): the photographed display read "U: SynStrings" / "L:
// ArcoAttack" for "I-4 Arco Strings", and decoding these ten bytes here with
// the same panel character set patch names already use reproduces both
// strings exactly - which also confirms the upper/lower block assignment
// and Structure/Key Mode decoding are all correct, not just the name field.
juce::String decodeToneName(D50AudioProcessor &proc, int commonBase) {
    static const char kChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz1234567890-";
    juce::String out;
    for (int i = 0; i < 10; ++i) {
        const int v = proc.getPatchByte(commonBase + i) & 0x3F;
        out += juce::String::charToString(v < static_cast<int>(sizeof(kChars)) - 1 ? kChars[v] : ' ');
    }
    return out.trimEnd();
}

using ToneParam = D50Editor::ToneParam;

// Per-partial: the compact PartialPanel above already covers waveform (6),
// PCM wave (7), pulse width (8), cutoff (13) and resonance (14) - everything
// else map_partial() reads from these 64 bytes lives here.
// PW Velocity/LFO/AT Range are pulse-width modulation depth controls - the
// D-50's pulse width only exists on a synth WG waveform, so a PCM partial
// never reads these four (see D50Editor::updatePartialApplicability()).
constexpr ToneParam kWgExtra[] = {
    {"Pitch Coarse", 0, 72},   {"Pitch Fine", 1, 100},   {"Pitch Keyfollow", 2, 16},
    {"Bender Mode", 5, 2},     {"Pitch Env Mode", 4, 2}, {"PW Velocity", 9, 14, true},
    {"PW LFO Select", 10, 5, true},  {"PW LFO Depth", 11, 100, true}, {"PW AT Range", 12, 14, true},
};
constexpr ToneParam kTvfFull[] = {
    {"Cutoff Keyfollow", 15, 14}, {"Bias Point", 16, 127},   {"Bias Level", 17, 14},
    {"Env Depth", 18, 100},       {"Env Velocity", 19, 100}, {"Env Depth KF", 20, 4},
    {"Env Time KF", 21, 4},       {"Env T1", 22, 100},       {"Env T2", 23, 100},
    {"Env T3", 24, 100},          {"Env T4", 25, 100},       {"Env T5", 26, 100},
    {"Env L1", 27, 100},          {"Env L2", 28, 100},       {"Env L3", 29, 100},
    {"Env Sustain", 30, 100},     {"Env End", 31, 100},      {"LFO Select", 32, 5},
    {"LFO Depth", 33, 100},       {"AT Range", 34, 14},
};
constexpr ToneParam kTvaFull[] = {
    {"Velocity", 36, 100},   {"Bias Point", 37, 127}, {"Bias Level", 38, 12},
    {"Env T1", 39, 100},     {"Env T2", 40, 100},     {"Env T3", 41, 100},
    {"Env T4", 42, 100},     {"Env T5", 43, 100},     {"Env L1", 44, 100},
    {"Env L2", 45, 100},     {"Env L3", 46, 100},     {"Env Sustain", 47, 100},
    {"Env End", 48, 100},    {"Velocity KF", 49, 4},  {"Time KF", 50, 4},
    {"LFO Select", 51, 5},   {"LFO Depth", 52, 100},  {"AT Range", 53, 14},
};

// Tone-common (shared by both partials): pitch envelope, the three LFOs
// (same 4-byte shape at offsets 25/29/33 - one table, three ParamColumns),
// equalizer and chorus. Structure (10) and Partial Balance (47) already have
// their own friendly widgets above.
constexpr ToneParam kPenv[] = {
    {"Velocity Mode", 11, 2}, {"Time Keyfollow", 12, 4}, {"T1", 13, 50},
    {"T2", 14, 50},           {"T3", 15, 50},            {"T4", 16, 50},
    {"L0", 17, 100},          {"L1", 18, 100},           {"L2", 19, 100},
    {"Sustain", 20, 100},     {"End", 21, 100},          {"LFO Depth", 22, 100},
    {"Lever Amount", 23, 100}, {"AT Amount", 24, 100},
};
constexpr ToneParam kLfo[] = {
    {"Waveform", 0, 3},
    {"Rate", 1, 100},
    {"Delay", 2, 100},
    {"Sync", 3, 2},
};
constexpr ToneParam kEqChorus[] = {
    {"EQ Low Freq", 37, 15},  {"EQ Low Gain", 38, 24},   {"EQ High Freq", 39, 21},
    {"EQ High Q", 40, 8},     {"EQ High Gain", 41, 24},  {"Chorus Type", 42, 7},
    {"Chorus Rate", 43, 100}, {"Chorus Depth", 44, 100}, {"Chorus Balance", 45, 100},
    {"Partials On", 46, 3},
};
// Patch-level, shared by upper AND lower tone (key mode, split point, both
// tones' key shift/tune, portamento, bender/aftertouch range). Volume and
// Reverb Balance already have their own knobs, driven through the curved
// bridge setters rather than the raw byte, so they aren't repeated here.
constexpr ToneParam kPatchCommon[] = {
    {"Key Mode", 18, 8},          {"Split Point", 19, 91},   {"Portamento Mode", 20, 2},
    {"Upper Key Shift", 22, 48},  {"Lower Key Shift", 23, 48}, {"Upper Fine Tune", 24, 100},
    {"Lower Fine Tune", 25, 100}, {"Bender Range", 26, 12},  {"AT Bend Range", 27, 24},
    {"Portamento Time", 28, 100}, {"Reverb Type", 30, 31},   {"Portamento Switch", 41, 1},
};

juce::Slider &configureKnob(juce::Slider &s) {
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
    s.setRange(0.0, 100.0, 1.0);
    return s;
}
}  // namespace

D50Editor::PartialPanel::PartialPanel(D50AudioProcessor &proc, int blockBase, const juce::String &title)
    : processor(proc), base(blockBase) {
    titleLabel.setText(title, juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    addAndMakeVisible(titleLabel);

    waveformLabel.setText("Waveform", juce::dontSendNotification);
    addAndMakeVisible(waveformLabel);
    waveformBox.addItem("Square", 1);
    waveformBox.addItem("Sawtooth", 2);
    addAndMakeVisible(waveformBox);
    // WG Waveform, offset 6: strictly 0 or 1 on real hardware (see
    // d5_syx_to_patches.py's own CHECKS table) - 0 is Square, non-zero Saw.
    waveformBox.onChange = [this] { processor.setPatchByte(base + 6, waveformBox.getSelectedId() - 1); };

    auto setupSlider = [this](juce::Slider &s, juce::Label &l, const juce::String &name, double hi, int offset) {
        l.setText(name, juce::dontSendNotification);
        l.setFont(juce::Font(juce::FontOptions(13.0f)));
        addAndMakeVisible(l);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 45, 22);
        s.setRange(0.0, hi, 1.0);
        addAndMakeVisible(s);
        s.onValueChange = [this, &s, offset] { processor.setPatchByte(base + offset, static_cast<int>(s.getValue())); };
    };
    // PCM Wave is shown 1..100, matching Roland's own MIDI implementation
    // manual (Table 2, "Number" column) and this app's existing convention
    // for Structure (also a "pick the Nth item" field, also shown 1-based) -
    // the raw byte underneath is still 0..99 (Alan, 2026-09-02: checked this
    // exact table against d5_pcm_table.h's own name order and confirmed no
    // off-by-one - byte 45 is table Number 46, "Violns", both here and on
    // the printed page).
    pcmLabel.setText("PCM Wave", juce::dontSendNotification);
    pcmLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    addAndMakeVisible(pcmLabel);
    pcmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    pcmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 45, 22);
    pcmSlider.setRange(1.0, 100.0, 1.0);
    addAndMakeVisible(pcmSlider);
    pcmSlider.onValueChange = [this] {
        processor.setPatchByte(base + 7, static_cast<int>(pcmSlider.getValue()) - 1);
    };
    setupSlider(cutoffSlider, cutoffLabel, "Cutoff", 100.0, 13);
    setupSlider(resonanceSlider, resonanceLabel, "Resonance", 30.0, 14);
    setupSlider(pulseWidthSlider, pulseWidthLabel, "Pulse Width", 100.0, 8);

    // Debug-only, not a real patch byte - see setDebugPitchAccessors()'s own
    // comment. Sits right under PCM Wave, per Alan's request (2026-09-02),
    // to A/B a suspected octave-transposition bug on real PCM samples.
    debugPitchLabel.setText("PCM Pitch (debug)", juce::dontSendNotification);
    debugPitchLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    addAndMakeVisible(debugPitchLabel);
    debugPitchSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    debugPitchSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 45, 22);
    debugPitchSlider.setRange(-48.0, 48.0, 1.0);
    addAndMakeVisible(debugPitchSlider);
    debugPitchSlider.onValueChange = [this] {
        if (setDebugPitch) setDebugPitch(static_cast<int>(debugPitchSlider.getValue()));
    };
}

void D50Editor::PartialPanel::refresh() {
    waveformBox.setSelectedId(processor.getPatchByte(base + 6) == 0 ? 1 : 2, juce::dontSendNotification);
    const int wave = processor.getPatchByte(base + 7);
    pcmSlider.setValue(wave + 1, juce::dontSendNotification);
    pcmLabel.setText("PCM: " + processor.pcmWaveName(wave), juce::dontSendNotification);
    cutoffSlider.setValue(processor.getPatchByte(base + 13), juce::dontSendNotification);
    resonanceSlider.setValue(processor.getPatchByte(base + 14), juce::dontSendNotification);
    pulseWidthSlider.setValue(processor.getPatchByte(base + 8), juce::dontSendNotification);
    if (getDebugPitch) debugPitchSlider.setValue(getDebugPitch(), juce::dontSendNotification);
}

void D50Editor::PartialPanel::resized() {
    auto area = getLocalBounds();
    titleLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    auto row = [&](juce::Label &label, juce::Component &control) {
        auto r = area.removeFromTop(26);
        // Wide enough for "PCM: " plus the wave's six-character name (see
        // PartialPanel::refresh()) - the other four labels are short words,
        // so this only ever gives them a bit of unused margin, not a squeeze.
        label.setBounds(r.removeFromLeft(112));
        control.setBounds(r);
        area.removeFromTop(4);
    };
    row(waveformLabel, waveformBox);
    row(pcmLabel, pcmSlider);
    row(debugPitchLabel, debugPitchSlider);
    row(cutoffLabel, cutoffSlider);
    row(resonanceLabel, resonanceSlider);
    row(pulseWidthLabel, pulseWidthSlider);
}

// ---- ParamColumn: one titled group of raw-byte sliders from a ToneParam table.
D50Editor::ParamColumn::ParamColumn(D50AudioProcessor &proc, int columnBase, const juce::String &title,
                                     const ToneParam *params, int count, bool wholeColumnSynthOnly)
    : processor(proc), base(columnBase) {
    titleLabel.setText(title, juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    addAndMakeVisible(titleLabel);

    for (int i = 0; i < count; ++i) {
        auto *label = paramLabels.add(new juce::Label());
        label->setText(params[i].name, juce::dontSendNotification);
        label->setFont(juce::Font(juce::FontOptions(11.5f)));
        addAndMakeVisible(label);

        auto *slider = sliders.add(new juce::Slider());
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 18);
        slider->setRange(0.0, static_cast<double>(params[i].hi), 1.0);
        addAndMakeVisible(slider);

        const int offset = params[i].offset;
        slider->onValueChange = [this, slider, offset] {
            processor.setPatchByte(base + offset, static_cast<int>(slider->getValue()));
        };
        offsets.push_back(offset);
        synthOnly.push_back(wholeColumnSynthOnly || params[i].synthOnly);
    }
}

void D50Editor::ParamColumn::refresh() {
    for (int i = 0; i < sliders.size(); ++i)
        sliders[i]->setValue(processor.getPatchByte(base + offsets[static_cast<size_t>(i)]),
                              juce::dontSendNotification);
}

int D50Editor::ParamColumn::preferredHeight() const { return 22 + sliders.size() * 20; }

void D50Editor::ParamColumn::resized() {
    auto area = getLocalBounds();
    titleLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(2);
    for (int i = 0; i < sliders.size(); ++i) {
        auto row = area.removeFromTop(20);
        paramLabels[i]->setBounds(row.removeFromLeft(juce::roundToInt(row.getWidth() * 0.56f)));
        sliders[i]->setBounds(row);
    }
}

void D50Editor::LcdReadout::setLines(const juce::String &l1, const juce::String &l2) {
    if (line1 == l1 && line2 == l2) return;
    line1 = l1;
    line2 = l2;
    repaint();
}

void D50Editor::LcdReadout::paint(juce::Graphics &g) {
    // The real D-110's own LCD colours (see D110SequencerRetroPanel.cpp's
    // kLcdGlass/kLcdInk) - a cosmetic nod to the same LA-synth era, not a
    // claim about what the real D-50's own display looked like.
    const juce::Colour glass(0xff6ab81f);
    const juce::Colour ink(0xff05230a);
    g.setColour(ink);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    auto area = getLocalBounds().toFloat().reduced(10.0f, 6.0f);
    const float charPx = juce::jmax(8.0f, area.getHeight() * 0.4f);
    g.setColour(glass);
    dotmatrix::drawDotText(g, line1, area.removeFromTop(area.getHeight() * 0.5f), juce::Justification::centredLeft,
                            charPx);
    dotmatrix::drawDotText(g, line2, area, juce::Justification::centredLeft, charPx);
}

void D50Editor::FoldHandle::paint(juce::Graphics &g) {
    const auto &pal = d110ui::palette();
    const auto band = getLocalBounds().toFloat();
    g.setColour(pal.handleBg);
    g.fillRect(band);
    g.setColour(pal.handleBar);
    g.fillRect(band.reduced(0.0f, band.getHeight() * 0.28f));

    // Chevron points away from the block's current resting edge - down when it's open (there's
    // more to reveal below), up when folded - same convention PluginEditor.cpp's own drawer
    // handles use.
    const float cx = band.getCentreX() - 60.0f;
    const float cy = band.getCentreY();
    const float a = juce::jmax(3.0f, band.getHeight() * 0.22f);
    const float dir = folded ? 1.0f : -1.0f;
    juce::Path chevron;
    chevron.startNewSubPath(cx - a * 1.6f, cy - a * 0.5f * dir);
    chevron.lineTo(cx, cy + a * 0.5f * dir);
    chevron.lineTo(cx + a * 1.6f, cy - a * 0.5f * dir);
    g.setColour(pal.handleChevron);
    g.strokePath(chevron, juce::PathStrokeType(juce::jmax(1.5f, a * 0.35f)));

    g.setFont(juce::FontOptions(juce::jlimit(9.0f, 13.0f, band.getHeight() * 0.55f)));
    g.setColour(pal.handleLabel);
    g.drawText(folded ? "PATCH EDIT (click to reopen)" : "PATCH EDIT", band, juce::Justification::centred);
}

void D50Editor::FoldHandle::mouseDown(const juce::MouseEvent &) {
    folded = !folded;
    repaint();
    if (onToggle) onToggle();
}

D50Editor::D50Editor(D50AudioProcessor &p)
    : juce::AudioProcessorEditor(p),
      processor(p),
      partial1(p, kUpperP1Base, "Partial 1"),
      partial2(p, kUpperP2Base, "Partial 2"),
      keyboard(p),
      sequencerPanel(p) {
    // Synced here, not just read lazily, so a project loaded with the big font size looks
    // right from the very first paint - see UiTheme.h's own d110ui::FontScale comment on why
    // the Desktop-global call is Standalone-only.
    d110ui::setFontScale(p.getUiFontScaleBig() ? d110ui::FontScale::Big : d110ui::FontScale::Normal);
#if JucePlugin_Build_Standalone
    juce::Desktop::getInstance().setGlobalScaleFactor(p.getUiFontScaleBig() ? d110ui::kBigScaleFactor : 1.0f);
#endif

    titleLabel.setText("D-50 Emulator (early port)", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    addAndMakeVisible(statusLabel);

#if JucePlugin_Build_Standalone
    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.onClick = [] {
        if (auto *holder = juce::StandalonePluginHolder::getInstance()) holder->showAudioSettingsDialog();
    };

    addAndMakeVisible(fontScaleButton);
    fontScaleButton.setButtonText(processor.getUiFontScaleBig() ? "Font: Big" : "Font: Normal");
    fontScaleButton.onClick = [this] {
        const bool big = !processor.getUiFontScaleBig();
        processor.setUiFontScaleBig(big);
        d110ui::setFontScale(big ? d110ui::FontScale::Big : d110ui::FontScale::Normal);
        juce::Desktop::getInstance().setGlobalScaleFactor(big ? d110ui::kBigScaleFactor : 1.0f);
        fontScaleButton.setButtonText(big ? "Font: Big" : "Font: Normal");
    };
#endif

    addAndMakeVisible(patchMenuButton);
    patchMenuButton.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Import SysEx bank...");
        m.addItem(2, "Export current patch...");
        m.addItem(3, "Export whole bank...");
        m.addSeparator();
        // Send-to-real-hardware, mirroring D110AudioProcessor's own MIDI Out
        // picker (Alan, 2026-09-02: wants to A/B the current patch bytes -
        // including live Tone-tab edits - against his real D-50, to tell a
        // SysEx decode bug from a synthesis-engine one). Same id-numbering
        // scheme (500+i for devices) so the pattern stays recognizable.
        const auto outs = D50AudioProcessor::midiOutputs();
        juce::PopupMenu outMenu;
        outMenu.addItem(5, "(none)", true, processor.getMidiOutputId().isEmpty());
        for (int i = 0; i < outs.size(); ++i)
            outMenu.addItem(500 + i, outs[i].name, true, processor.getMidiOutputId() == outs[i].identifier);
        m.addSubMenu("MIDI Out", outMenu);
        m.addItem(4, "Send to real D-50", processor.hasExternalMidiOutput());
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(patchMenuButton), [this, outs](int result) {
            if (result == 1) importSyxBank();
            else if (result == 2) exportCurrentPatch();
            else if (result == 3) exportBank();
            else if (result == 4) processor.sendPatchToExternalMidi();
            else if (result == 5) processor.setMidiOutputDevice({});
            else if (result >= 500 && result - 500 < outs.size())
                processor.setMidiOutputDevice(outs[result - 500].identifier);
        });
    };

    patchBox.setLookAndFeel(&patchBoxLnf);
    addAndMakeVisible(patchBox);
    patchBox.onChange = [this] {
        const int id = patchBox.getSelectedId();
        if (id > 0) processor.setPatch(id - 1);
    };

    addAndMakeVisible(lcd);

    for (auto *l : {&volumeLabel, &reverbLabel, &chorusLabel}) {
        l->setJustificationType(juce::Justification::centred);
        l->setFont(juce::Font(juce::FontOptions(13.0f)));
        addAndMakeVisible(l);
    }
    volumeLabel.setText("Volume", juce::dontSendNotification);
    reverbLabel.setText("Reverb", juce::dontSendNotification);
    chorusLabel.setText("Chorus", juce::dontSendNotification);

    configureKnob(volumeSlider).setValue(processor.getVolumePercent(), juce::dontSendNotification);
    volumeSlider.onValueChange = [this] { processor.setVolumePercent(static_cast<int>(volumeSlider.getValue())); };
    addAndMakeVisible(volumeSlider);

    configureKnob(reverbSlider).setValue(processor.getReverbPercent(), juce::dontSendNotification);
    reverbSlider.onValueChange = [this] { processor.setReverbPercent(static_cast<int>(reverbSlider.getValue())); };
    addAndMakeVisible(reverbSlider);

    configureKnob(chorusSlider).setValue(processor.getChorusPercent(), juce::dontSendNotification);
    chorusSlider.onValueChange = [this] { processor.setChorusPercent(static_cast<int>(chorusSlider.getValue())); };
    addAndMakeVisible(chorusSlider);

    addAndMakeVisible(patchEditFold);
    patchEditFold.onToggle = [this] {
        // setSize() triggers resized() on its own, same as D110AudioProcessorEditor's own
        // drawer resize - see its applySize()'s own comment.
        updatePatchEditFoldVisibility();
        processor.setFolds(patchEditFold.isFolded(), sequencerFold.isFolded());
        setSize(getWidth(), computeContentHeight());
    };

    addAndMakeVisible(sequencerFold);
    sequencerFold.onToggle = [this] {
        updateSequencerFoldVisibility();
        processor.setFolds(patchEditFold.isFolded(), sequencerFold.isFolded());
        setSize(getWidth(), computeContentHeight());
    };

    toneSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    addAndMakeVisible(toneSectionLabel);

    addAndMakeVisible(toneScopeToggle);
    toneScopeToggle.onClick = [this] { setToneScope(!showingLowerTone); };

    for (auto *b : {&lowerP1OnButton, &lowerP2OnButton, &upperP1OnButton, &upperP2OnButton}) {
        b->setClickingTogglesState(true);
        // Lit = sounding (the default), dark = muted - the reverse sense of
        // JUCE's own buttonOnColourId convention, which is why both colours
        // are set explicitly rather than relying on the default look.
        b->setColour(juce::TextButton::buttonOnColourId, juce::Colours::darkgreen);
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey);
        addAndMakeVisible(*b);
    }
    lowerP1OnButton.setToggleState(!processor.getLowerPartial1Muted(), juce::dontSendNotification);
    lowerP2OnButton.setToggleState(!processor.getLowerPartial2Muted(), juce::dontSendNotification);
    upperP1OnButton.setToggleState(!processor.getUpperPartial1Muted(), juce::dontSendNotification);
    upperP2OnButton.setToggleState(!processor.getUpperPartial2Muted(), juce::dontSendNotification);
    lowerP1OnButton.onClick = [this] { processor.setLowerPartial1Muted(!lowerP1OnButton.getToggleState()); };
    lowerP2OnButton.onClick = [this] { processor.setLowerPartial2Muted(!lowerP2OnButton.getToggleState()); };
    upperP1OnButton.onClick = [this] { processor.setUpperPartial1Muted(!upperP1OnButton.getToggleState()); };
    upperP2OnButton.onClick = [this] { processor.setUpperPartial2Muted(!upperP2OnButton.getToggleState()); };

    // TVF ENV DEPTH keyfollow direction. Read once per note by the engine, so
    // a click lands on the next key struck - hold a chord, click, strike
    // again to hear the two side by side. The label names the firmware
    // revision each direction corresponds to, because "on/off" would say
    // nothing about which one is supposed to be right.
    addAndMakeVisible(keyfollowFixButton);
    keyfollowFixButton.setClickingTogglesState(true);
    keyfollowFixButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::darkgreen);
    keyfollowFixButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey);
    auto refreshKeyfollowFixButton = [this] {
        const bool fixed = processor.getTvfKeyfollowFixed();
        keyfollowFixButton.setButtonText(fixed ? "KF v1.07" : "KF v1.06");
        keyfollowFixButton.setTooltip(
            fixed ? "TVF ENV DEPTH keyfollow: the direction ROM 1.07 corrected it to. Click for the "
                    "uncorrected 1.06 direction this engine was disassembled from."
                  : "TVF ENV DEPTH keyfollow: the uncorrected ROM 1.06 direction, verbatim from the "
                    "disassembly. Click for the direction ROM 1.07 corrected it to.");
    };
    keyfollowFixButton.setToggleState(processor.getTvfKeyfollowFixed(), juce::dontSendNotification);
    refreshKeyfollowFixButton();
    keyfollowFixButton.onClick = [this, refreshKeyfollowFixButton] {
        processor.setTvfKeyfollowFixed(keyfollowFixButton.getToggleState());
        refreshKeyfollowFixButton();
    };

    structureLabel.setText("Structure", juce::dontSendNotification);
    addAndMakeVisible(structureLabel);
    for (int i = 1; i <= 7; ++i) structureBox.addItem("Structure " + juce::String(i), i);
    addAndMakeVisible(structureBox);
    structureBox.onChange = [this] {
        processor.setPatchByte(currentCommonBase + kStructureOffset, structureBox.getSelectedId() - 1);
        updatePartialApplicability();
    };

    balanceLabel.setText("Balance", juce::dontSendNotification);
    addAndMakeVisible(balanceLabel);
    balanceSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    balanceSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 45, 22);
    balanceSlider.setRange(0.0, 100.0, 1.0);
    addAndMakeVisible(balanceSlider);
    balanceSlider.onValueChange = [this] {
        processor.setPatchByte(currentCommonBase + kBalanceOffset, static_cast<int>(balanceSlider.getValue()));
    };

    addAndMakeVisible(partial1);
    addAndMakeVisible(partial2);

    // The rest of the patch: full envelopes, LFOs, EQ/chorus detail, patch-
    // common - see this file's own ToneParam tables above. One ParamColumn
    // per group, laid out in a wrapping grid inside a scrolling viewport
    // (layoutToneColumns()) rather than as fixed rows, since there's no
    // window size that fits ~150 sliders unscrolled. Three flavours, so
    // setToneScope() knows which of them to re-target and by how much:
    // per-partial (whole column moves to the other tone's own partial
    // block), tone-common (moves, but each keeps its own fixed offset
    // within the block - the three LFOs share one table at three different
    // offsets), and patch-common (never moves - addColumn() below, not
    // wrapped by either helper).
    auto addColumn = [this](int columnBase, const juce::String &title, const ToneParam *params, int count,
                             bool wholeColumnSynthOnly = false) {
        auto *col = toneColumns.add(new ParamColumn(processor, columnBase, title, params, count, wholeColumnSynthOnly));
        toneViewportContent.addAndMakeVisible(col);
        return col;
    };
    auto addPartial1Column = [&](const juce::String &title, const ToneParam *params, int count,
                                  bool wholeColumnSynthOnly = false) {
        partial1ScopedColumns.push_back(addColumn(kUpperP1Base, title, params, count, wholeColumnSynthOnly));
    };
    auto addPartial2Column = [&](const juce::String &title, const ToneParam *params, int count,
                                  bool wholeColumnSynthOnly = false) {
        partial2ScopedColumns.push_back(addColumn(kUpperP2Base, title, params, count, wholeColumnSynthOnly));
    };
    auto addCommonColumn = [&](int relativeOffset, const juce::String &title, const ToneParam *params, int count) {
        commonScopedColumns.push_back({addColumn(kUpperCommonBase + relativeOffset, title, params, count), relativeOffset});
    };
    addPartial1Column("Partial 1: WG", kWgExtra, static_cast<int>(std::size(kWgExtra)));
    // Whole column, not per-row: a PCM partial has no TVF stage at all
    // (see d5_voice.h's Voice::next()), so none of kTvfFull's ~19 rows
    // apply, not just some of them.
    addPartial1Column("Partial 1: TVF", kTvfFull, static_cast<int>(std::size(kTvfFull)), true);
    addPartial1Column("Partial 1: TVA", kTvaFull, static_cast<int>(std::size(kTvaFull)));
    addPartial2Column("Partial 2: WG", kWgExtra, static_cast<int>(std::size(kWgExtra)));
    addPartial2Column("Partial 2: TVF", kTvfFull, static_cast<int>(std::size(kTvfFull)), true);
    addPartial2Column("Partial 2: TVA", kTvaFull, static_cast<int>(std::size(kTvaFull)));
    addCommonColumn(0, "Pitch Envelope", kPenv, static_cast<int>(std::size(kPenv)));
    addCommonColumn(25, "LFO 1", kLfo, static_cast<int>(std::size(kLfo)));
    addCommonColumn(29, "LFO 2", kLfo, static_cast<int>(std::size(kLfo)));
    addCommonColumn(33, "LFO 3", kLfo, static_cast<int>(std::size(kLfo)));
    addCommonColumn(0, "EQ / Chorus", kEqChorus, static_cast<int>(std::size(kEqChorus)));
    addColumn(kPatchBase, "Patch Common", kPatchCommon, static_cast<int>(std::size(kPatchCommon)));
    toneViewport.setViewedComponent(&toneViewportContent, false);
    addAndMakeVisible(toneViewport);
    setToneScope(false);

    addAndMakeVisible(keyboard);

    addAndMakeVisible(sequencerPanel);
    // Folded or not as left last time (sequencer folded, patch edit open, on a first launch).
    patchEditFold.setFolded(processor.getPatchEditFolded());
    sequencerFold.setFolded(processor.getSequencerFolded());
    updatePatchEditFoldVisibility();
    updateSequencerFoldVisibility();

    setResizable(true, true);
    // Minimum is whatever's needed with both FoldHandles folded (title, status, patch box,
    // knobs, both fold handles, keyboard - see computeContentHeight()); maximum is generous
    // enough for both unfolded at once plus some resize headroom for the scrolling grid.
    setResizeLimits(620, 480, 1400, 1600);
    setSize(760, computeContentHeight());
    startTimerHz(4);
    timerCallback();
}

D50Editor::~D50Editor() {
    stopTimer();
    patchBox.setLookAndFeel(nullptr);  // patchBoxLnf is about to be destroyed with us
}

void D50Editor::importSyxBank() {
    fileChooser = std::make_unique<juce::FileChooser>("Import a D-50 SysEx bank", juce::File(), "*.syx");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser &fc) {
                                  const auto file = fc.getResult();
                                  if (file == juce::File()) return;
                                  const auto message = processor.importSyxBank(file);
                                  refreshPatchList();
                                  statusLabel.setText(message, juce::dontSendNotification);
                              });
}

void D50Editor::exportCurrentPatch() {
    const auto suggested = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                .getChildFile(processor.patchNameAt(processor.currentPatch()).isNotEmpty()
                                                  ? processor.patchNameAt(processor.currentPatch())
                                                  : juce::String("D-50 Patch"))
                                .withFileExtension("syx");
    fileChooser = std::make_unique<juce::FileChooser>("Export the current patch", suggested, "*.syx");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser &fc) {
                                  const auto file = fc.getResult();
                                  if (file == juce::File()) return;
                                  const bool ok = processor.exportCurrentPatch(file);
                                  statusLabel.setText(ok ? ("Exported " + file.getFileName())
                                                          : juce::String("Export failed"),
                                                       juce::dontSendNotification);
                              });
}

void D50Editor::exportBank() {
    const auto suggested =
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("D-50 Bank.syx");
    fileChooser = std::make_unique<juce::FileChooser>("Export the whole bank", suggested, "*.syx");
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser &fc) {
                                  const auto file = fc.getResult();
                                  if (file == juce::File()) return;
                                  const bool ok = processor.exportBank(file);
                                  statusLabel.setText(
                                      ok ? ("Exported " + juce::String(processor.patchCount()) + " patches to " +
                                            file.getFileName())
                                         : juce::String("Export failed"),
                                      juce::dontSendNotification);
                              });
}

void D50Editor::parentHierarchyChanged() {
    // The OS's own window chrome, not JUCE's custom-drawn one - Alan's
    // explicit request (2026-09-01), see this class's own header comment.
    // Only meaningful once actually attached to a real top-level window, so
    // this is a no-op (and stays one) until then, and a permanent no-op
    // wherever the host's own window isn't a juce::TopLevelWindow at all.
    //
    // Deferred to the next message-loop tick, not done here directly: this
    // callback fires the instant the editor is parented, which for the
    // Standalone window is BEFORE its own "resize to fit the content"
    // handling has run - toggling the title bar style at that point recreates
    // the native window peer at whatever size the window happened to have a
    // moment before (observed: setResizeLimits()'s minimum), and the content
    // is then stuck there even once the window's own logic tries to resize
    // it afterwards. Waiting one tick lets that settle first.
    if (nativeTitleBarApplied) return;
    if (dynamic_cast<juce::TopLevelWindow *>(getTopLevelComponent()) == nullptr) return;
    nativeTitleBarApplied = true;
    juce::Component::SafePointer<D50Editor> self(this);
    juce::MessageManager::callAsync([self] {
        if (self == nullptr) return;
        auto *tlw = dynamic_cast<juce::TopLevelWindow *>(self->getTopLevelComponent());
        if (tlw == nullptr) return;
        const auto settledBounds = tlw->getBounds();
        tlw->setUsingNativeTitleBar(true);
        if (!settledBounds.isEmpty()) tlw->setBounds(settledBounds);
    });
}

void D50Editor::setToneScope(bool lower) {
    showingLowerTone = lower;
    const int p1Base = lower ? kLowerP1Base : kUpperP1Base;
    const int p2Base = lower ? kLowerP2Base : kUpperP2Base;
    currentCommonBase = lower ? kLowerCommonBase : kUpperCommonBase;

    partial1.setBase(p1Base);
    partial2.setBase(p2Base);
    // Re-point the debug PCM pitch accessors at whichever of the four
    // upper/lower x P1/P2 slots this panel now shows - see PartialPanel::
    // setDebugPitchAccessors()'s own comment.
    partial1.setDebugPitchAccessors(
        [this, lower] { return lower ? processor.getLowerPartial1PitchOffset() : processor.getUpperPartial1PitchOffset(); },
        [this, lower](int v) { if (lower) processor.setLowerPartial1PitchOffset(v); else processor.setUpperPartial1PitchOffset(v); });
    partial2.setDebugPitchAccessors(
        [this, lower] { return lower ? processor.getLowerPartial2PitchOffset() : processor.getUpperPartial2PitchOffset(); },
        [this, lower](int v) { if (lower) processor.setLowerPartial2PitchOffset(v); else processor.setUpperPartial2PitchOffset(v); });
    for (auto *col : partial1ScopedColumns) col->setBase(p1Base);
    for (auto *col : partial2ScopedColumns) col->setBase(p2Base);
    for (auto &binding : commonScopedColumns) binding.first->setBase(currentCommonBase + binding.second);

    toneScopeToggle.setButtonText(lower ? "Switch to Upper" : "Switch to Lower");
    toneSectionLabel.setText((lower ? juce::String("Tone (lower): ") : juce::String("Tone (upper): ")) +
                                  decodeToneName(processor, currentCommonBase),
                              juce::dontSendNotification);

    if (processor.isReady()) {
        structureBox.setSelectedId(processor.getPatchByte(currentCommonBase + kStructureOffset) + 1,
                                    juce::dontSendNotification);
        balanceSlider.setValue(processor.getPatchByte(currentCommonBase + kBalanceOffset), juce::dontSendNotification);
    }
    updatePartialApplicability();
}

void D50Editor::updatePartialApplicability() {
    if (!processor.isReady()) return;
    const int raw = processor.getPatchByte(currentCommonBase + kStructureOffset);
    // Same clamp as Voice::structure() (d5_voice.h) - an out-of-range byte
    // (e.g. no patch loaded yet) falls back to Structure 1, both partials
    // Synth, rather than reading past the table.
    const int idx = (raw < 0 || raw > 6) ? 0 : raw;
    const bool p1Pcm = d5::kStructures[idx].p1 == d5::PartialType::kPcm;
    const bool p2Pcm = d5::kStructures[idx].p2 == d5::PartialType::kPcm;
    partial1.setPcmActive(p1Pcm);
    partial2.setPcmActive(p2Pcm);
    for (auto *col : partial1ScopedColumns) col->setPartialIsPcm(p1Pcm);
    for (auto *col : partial2ScopedColumns) col->setPartialIsPcm(p2Pcm);
}

void D50Editor::updatePatchEditFoldVisibility() {
    const bool visible = !patchEditFold.isFolded();
    for (auto *c : {static_cast<juce::Component *>(&toneScopeToggle), static_cast<juce::Component *>(&keyfollowFixButton),
                    static_cast<juce::Component *>(&lowerP1OnButton), static_cast<juce::Component *>(&lowerP2OnButton),
                    static_cast<juce::Component *>(&upperP1OnButton), static_cast<juce::Component *>(&upperP2OnButton),
                    static_cast<juce::Component *>(&toneSectionLabel), static_cast<juce::Component *>(&structureLabel),
                    static_cast<juce::Component *>(&structureBox), static_cast<juce::Component *>(&balanceLabel),
                    static_cast<juce::Component *>(&balanceSlider), static_cast<juce::Component *>(&partial1),
                    static_cast<juce::Component *>(&partial2), static_cast<juce::Component *>(&toneViewport)})
        c->setVisible(visible);
}

void D50Editor::updateSequencerFoldVisibility() { sequencerPanel.setVisible(!sequencerFold.isFolded()); }

// Fixed default the scrolling ParamColumn grid gets whenever patchEditFold is open - see this
// method's own declaration in D50Editor.h for the trade-off (a user's own resize of that area
// doesn't survive a fold/unfold round trip).
namespace {
constexpr int kToneViewportDefaultH = 220;
}

int D50Editor::computeContentHeight() const {
    // Mirrors resized()'s own reservations exactly, top to bottom: margins(12+12) + title(26)
    // + status(20) + gap(8) + patchRow(28) + gap(8) + lcd(46) + gap(16) + knobs(110).
    int h = 24 + 26 + 20 + 8 + 28 + 8 + 46 + 16 + 110;
    h += 20;  // patchEditFold's own handle
    if (!patchEditFold.isFolded()) {
        // gap(6) + toneHeaderRow(20) + structRow(28) + gap(10) + compactRow(204) + gap(10)
        // + the scrolling grid's own default height.
        h += 6 + 20 + 28 + 10 + 204 + 10 + kToneViewportDefaultH;
    }
    h += 10;                                          // gap before the keyboard
    h += static_cast<int>(D110Keyboard::kRefH);        // keyboard
    h += 10;                                           // gap before sequencerFold
    h += 20;                                           // sequencerFold's own handle
    if (!sequencerFold.isFolded()) {
        h += 6 + static_cast<int>(D110SequencerPanel::kRefH);
    }
    return h;
}

void D50Editor::refreshPatchList() {
    patchBox.clear(juce::dontSendNotification);
    if (!processor.isReady()) {
        patchBox.setTextWhenNothingSelected("(no patches)");
        return;
    }
    const int n = processor.patchCount();
    for (int i = 0; i < n; ++i) patchBox.addItem(juce::String(i + 1) + ". " + processor.patchNameAt(i), i + 1);
    patchBox.setSelectedId(processor.currentPatch() + 1, juce::dontSendNotification);
}

void D50Editor::timerCallback() {
    statusLabel.setText(processor.statusMessage(), juce::dontSendNotification);
    if (processor.isReady() != wasReady) {
        wasReady = processor.isReady();
        refreshPatchList();
    }
    // The bridge advances patch() on a bare Program Change too (no round trip
    // through setPatch()), so the combo box has to be kept in sync from here
    // rather than only from its own onChange.
    if (processor.isReady() && patchBox.getSelectedId() != processor.currentPatch() + 1)
        patchBox.setSelectedId(processor.currentPatch() + 1, juce::dontSendNotification);

    if (processor.isReady()) {
        structureBox.setSelectedId(processor.getPatchByte(currentCommonBase + kStructureOffset) + 1,
                                    juce::dontSendNotification);
        balanceSlider.setValue(processor.getPatchByte(currentCommonBase + kBalanceOffset), juce::dontSendNotification);
        partial1.refresh();
        partial2.refresh();
        for (auto *col : toneColumns) col->refresh();
        // Catches Structure changes from anywhere that isn't structureBox's
        // own onChange or setToneScope() - a patch switch, a bank import, a
        // SysEx write from elsewhere.
        updatePartialApplicability();
        toneSectionLabel.setText((showingLowerTone ? juce::String("Tone (lower): ") : juce::String("Tone (upper): ")) +
                                      decodeToneName(processor, currentCommonBase),
                                  juce::dontSendNotification);

        const int idx = processor.currentPatch();
        // Same "U: <name>   L: <name>" line the real D-50's own LCD shows
        // (confirmed against a real unit, Alan, 2026-09-02 - see
        // decodeToneName()'s own comment) - both tone names always, not just
        // the one currently being edited above.
        lcd.setLines(juce::String(idx / 8 + 1) + "-" + juce::String(idx % 8 + 1).paddedLeft('0', 1) + " " +
                         processor.patchNameAt(idx),
                     "U:" + decodeToneName(processor, kUpperCommonBase) + " L:" +
                         decodeToneName(processor, kLowerCommonBase));
    } else {
        lcd.setLines("D-50", "NO PATCH LOADED");
    }
}

void D50Editor::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff2a2a2e));
}

void D50Editor::layoutToneColumns() {
    const int colW = 190;
    const int gap = 10;
    const int viewportWidth = toneViewport.getMaximumVisibleWidth();
    const int perRow = juce::jmax(1, (viewportWidth + gap) / (colW + gap));

    int x = 0, y = 0, rowMaxH = 0, count = 0;
    for (auto *col : toneColumns) {
        const int h = col->preferredHeight();
        col->setBounds(x, y, colW, h);
        rowMaxH = juce::jmax(rowMaxH, h);
        x += colW + gap;
        ++count;
        if (count % perRow == 0) {
            x = 0;
            y += rowMaxH + gap;
            rowMaxH = 0;
        }
    }
    if (count % perRow != 0) y += rowMaxH + gap;
    toneViewportContent.setSize(perRow * colW + (perRow - 1) * gap, y);
}

void D50Editor::resized() {
    auto area = getLocalBounds().reduced(12);
    auto titleRow = area.removeFromTop(26);
#if JucePlugin_Build_Standalone
    audioSettingsButton.setBounds(titleRow.removeFromRight(160));
    titleRow.removeFromRight(8);
    fontScaleButton.setBounds(titleRow.removeFromRight(100));
    titleRow.removeFromRight(8);
#endif
    titleLabel.setBounds(titleRow);
    statusLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);
    auto patchRow = area.removeFromTop(28);
    patchMenuButton.setBounds(patchRow.removeFromRight(90));
    patchRow.removeFromRight(8);
    patchBox.setBounds(patchRow);
    area.removeFromTop(8);
    lcd.setBounds(area.removeFromTop(46));
    area.removeFromTop(16);

    auto knobs = area.removeFromTop(110);
    const int w = knobs.getWidth() / 3;
    auto layout = [&](juce::Rectangle<int> slot, juce::Label &label, juce::Slider &slider) {
        label.setBounds(slot.removeFromTop(18));
        slider.setBounds(slot);
    };
    layout(knobs.removeFromLeft(w), volumeLabel, volumeSlider);
    layout(knobs.removeFromLeft(w), reverbLabel, reverbSlider);
    layout(knobs, chorusLabel, chorusSlider);

    // Sequencer drawer, bottom-most - same stacking order the D-110 plugin uses (EditorPane,
    // Keyboard, Sequencer). Reserved from the bottom BEFORE the keyboard area below, so it
    // ends up under the keyboard rather than under the patch-edit block.
    if (!sequencerFold.isFolded()) {
        auto seqArea = area.removeFromBottom(static_cast<int>(D110SequencerPanel::kRefH));
        sequencerPanel.setBounds(seqArea);
        area.removeFromBottom(6);
    }
    sequencerFold.setBounds(area.removeFromBottom(20));
    area.removeFromBottom(10);

    auto keyboardArea = area.removeFromBottom(static_cast<int>(D110Keyboard::kRefH));
    area.removeFromBottom(10);

    patchEditFold.setBounds(area.removeFromTop(20));

    if (!patchEditFold.isFolded()) {
        area.removeFromTop(6);
        auto toneHeaderRow = area.removeFromTop(20);
        toneScopeToggle.setBounds(toneHeaderRow.removeFromRight(140));
        toneHeaderRow.removeFromRight(8);
        keyfollowFixButton.setBounds(toneHeaderRow.removeFromRight(76));
        toneHeaderRow.removeFromRight(10);
        upperP2OnButton.setBounds(toneHeaderRow.removeFromRight(50));
        toneHeaderRow.removeFromRight(4);
        upperP1OnButton.setBounds(toneHeaderRow.removeFromRight(50));
        toneHeaderRow.removeFromRight(6);
        lowerP2OnButton.setBounds(toneHeaderRow.removeFromRight(50));
        toneHeaderRow.removeFromRight(4);
        lowerP1OnButton.setBounds(toneHeaderRow.removeFromRight(50));
        toneHeaderRow.removeFromRight(8);
        toneSectionLabel.setBounds(toneHeaderRow);
        auto structRow = area.removeFromTop(28);
        structureLabel.setBounds(structRow.removeFromLeft(70));
        structureBox.setBounds(structRow.removeFromLeft(130));
        structRow.removeFromLeft(16);
        balanceLabel.setBounds(structRow.removeFromLeft(60));
        balanceSlider.setBounds(structRow);
        area.removeFromTop(10);

        const int half = area.getWidth() / 2;
        // PartialPanel needs title(20) + 4px gap + 6 rows*(26+4) = 204.
        auto compactRow = area.removeFromTop(204);
        partial1.setBounds(compactRow.removeFromLeft(half).withTrimmedRight(8));
        partial2.setBounds(compactRow.withTrimmedLeft(8));
        area.removeFromTop(10);

        // Everything else, scrolling - whatever height/width is left, which is
        // the part that actually grows when the window is resized.
        toneViewport.setBounds(area);
        layoutToneColumns();
    }

    keyboard.setBounds(keyboardArea);
}
