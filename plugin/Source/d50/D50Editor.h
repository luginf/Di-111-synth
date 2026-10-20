#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <utility>
#include <vector>

#include "D50AudioProcessor.h"
#include "../D110Keyboard.h"
#include "../sequencer/D110SequencerPanel.h"

// D-50 editor: patch picker, the panel's own three balance knobs (volume/
// reverb/chorus), a first slice of real sound-parameter editing (structure
// and each partial's waveform/PCM source/filter - see PartialPanel below),
// and the shared on-screen test keyboard so this is playable without a DAW
// or external controller. Not a front-panel recreation like D110Panel -
// there is no real firmware/panel behind this engine to photograph or
// measure (see CLAUDE.md's D-50 section), so this is plain JUCE widgets.
//
// Uses the OS's own native window chrome (see parentHierarchyChanged()) and
// is genuinely resizable - Alan's explicit request, 2026-09-01: JUCE's own
// custom-drawn title bar wouldn't let the Standalone window drag past the
// screen edge (see project memory project_window_offscreen_drag_
// investigation - a native title bar is one of the two mechanisms it
// flagged as "not yet checked", now resolved by just using one).
class D50Editor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit D50Editor(D50AudioProcessor &p);
    ~D50Editor() override;

    void paint(juce::Graphics &g) override;
    void resized() override;
    bool sizeTracking = false; // set once the constructor has chosen the initial size
    int sizeSaveToken = 0;
    // Only reachable once this editor is actually attached to a top-level
    // window (see this file's own header comment) - a Standalone run gets
    // JUCE's own DocumentWindow, so this applies; a future VST3 build's host
    // window may or may not be a juce::TopLevelWindow at all, in which case
    // the dynamic_cast below is simply null and this is a no-op there.
    void parentHierarchyChanged() override;

    // {display name, byte offset, maximum value} - same idiom as D110EditorPane's
    // own ToneParam (PluginEditor.h), one generic row per entry rather than a
    // hand-written widget per parameter. Offsets/ranges are relative to
    // whatever base a ParamColumn instance is given (a partial's own 64-byte
    // block, the tone-common block, or the patch block) - see the .cpp's own
    // parameter tables for where each one comes from in d5_patch_map.h. Public
    // only so the .cpp's file-scope tables can name the type; never used
    // outside this class.
    struct ToneParam {
        const char *name;
        int offset;
        int hi;
        // Grayed out when this partial's Structure-determined type doesn't
        // read this byte at all - a pulse-width row on a partial currently
        // playing a PCM sample, say. See D50Editor::updatePartialApplicability()
        // and d5_voice.h's own kStructures/PartialType (Alan, 2026-09-02:
        // asked for this after confirming Pipe Solo's octave bug is PCM-
        // pitch-specific, not structure-wide - localizing which controls can
        // possibly be at fault helps narrow the rest down).
        bool synthOnly = false;
    };

