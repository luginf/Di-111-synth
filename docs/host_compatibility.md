# DAW/host compatibility

## Direct MIDI ports

Both the Standalone app and the VST3 plugin can open a system MIDI input/output pair of their
own, independent of whatever the host routes (Options menu -> "MIDI In"/"MIDI Out"). Useful for
pointing the app directly at a hardware controller or an external editor.

### JACK MIDI input (Linux Standalone only)

If JACK is available, the Linux Standalone build also exposes a real JACK MIDI input port,
`D-110 Emulator:midi_in`, visible in any patchbay (qjackctl, Catia, qpwgraph...) - so a DAW's
MIDI Out can be wired in directly. Detected automatically at build time; silently absent if
JACK's dev headers weren't found, or on Windows/macOS. Standalone only, not the VST3 plugin.

## Known VST3 host issues

The same VST3 build behaves differently across hosts:

| Host | Status |
| --- | --- |
| Ardour | Notes work. Pitch bend did not reach the plugin until the VST3 MIDI-CC shim was re-enabled (issue #8): VST3 hosts deliver bend/modulation as parameter changes through IMidiMapping, not as MIDI events. To be re-verified in Ardour. |
| Carla | Sound is pitched up; hardware pitch bend has no effect. Also: Carla doesn't forward live SysEx to hosted VST3 plugins, so a live external editor (e.g. Edisyn) won't reach the plugin there - use file-based import, or a host that does forward it. |
| FL Studio | Pitch bend/modulation not delivered before the same fix (issue #8). Needs re-testing. |
| Gig Performer | Bend and modulation work. A fixed pitch offset of about -3.3 semitones was reported (issue #8); sample rate is ruled out (the same note renders at the same pitch at 44.1/48/96 kHz), likely a stray initial Pitch Bend 0, now filtered. Needs re-testing. |
| Qtractor | Plugin loads and shows a patch on the LCD, but produces no audio. |

The Standalone build is unaffected by any of this. These look like host-side bugs rather than
something in this project's VST3 handling (the same code works correctly in Ardour), but the
root cause hasn't been pinned down. If you hit either issue, Ardour or the Standalone app are
the reliable options today.


### Pitch bend, modulation and the MIDI-CC shim

`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS` must stay on: without it a VST3 host that delivers
pitch bend/CC as parameter changes (Ardour, FL Studio) reaches nothing. Its side effect, a host
that resets all parameters to default sending a Pitch Bend of 0 (fully down), is filtered in
`processBlock()` by `dropRogueInitialPitchBend()` (`plugin/Source/PitchBendGuard.h`, tested by
`d110_pitch_bend_guard_probe`).
