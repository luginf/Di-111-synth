#pragma once

// The whole surface D110SequencerPanel actually needs from whatever it's embedded in -
// six required methods plus six optional ones below, deliberately, so the panel (like the
// engine underneath it) doesn't have to depend on the full D-110 plugin. D110AudioProcessor
// implements this for the plugin; NonetSeqHost implements it for Nonet Sequencer, the
// independent sequencer app, which has no firmware, no ROMs, and nothing else to power on.

#include <juce_core/juce_core.h>

#include <vector>

namespace d110seq { class D110SequencerEngine; }

class D110SequencerHost {
public:
	virtual ~D110SequencerHost() = default;

	virtual d110seq::D110SequencerEngine &getSequencer() = 0;

	// All 4 song slots at once, as a standalone .midiseq file - see
	// D110SequencerSongsFile.h, which both implementations delegate to.
	virtual void exportSequencerSongs(const juce::File &file) = 0;
	virtual void importSequencerSongs(const juce::File &file) = 0;

	// Right-click STOP: all-notes-off, everywhere this host can reach - the firmware/sound
	// engine for the plugin, the direct MIDI Out port for both.
	virtual void midiPanic() = 0;
	// The deliberate version of the above (right-click on STOP): a host whose instrument can keep a note
	// sounding that nothing will ever release (the D-110's firmware) clears that too. Not safe to follow
	// immediately with PLAY on such a host, which is why a plain STOP uses midiPanic() instead.
	virtual void midiPanicHard() { midiPanic(); }

	// Where the panel's own file dialogs (Load/Save .mid, Load/Save .midiseq) should
	// start from and remember afterwards - one shared "last folder" per host, the same one
	// D110AudioProcessor's other dialogs (SysEx bank, memory snapshot) also read/write, so
	// picking a folder anywhere in the app carries over everywhere else instead of each
	// dialog independently defaulting back to $HOME.
	virtual juce::File getLastDialogDir() const = 0;
	virtual void setLastDialogDir(const juce::File &dir) = 0;

	// Optional: whether this host lets a track's MIDI channel be changed from the panel
	// itself (clicking its CH readout). False by default - the plugin's own channel map
	// comes from the live firmware's SYSTEM page instead, so D110AudioProcessor doesn't
	// override either of these. NonetSeqHost does: it has no firmware/SYSTEM page to set
	// channels from, so this is the only way to point a track anywhere but the factory
	// default (Part 1-8 -> channels 2-9, Rhythm -> 10).
	virtual bool supportsTrackChannelEdit() const { return false; }
	virtual void setTrackChannel(int /*track*/, int /*channel*/) {}

	// Optional: whether this host lets the panel offer D110SequencerEngine's extra 7 tracks
	// (see D110SequencerEngine::kMaxTracks) at all. False by default - the D-110 plugin's
	// sequencer is always exactly the 9 D-110-shaped tracks (Parts 1-8 + Rhythm); the actual
	// enable/disable flag and track data live on the engine itself
	// (setExtraTracksEnabled()/getExtraTracksEnabled(), reachable via getSequencer()), so
	// this is purely a capability gate for the panel's own "activate extra tracks" right-click
	// menu and page-switch buttons - NonetSeqHost is the only override, returning true.
	virtual bool supportsExtraTracks() const { return false; }

	// Optional: whether this host lets a track have its own fixed MIDI Program Change (0-127,
	// -1 = none), sent once at the moment PLAY/REC starts, so whatever's on that channel picks
	// the right patch without a separate manual step first. False by default; both concrete
	// hosts override it to true, but the wire format they send is NOT the same (see
	// getTrackBank()'s own comment) - NonetSeqHost sends a real Bank Select (CC0) + Program
	// Change out over MIDI Out, to whatever external synth is listening; D110AudioProcessor
	// sends a bare Program Change into the firmware's own MIDI IN (plus MIDI Out), since the
	// D-110 has no Bank Select of its own to receive. The VALUE itself lives directly on
	// D110SequencerEngine, per song slot - see its own getTrackProgram()'s comment (2026-08-21:
	// this used to be a workspace-wide value shared by all 4 songs, which Alan pointed out
	// doesn't make sense - a song's own instrumentation is part of what makes it that song).
	// These two methods are just a thin pass-through to the engine, kept on the host interface
	// only because the actual SENDING mechanism (below) is genuinely host-specific.
	virtual bool supportsProgramChange() const { return false; }
	virtual int getTrackProgram(int /*track*/) const { return -1; }
	virtual void setTrackProgram(int /*track*/, int /*program*/) {}