private:
    void timerCallback() override;
    void refreshPatchList();
    void layoutToneColumns();
    void importSyxBank();
    void exportCurrentPatch();
    void exportBank();
    // Reads the currently-shown tone's Structure byte and grays out whatever
    // each partial's resulting type (PCM or Synth, d5_voice.h's kStructures)
    // doesn't use - see PartialPanel::setPcmActive()/ParamColumn::
    // setPartialIsPcm(). Called after anything that can change which tone is
    // shown or what its Structure is: setToneScope(), structureBox.onChange,
    // and every timerCallback() tick (patch switches/imports/SysEx writes
    // change the byte without going through either of the first two).
    void updatePartialApplicability();
    // FoldHandle::onToggle's target - shows/hides every component the fold covers (see
    // FoldHandle's own comment) to match its current state. Called once from the constructor's
    // wiring and again on every click.
    void updatePatchEditFoldVisibility();
    void updateSequencerFoldVisibility();
    // What the window's total height should be for the CURRENT state of both FoldHandles -
    // every toggle's onToggle calls setSize(getWidth(), computeContentHeight()) with this,
    // rather than measuring/replaying a delta off whatever the layout happened to be a moment
    // ago (fragile once two independent folds can each change size on their own). The
    // scrolling ParamColumn grid's own height (when patchEditFold is open) is given a fixed
    // reasonable default here, same as its initial constructed size - a user who resizes the
    // window bigger to see more of it keeps that height until the NEXT fold toggle, which
    // resets it back to this default (a known, minor trade-off for not tracking that
    // separately - see FoldHandle's own comment on why this whole approach was chosen).
    int computeContentHeight() const;
    // Re-targets partial1/partial2 and every tone-scoped ParamColumn (WG/TVF/
    // TVA per partial, P-ENV, the three LFOs, EQ/Chorus) at the lower tone's
    // blocks instead of the upper tone's, or back - the D-50 has two of these
    // (Alan, 2026-09-02: confirming it plays both at once in Dual/Split key
    // mode), and this toggles which one the Tone section is currently
    // editing rather than showing both at once and doubling the section's
    // already-substantial size. Patch Common (key mode, split point, etc.)
    // is genuinely patch-level, not tone-level, so it never moves.
    void setToneScope(bool lower);

    // A cosmetic lookalike of a D-50-era alphanumeric LCD - patch number/name
    // and structure, in the shared dot-matrix font (see ../DotMatrixFont.h).
    // Not a real display: there is no real firmware behind this engine to
    // render an authentic one (see this class's own CLAUDE.md reference) -
    // this is only for the same reason D110SequencerRetroPanel's screen is,
    // to look the part.
    class LcdReadout : public juce::Component {
    public:
        void setLines(const juce::String &line1, const juce::String &line2);
        void paint(juce::Graphics &g) override;

    private:
        juce::String line1, line2;
    };

    // Folds a whole block of the editor out of the way AND shrinks the window to match, so
    // whatever sits below rides up rather than leaving blank space - Alan's request,
    // 2026-09-03, "comme pour le D110": the extended editor drawer there (D110EditorPane, see
    // PluginEditor.h) does the same. That one animates the resize and can be dragged too; this
    // is the plain-click version - a click just flips visible/hidden and jumps straight to the
    // new size (see D50Editor::computeContentHeight(), which every toggle calls into via
    // setSize()) with no animation or drag-resize. Two independent instances -
    // D50Editor::patchEditFold (Structure/Balance/PartialPanels/the scrolling ParamColumn
    // grid) and ::sequencerFold (the D110SequencerPanel drawer) - hence the constructor-time
    // label rather than a hardcoded one. Their open/closed state is saved with the plugin state
    // (format 8, D50AudioProcessor::setFolds()), like D110AudioProcessorEditor's drawers; defaults
    // on a first launch are patchEditFold open, sequencerFold closed - see D50Editor's constructor.
    class FoldHandle : public juce::Component {
    public:
        explicit FoldHandle(juce::String label) : label_(std::move(label)) {}
        void paint(juce::Graphics &g) override;
        void mouseDown(const juce::MouseEvent &) override;
        void setFolded(bool f) {
            folded = f;
            repaint();
        }
        bool isFolded() const { return folded; }
        std::function<void()> onToggle;

    private:
        juce::String label_;
        bool folded = false;
    };

    // One partial's sound-shaping controls: WG Waveform, PCM wave number,
    // TVF cutoff/resonance, pulse width. `blockBase` is that partial's own
    // 64-byte block's absolute offset into the 448-byte patch (see
    // d50/d5_engine/d5_patch_map.h's PatchBlock enum - kBlkUpperP1/P2 times
    // 64) - the byte offsets used here are relative to it, straight from
    // that file's own map_partial() comments.
    class PartialPanel : public juce::Component {
    public:
        PartialPanel(D50AudioProcessor &proc, int blockBase, const juce::String &title);
        void resized() override;
        void refresh();
        // Re-targets this panel at a different 64-byte block - the Upper/Lower
        // toggle (see D50Editor::setToneScope()) moves the SAME two
        // PartialPanel instances between the upper and lower tone's blocks
        // rather than building a second pair, so the compact view doesn't
        // double in size for a patch that hardly ever needs both at once.
        void setBase(int newBase) {
            base = newBase;
            refresh();
        }

        // Debug PCM pitch offset (-48..+48 semitones, see D50AudioProcessor::
        // setUpperPartial1PitchOffset()'s own comment) - this panel is reused
        // for all four upper/lower x P1/P2 combinations via setBase(), so it
        // doesn't own that state itself; D50Editor::setToneScope() re-points
        // these accessors at whichever of the four the panel currently shows,
        // the same way it re-points setBase().
        void setDebugPitchAccessors(std::function<int()> getter, std::function<void(int)> setter) {
            getDebugPitch = std::move(getter);
            setDebugPitch = std::move(setter);
            refresh();
        }

        // Grays out whichever half of this panel the current Structure
        // doesn't actually use for this partial - Waveform/Cutoff/Resonance/
        // Pulse Width when it's playing a PCM sample (no WG waveform or TVF
        // at all, see d5_voice.h's Voice::next()), or PCM Wave/PCM Pitch
        // (debug) when it's a synth waveform partial instead. Driven by
        // D50Editor::updatePartialApplicability() from the tone-common
        // Structure byte, not owned here (same reasoning as setBase()/
        // setDebugPitchAccessors() - this panel doesn't know which of the
        // four upper/lower x P1/P2 slots it currently shows).
        void setPcmActive(bool isPcm) {
            for (auto *c : {static_cast<juce::Component *>(&waveformBox), static_cast<juce::Component *>(&cutoffSlider),
                            static_cast<juce::Component *>(&resonanceSlider),
                            static_cast<juce::Component *>(&pulseWidthSlider)}) {
                c->setEnabled(!isPcm);
                c->setAlpha(!isPcm ? 1.0f : 0.4f);
            }
            for (auto *l : {&waveformLabel, &cutoffLabel, &resonanceLabel, &pulseWidthLabel})
                l->setAlpha(!isPcm ? 1.0f : 0.4f);
            for (auto *c : {static_cast<juce::Component *>(&pcmSlider), static_cast<juce::Component *>(&debugPitchSlider)}) {
                c->setEnabled(isPcm);
                c->setAlpha(isPcm ? 1.0f : 0.4f);
            }
            for (auto *l : {&pcmLabel, &debugPitchLabel}) l->setAlpha(isPcm ? 1.0f : 0.4f);
        }

    private:
        D50AudioProcessor &processor;
        int base;
        juce::Label titleLabel;
        juce::Label waveformLabel, pcmLabel, cutoffLabel, resonanceLabel, pulseWidthLabel, debugPitchLabel;
        juce::ComboBox waveformBox;
        juce::Slider pcmSlider, cutoffSlider, resonanceSlider, pulseWidthSlider, debugPitchSlider;
        std::function<int()> getDebugPitch;
        std::function<void(int)> setDebugPitch;
    };

    // One titled group of raw-byte sliders, built from a ToneParam table -
    // the generic building block every group below (WG/TVF/TVA per partial,
    // P-ENV, the three LFOs, EQ/Chorus, patch-common) is made of, so adding
    // another group later is a new table, not new layout code.
    class ParamColumn : public juce::Component {
    public:
        // wholeColumnSynthOnly marks every row synthOnly regardless of what
        // the table says - for the TVF columns, where that's true of the
        // whole thing (a PCM partial has no filter stage at all), without
        // having to repeat `true` on all ~19 rows of kTvfFull.
        ParamColumn(D50AudioProcessor &proc, int base, const juce::String &title, const ToneParam *params,
                    int count, bool wholeColumnSynthOnly = false);
        void resized() override;
        void refresh();
        int preferredHeight() const;
        // See PartialPanel::setBase()'s own comment - same idea, for the
        // scrolling per-partial/tone-common groups below.
        void setBase(int newBase) {
            base = newBase;
            refresh();
        }

        // See PartialPanel::setPcmActive()'s own comment - same idea, for
        // whichever rows of this column are synthOnly.
        void setPartialIsPcm(bool isPcm) {
            for (int i = 0; i < sliders.size(); ++i) {
                const bool disabled = isPcm && synthOnly[static_cast<size_t>(i)];
                sliders[i]->setEnabled(!disabled);
                sliders[i]->setAlpha(disabled ? 0.4f : 1.0f);
                paramLabels[i]->setAlpha(disabled ? 0.4f : 1.0f);
            }
        }

    private:
        D50AudioProcessor &processor;
        int base;
        juce::Label titleLabel;
        juce::OwnedArray<juce::Label> paramLabels;
        juce::OwnedArray<juce::Slider> sliders;
        std::vector<int> offsets;
        std::vector<bool> synthOnly;
    };

    D50AudioProcessor &processor;

    juce::Label titleLabel, statusLabel;
    // Standalone only (see the .cpp): the native title bar means JUCE's own
    // StandaloneFilterWindow "Options" button - which lived IN that
    // custom-drawn title bar - no longer has anywhere to sit
    // (getTitleBarHeight() goes to 0), so this replaces it for Audio/MIDI
    // Settings access. Always constructed (a plain TextButton costs nothing
    // unused) but only wired up and shown for JucePlugin_Build_Standalone.
    juce::TextButton audioSettingsButton{"Audio/MIDI Settings..."};
    // Normal/Big text and control size - see UiTheme.h's own d110ui::FontScale comment.
    // Standalone only, same reasoning as audioSettingsButton just above: the Desktop-global
    // scale call this drives would resize the DAW itself from inside a shared VST3/AU process.
    juce::TextButton fontScaleButton{"Font: Normal"};
    // Import a whole bank (any real D-50 SysEx bulk dump) or export just the
    // currently-sounding patch, with whatever edits the Tone section above
    // made to it, as a single-patch SysEx file someone else's D-50 - or this
    // app, later - can load back in. See importSyxBank()/exportCurrentPatch().
    juce::TextButton patchMenuButton{"Patch..."};
    std::unique_ptr<juce::FileChooser> fileChooser;
    // stock juce::LookAndFeel_V2::getOptionsForComboBoxPopupMenu() pins every ComboBox popup to
    // a single column (PopupMenu::Options::withMaximumNumColumns(1)), so a multi-bank patch list
    // (up to 384 entries, see D50AudioProcessor::importSyxBank()) opens as one long scrolling
    // column - Alan's request, 2026-09-03: let it wrap into several side by side instead, the
    // way PopupMenu already knows how to when there's room. Scoped to patchBox alone (setLook
    // AndFeel() on the component, not the app-wide default) so no other combo box - all a
    // handful of items - is affected.
    //
    // Also overrides withMinimumWidth() (2026-09-03 follow-up, Alan: widening the window past a
    // certain point collapsed it back to one column) - the stock implementation forces that
    // minimum to box.getWidth(), which grows with the editor window; juce_PopupMenu.cpp's own
    // column-fitting loop (insertColumnBreaks()) bails out at 1 column as soon as that forced
    // minimum exceeds roughly half the screen's usable width, regardless of
    // withMaximumNumColumns(). Capping it to a small fixed value instead keeps the popup's own
    // natural (content-driven) width in charge of the column count, independent of how wide the
    // combo box itself happens to be.
    class WideComboLnf : public juce::LookAndFeel_V4 {
        juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox &box, juce::Label &label) override {
            return juce::LookAndFeel_V2::getOptionsForComboBoxPopupMenu(box, label)
                .withMaximumNumColumns(4)
                .withMinimumWidth(juce::jmin(box.getWidth(), 260));
        }
    };
    WideComboLnf patchBoxLnf;
    juce::ComboBox patchBox;
    LcdReadout lcd;
    juce::Label volumeLabel, reverbLabel, chorusLabel;
    juce::Slider volumeSlider, reverbSlider, chorusSlider;

    // See FoldHandle's own comment - sits right where the divider line used to be drawn,
    // above toneSectionLabel, and now draws that divider itself (see D50Editor::paint()).
    FoldHandle patchEditFold{"PATCH EDIT"};
    juce::Label toneSectionLabel, structureLabel, balanceLabel;
    juce::TextButton toneScopeToggle{"Switch to Lower"};
    // Solo/mute for auditioning, one button per PARTIAL rather than one per
    // tone - see D50AudioProcessor::setUpperPartial1Muted()'s own comment
    // (Alan, 2026-09-02: split further to localize a per-note artifact one
    // tone-wide mute couldn't pin down). Independent of toneScopeToggle,
    // which only moves which tone the Tone section below is EDITING, not
    // which partials are audible. Lit (toggle state on) means the partial
    // SOUNDS - so each button's own on/off state is the inverse of
    // D5_Bridge's mute flag, and all four start lit (nothing muted).
    juce::TextButton lowerP1OnButton{"Lo P1"}, lowerP2OnButton{"Lo P2"};
    juce::TextButton upperP1OnButton{"Up P1"}, upperP2OnButton{"Up P2"};
    // One-click A/B of the TVF ENV DEPTH keyfollow direction (Alan,
    // 2026-09-02) - see D5_Bridge::setTvfKeyfollowFixed(). Sits with the mute
    // buttons because it belongs to the same listening-test toolkit, but
    // unlike them it really changes the instrument, so its label says which
    // of the two firmware revisions is being heard rather than on/off.
    juce::TextButton keyfollowFixButton;
    bool showingLowerTone = false;
    // Structure/Balance live in the tone-common block, which moves with the
    // Upper/Lower toggle - kept as a member rather than the upper-tone
    // constant every other reference in the .cpp uses, so their onChange
    // handlers write to whichever tone is currently shown.
    int currentCommonBase = 0;
    juce::ComboBox structureBox;
    juce::Slider balanceSlider;
    PartialPanel partial1, partial2;

    // The rest of the 448-byte patch - full TVF/TVA envelopes, P-ENV, the
    // three LFOs, EQ/chorus detail, patch-common - scrolls, rather than
    // trying to cram ~150 more sliders into a window that still has to fit
    // on a screen. See buildToneColumns()/layoutToneColumns() in the .cpp.
    juce::Viewport toneViewport;
    juce::Component toneViewportContent;
    juce::OwnedArray<ParamColumn> toneColumns;
    // Which of toneColumns move with the Upper/Lower toggle, and how - see
    // setToneScope(). Patch Common columns are absent from both: they never
    // move at all.
    std::vector<ParamColumn *> partial1ScopedColumns, partial2ScopedColumns;
    std::vector<std::pair<ParamColumn *, int>> commonScopedColumns;

    D110Keyboard keyboard;

    // The sequencer - added 2026-09-03, Alan's request, mono-timbral first cut (see
    // D50AudioProcessor's own D110SequencerHost overrides). Stacked below the keyboard, same
    // drawer order the D-110 plugin uses (EditorPane, Keyboard, Sequencer, each one below the
    // previous) - closed by default, same convention D110SequencerPanel's own class comment
    // in PluginEditor.h documents for the D-110 plugin.
    FoldHandle sequencerFold{"SEQUENCER"};
    D110SequencerPanel sequencerPanel;

    bool wasReady = false;
    bool nativeTitleBarApplied = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D50Editor)
};
