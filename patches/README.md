# Patches for MAME

The plugin builds against an **unmodified** MAME tree - that is a property of it, and it stays
true for one of the patches below but not for the others: `mame_mcs96_stale_irq_level.patch`
is **mandatory**, see its own section. `mame_flopimg_missing_string_view_include.patch`
is also **mandatory**, but only when building on Windows with MSVC - see its section.
`mame_roland_d10_dropped_writes.patch` is still optional - nothing in it is needed for the
plugin to work. This directory holds fixes found while working on the D-110 that belong to MAME
itself, not to the plugin. They are kept here for a simple reason: the MAME tree is shared by several
projects, is not vendored here and holds other people's uncommitted changes, so a fix left in
it is easily lost.

Apply to a MAME 0.288 tree:

```
cd <mame-tree>
git apply <path>/mame_mcs96_stale_irq_level.patch
git apply <path>/mame_flopimg_missing_string_view_include.patch
git apply <path>/mame_roland_d10_dropped_writes.patch
```

## `mame_flopimg_missing_string_view_include.patch` — REQUIRED on Windows/MSVC

`src/lib/formats/flopimg.h` uses `std::string_view` (`extension_matches`, declared and
defined against it) but never includes `<string_view>` - only `<memory>`, `<vector>`,
`<cassert>`, `<cstddef>`, `<cstdint>`. It still compiles on toolchains where some other
standard header happens to transitively drag `<string_view>` in, which is exactly what
masked this for years: found building MAME's `formats` project (this fork's
`.github/workflows/build-windows.yml`) against a GitHub Actions `windows-latest` runner's
MSVC/STL, which apparently doesn't. The symptom is `error C2039: 'string_view': is not a
member of 'std'` at `flopimg.h`'s own declaration, immediately ruling out anything to do
with `/std:` flags or `LanguageStandard` project settings (the generated `formats.vcxproj`
already requested `stdcpp20` in every configuration - confirmed by printing it in CI before
concluding this was a real header bug, not a build-flag one). One-line fix: add the missing
`#include <string_view>`.

Not confirmed necessary on Linux/GCC or macOS/Clang - both apparently transitively pull in
`<string_view>` some other way - so it's flagged Windows-only above rather than folded into
the always-required patch, but applying it everywhere is harmless.

## `mame_mcs96_stale_irq_level.patch` — REQUIRED

Fixes a crash: MCS-96 core's interrupt-vector fetch (`src/devices/cpu/mcs96/mcs96ops.lst`,
the `fetch` block) computes which interrupt level to take from a **fresh** scan of
`PSW & pending_irq`, but only runs that scan because a **stale** flag (`irq_requested`,
snapshotted at the end of the previous instruction) said an interrupt was pending. If
something clears the one bit that flag was based on before this fetch's own re-scan runs -
which is exactly what `D110Core.cpp`'s `midiTick()` does to work around EXTINT never
self-clearing (see `docs/la32_interface.md`, 2026-07-31) - the scan finds nothing and `level`
is left at **-1**. That's taken as a real level anyway: `1<<(-1)` into `pending_irq`, `-1`
into `OP1`, and `-1` as the index into `standard_irq_callback()`'s `m_input[]` - undefined
behaviour, reproduced as a SIGSEGV inside `device_execute_interface::device_input::
default_irq_callback()` (confirmed via `coredumpctl`/gdb backtrace against a real Carla
crash report, thread running `mcs96_device::execute_run` at the time).

The fix is a defensive guard: if the re-scan finds nothing (`level < 0`), skip taking an
interrupt this cycle instead of acting on the sentinel value. `check_irq()` re-evaluates
`irq_requested` fresh after every subsequent instruction regardless, so this cannot lose a
real interrupt - it only skips the specific cycle where the flag had already gone stale.

Same patch also fixes `fe7f mulb indexed_2b` reading its byte operand through `any_r16()` -
which masks the address to even before reading a word - instead of `any_r8()`, the only one
of the eight `indexed_2b` byte ops in the file that didn't already use it. No observed
audible or crash effect from this one today (the destination is inside the LA32 register
window this project's stub routes around, and an unmirrored RAM offset - see
`docs/la32_interface.md`'s 2026-07-31 entry, which found the same bug independently and left
it unfixed as out of scope at the time), but it's an unambiguous, one-line copy-paste bug
fixed by the same read while already in this file for the crash above.

Verified after applying: `d110_longrun_test`'s heaviest phase (chords, notes forwarded to
the firmware - the phase that generates real, sustained EXTINT traffic through
`StuckPolicy::La32Ramps`) ran a full 28-second stress window with the panel responsive
throughout and no crash, where the unpatched build reproducibly segfaulted on a real user's
machine under the same plugin build within seconds of opening the editor.

## `mame_roland_d10_dropped_writes.patch` — optional

The `roland_d10.cpp` driver silently drops two groups of writes that the D-110 firmware really
makes.

**The SO latch also answers at `0x0280`** - the same address up to A7 - while the map
describes only `0x0200`, so these writes fall through as unmapped. They are not accidental:
they are what raises R.SW (output to the analog board) and selects the reverb chip's program.
The firmware makes them three times at boot, from ROM `0x1C94`, `0x1CC1` and `0x20FF`,
with the values `2C` and `0C`; by the bit layout described in `so_w` itself, that is "program 2,
R.SW on", whereas both writes to `0x0200` are zeros.

**Writes to `0x021A` are declared `nopw()`** and thrown away. Meanwhile the firmware puts
bits 1-3 of the reverb type there: sweeping all eight types plus OFF gives `00 02 02 04 04 06 06
08`, i.e. `type & 0x0E`. The low bit of the type goes separately - into bit 2 of the external latch
`0x0800`. The same byte is also written by the panel scan, changing only bit 0 in it.

Verified by measurement, with a control: with the same presses and the same note, but without changing the type, there is
**not a single write** to either address. Details and a walkthrough of the ROM routine `0x4C7B`-`0x4CC5` are
in [`../docs/service_notes_findings.md`](../docs/service_notes_findings.md).

The patch does not change behaviour: `so_w` in MAME still only logs, and the new handler just
accepts the byte - there is nothing to consume these bits yet, nobody emulates the reverb chip.
The point is that the driver stops losing data and the code records what it means.
Verified after applying: the unit boots, all eight types give the same values as
before the patch, the demo song plays for 75 seconds with a live panel at 100% of real time.

**A caveat that must travel with the patch:** the `0x0280` mirror is inferred from the fact that the
values decompose meaningfully by the bits of `so_w`, not from the schematic - the address decoding
sits inside the gate array IC16 and is not visible on the schematic. That bit 0 in
`0x021A` is the panel column strobe is also consistent with measurement, but was not
proven separately.