	// Optional narrowing of supportsProgramChange() to specific tracks - default mirrors the
	// blanket flag. The D-110 plugin overrides this to exclude the Rhythm track: Program
	// Change picks one of a melodic Part's 128 stored Timbres, and Rhythm has no equivalent
	// single-number selection (each key already has its own sound) - see
	// D110AudioProcessor::supportsProgramChangeForTrack().
	virtual bool supportsProgramChangeForTrack(int track) const { return supportsProgramChange(); }

	// Same 1-128 musician-facing numbering as getTrackProgram(), only meaningful once a Program
	// Change is actually set - defaults to bank 1 rather than "none" since most synths treat
	// bank 0/MSB-absent as bank 1 anyway, so there's no useful "unset" state to represent. What
	// it actually SENDS is host-specific: NonetSeqHost sends a real Bank Select (CC0, MSB only)
	// immediately before the Program Change, the generic GM-era mechanism most external synths
	// expect. D110AudioProcessor sends no CC0 at all - the D-110 predates that convention -
	// and instead folds BANK straight into the raw Program Change byte, since its 128 Timbre
	// Memory slots are two pages of 64 ("A"/"B" on the instrument's own panel) addressed purely
	// by that one number; see the processBlock() sequencer-Program-Change site's own comment.
	virtual int getTrackBank(int /*track*/) const { return 1; }
	virtual void setTrackBank(int /*track*/, int /*bank*/) {}

	// MIDI actually has two Bank Select controllers, not one - CC0 (MSB, above) and CC32 (LSB) -
	// and real gear can require either or both depending on how many banks it has. Optional:
	// false/unimplemented by default (getTrackBank()'s single CC0 already covers the common
	// case, and D-110 has neither), NonetSeqHost is the only override, sending CC32 right after
	// CC0 and before the Program Change, standard MIDI ordering. Same 1-128 musician-facing
	// numbering and "no useful unset state" reasoning as getTrackBank().
	virtual bool supportsBankLsb() const { return false; }
	virtual int getTrackBankLsb(int /*track*/) const { return 1; }
	virtual void setTrackBankLsb(int /*track*/, int /*bankLsb*/) {}

	// One named patch out of a loaded instrument definition file (see
	// supportsInstrumentDefinitions() below) - prog/bank/bankLsb are 0-based wire values, same
	// convention D110SequencerEngine::Track's own fields use, ready to feed straight into
	// setTrackProgram()/setTrackBank()/setTrackBankLsb() (which take the 0-based value, not the
	// 1-based number the dialog shows). bank/bankLsb are -1 when the source .idf entry left that
	// attribute out entirely (most GM-only files never set one at all) - leave the corresponding
	// field untouched in that case, same "nothing to say" meaning -1 already carries on
	// getTrackProgramHint().
	struct InstrumentPatch {
		juce::String group, name;
		int prog = 0, bank = -1, bankLsb = -1;
	};

	// Optional: whether this host can load a MusE-style instrument definition (.idf - plain XML,
	// <MidiInstrument><PatchGroup><Patch name="..." prog=".." hbank=".." lbank=".."/>) and use it
	// to label patches by name in the Program Change dialog's own "Pick instrument..." picker,
	// rather than by bare number - Nonet Sequencer only (Alan's request, 2026-08-26). The D-110
	// plugin doesn't override this: its own Program Change always addresses this instrument's own
	// Timbre Memory directly, which already has real names of its own (the TIMBRES tab reads them
	// straight off the firmware), so an externally-authored file naming some OTHER synth's patches
	// wouldn't describe what Program Change here actually selects.
	// getInstrumentDefinitionPath() persists just the file's path (NonetSeqHost's own settings
	// file, like getRetroKeyBindings() does for its own string) - the patch list itself is
	// re-parsed from that path at startup, not persisted separately; a path that no longer
	// resolves (moved/deleted file) leaves getInstrumentPatches() empty rather than erroring.
	// loadInstrumentDefinition() returns false (and leaves whatever was loaded before in place)
	// if the file can't be parsed as one of these.
	virtual bool supportsInstrumentDefinitions() const { return false; }
	virtual juce::String getInstrumentDefinitionName() const { return {}; }
	virtual juce::String getInstrumentDefinitionPath() const { return {}; }
	virtual bool loadInstrumentDefinition(const juce::File & /*file*/) { return false; }
	virtual std::vector<InstrumentPatch> getInstrumentPatches() const { return {}; }

	// Re-sends whatever supportsProgramChange() would send at the next PLAY/REC edge right now
	// instead, even if the transport is already mid-play - Alan asked for this 2026-08-19 as a
	// manual "resync the current sound with what the sequencer has stored" escape hatch, for
	// whenever the two have drifted apart (changed a track's Program Change/Bank/Volume/Pan
	// while already playing, or the synth's patch changed some other way since). No-op by
	// default; both concrete hosts override it to re-run their own PLAY-edge send.
	virtual void resyncProgramChanges() {}

	// Optional: whether this host also lets a track carry a fixed Channel Volume/Pan, sent
	// alongside its Program Change (same PLAY/REC-edge moment, same resyncProgramChanges()).
	// False by default (2026-08-19, Alan's request). The wire format differs, same story as
	// getTrackBank(): D110AudioProcessor sends real Part LEVEL/PAN via SysEx DT1 (D-10 SYSTEM's
	// kTimbreTempRecord fields 8/9, the exact same write the PARTS tab's own LEVEL/PAN columns
	// already use) - not MIDI CC7/CC10, since unlike Bank Select there's no evidence the
	// firmware answers to those and this project doesn't ship guesses about firmware behaviour
	// (see D110EditorPane::layoutParts()). NonetSeqHost sends real MIDI CC7 (Volume, 0-100
	// scaled to 0-127) then CC10 (Pan, 0-14 scaled to 0-127) ahead of the Program Change, the
	// generic mechanism external synths expect.
	// -1 = not set (send neither) - same "no useful unset value to represent" limits as
	// getTrackBank(), just spelled out as -1 here since 0 is itself a meaningful LEVEL/PAN.
	// Range is the D-110's own: LEVEL 0-100, PAN 0-14 (15 positions, 7 = centre) - kept as the
	// one musician-facing scale both hosts show, even though NonetSeqHost's own wire values are
	// plain 0-127 CC data underneath.
	virtual bool supportsTrackVolumePan() const { return false; }
	virtual int getTrackVolume(int /*track*/) const { return -1; }
	virtual void setTrackVolume(int /*track*/, int /*volume*/) {}
	virtual int getTrackPan(int /*track*/) const { return -1; }
	virtual void setTrackPan(int /*track*/, int /*pan*/) {}

	// Optional narrowing of supportsTrackVolumePan() to specific tracks, same idea as
	// supportsProgramChangeForTrack() narrows supportsProgramChange() - default mirrors the
	// blanket flag (NonetSeqHost doesn't override: its Rhythm track already gets real CC7/CC10
	// like any other track, nothing D-110-specific to exclude). D110AudioProcessor overrides
	// this to stay true for Rhythm even though supportsProgramChangeForTrack() excludes it -
	// Rhythm has no Program Change equivalent, but it has exactly as real a LEVEL/PAN as any
	// melodic Part (2026-08-21, Alan's request: a fixed default Volume for the rhythm track).
	virtual bool supportsTrackVolumePanForTrack(int track) const { return supportsTrackVolumePan(); }

	// Optional: whether this host can read back its OWN current live sound per part (D110AudioProcessor
	// only - Nonet Sequencer has no synth of its own to read from) and use it to overwrite the
	// per-track Program Change/Bank/Volume/Pan above, one melodic part at a time - the opposite
	// direction from resyncProgramChanges() above (which pushes the stored settings TO the live
	// sound; this pulls the live sound INTO the stored settings). Alan asked for this 2026-08-19
	// as a way to capture "whatever I've dialled in by hand on the panel right now" into the
	// current song, rather than having to type Program/Volume/Pan numbers in by hand. Destructive
	// to whatever was stored before, so the UI must confirm before calling this. False/no-op by
	// default.
	virtual bool supportsCaptureLivePatch() const { return false; }
	virtual void captureLivePatchIntoTracks() {}

	// Optional convenience hint for the Program Change dialog's pre-fill: -1 = no hint (leave
	// the field blank when no override is set yet - Nonet-Seq's original behaviour, since it
	// has no sound engine of its own to read a "current" value from). The D-110 plugin
	// overrides this to report the Part's own live tone number, so the dialog defaults to
	// "whatever this Part is playing right now" - see
	// D110AudioProcessor::getTrackProgramHint().
	virtual int getTrackProgramHint(int /*track*/) const { return -1; }

	// Same idea as getTrackProgramHint(), for Volume/Pan - see
	// D110AudioProcessor::getTrackVolumeHint()/getTrackPanHint(). Nonet-Seq has no synth of its
	// own to hint from, so these stay -1 there, same reasoning as getTrackProgramHint().
	virtual int getTrackVolumeHint(int /*track*/) const { return -1; }
	virtual int getTrackPanHint(int /*track*/) const { return -1; }

	// Per-song sound snapshot: the instrument's whole memory (every Patch, Timbre, Tone and
	// System setting - see docs/sysex_address_map.md), captured/restored per song slot so
	// switching which song is loaded can also recall which sounds it used - a much bigger,
	// heavier-handed recall than the per-track Program Change override above (which only ever
	// covers Program Change/Bank/Volume/Pan, now also per-slot in its own right - see that
	// override's own comment). Plugin-only - Nonet-Seq has no firmware/memory of its own to
	// snapshot, so these stay false/no-op there, same reasoning as supportsTrackChannelEdit(). See
	// D110AudioProcessor::storeSoundSnapshotForSlot()/loadSoundSnapshotForSlot() and
	// D110SequencerPanel::showCopySongMenu(), which offers both from the song-slot buttons'
	// right-click menu.
	virtual bool supportsSoundSnapshots() const { return false; }
	virtual bool hasSoundSnapshot(int /*slot*/) const { return false; }
	virtual void storeSoundSnapshotForSlot(int /*slot*/) {}
	virtual void loadSoundSnapshotForSlot(int /*slot*/) {}

	// Persisted custom key bindings for the retro sequencer's D-pad (Alan's request,
	// 2026-08-23 - until this, a rebind via D110SequencerRetroPanel's KEY BINDINGS menu was
	// lost the moment the app closed). Encoded as a semicolon-joined list of
	// juce::KeyPress::getTextDescription() strings, one per
	// D110SequencerRetroPanel::BindingIndex slot in that enum's order; decoded with
	// juce::KeyPress::createFromDescription(). Empty string (the default, and what an old
	// saved project/settings file predating this feature will report) means "no override -
	// use the built-in numpad defaults", same fallback D110SequencerRetroPanel already
	// applies to a single malformed entry. Both concrete hosts just store/return the string
	// as-is, persisting it the same way they persist everything else (state XML for the
	// plugin, its own settings file for Nonet Sequencer) - the panel owns the encode/decode.
	virtual juce::String getRetroKeyBindings() const { return {}; }
	virtual void setRetroKeyBindings(const juce::String & /*encoded*/) {}

	// Label for HOME's PLAY/STOP/REC/[MIDI]/OPTIONS quick-bar row (Alan's request,
	// 2026-08-23: given an app-identity name since it's effectively that app's whole master
	// control surface, rather than the generic engineering term "TRANSPORT" - further
	// renamed the same day, Alan didn't like that generic term either). The D-110 plugin
	// keeps this default; Nonet Sequencer overrides it to its own app name.
	virtual juce::String transportRowLabel() const { return "D110-SEQ"; }

	// Persisted OPTIONS > LCD LINES toggle (Alan's request, 2026-08-23): false (default) is
	// the normal 4-line display (1 title row + 3 body rows); true switches to a 2-line
	// display (1 title row + 1 body row) with roughly double the character size, trading how
	// much is visible at once for legibility - see D110SequencerRetroPanel::bodyRows(). Both
	// concrete hosts just store/return the bool as-is, persisted the same way
	// getRetroKeyBindings() is.
	virtual bool getRetroLcdCompactMode() const { return false; }
	virtual void setRetroLcdCompactMode(bool /*compact*/) {}

	// Optional: sounds one note on a track's own live channel right now, so a click in the grid
	// editor (D110SequencerGridPanel) is heard as the pitch it just placed or grabbed. Goes
	// through the same path as the on-screen keyboard's own notes. A no-op by default (the
	// grid editor just stays silent), both concrete hosts override it.
	virtual void auditionTrackNote(int /*track*/, int /*note*/, int /*velocity*/, bool /*on*/) {}

	// Optional: the grid editor's (D110SequencerGridPanel) row height in pixels, so the choice
	// survives a restart. 0 = never set (the panel then uses its 14px desktop default; the
	// Android app sets a larger one for fingers the first time it runs). Both concrete hosts
	// persist it with the rest of their state.
	virtual int getGridRowHeight() const { return 0; }
	virtual void setGridRowHeight(int /*pixels*/) {}
};
