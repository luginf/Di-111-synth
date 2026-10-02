# Building: full details, and the optional MAME-backed backend

See the [README](../README.md#build) for the short version (the default native-core build).
This is the detail, plus how to build the opt-in MAME-backed `D110Emulator`.

Requires CMake and a C++ compiler (Visual Studio Build Tools on Windows; GCC and the SDL OSD's
dependencies on Linux - `libsdl2-dev libsdl2-ttf-dev libfontconfig1-dev libpulse-dev` cover it on
Debian/Ubuntu). JUCE is fetched automatically by CMake on first configure.

There are two interchangeable CPU backends - see [`plugin/Source/native/`](../plugin/Source/native)
vs. `plugin/Source/D110Core.*` - built as two separate plugins that can sit side by side in a
DAW's plugin list. **`D110EmulatorNative` is the default build and needs nothing beyond CMake
and a compiler**: the D-110's own i8x9x/MCS-96 CPU, hand-ported out of MAME's device/scheduler
framework into a standalone interpreter with zero MAME dependency, stepped inline on the audio
thread. The original `D110Emulator` runs the same firmware inside an embedded
[MAME](https://github.com/mamedev/mame) instance instead; it is kept in the tree as a dormant,
opt-in fallback (`-DD110_BUILD_MAME_BACKEND=ON`) rather than deleted, since it is the
longer-proven of the two.

Default build (native core only, no MAME):

```
cd plugin
cmake -B build -S .
cmake --build build --config Release
```

The built `.vst3` is copied automatically to the platform's shared VST3 folder
(`C:\Program Files\Common Files\VST3` on Windows, `~/Library/Audio/Plug-Ins/VST3` on macOS,
`~/.vst3` on Linux).

### macOS: Intel (x86_64) and Apple Silicon (arm64)

Building on an actual Intel Mac needs nothing special - the commands above already produce a
binary for whatever Mac you run them on, targeting whatever macOS version you're building on.
The official release binaries, though, are built on an Apple Silicon CI runner; to also cover
Intel Macs, add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` to the `cmake -B build -S .` line
above. Xcode's toolchain can cross-compile the x86_64 slice from an arm64 Mac (and vice versa)
with no other hardware involved - the two slices are `lipo`-combined into one universal
`.app`/`.vst3` automatically. Left unset, `CMAKE_OSX_DEPLOYMENT_TARGET` defaults to the build
machine's own SDK version, which on a CI runner is usually much newer than most users' actual
macOS - the resulting binary gets Gatekeeper-refused ("You can't use this version of the
application with this version of macOS") on anything older, so also add
`-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0` (Big Sur, the practical floor for a universal binary
since arm64 Macs don't exist on anything older) when reproducing a release-style build. This
is exactly how the project's own release zips are built (see `.github/workflows/build-macos.yml`).

### Optional: JACK MIDI input port (Linux Standalone only)

If libjack's dev headers are found at configure time (`libjack-dev`/`libjack-jackd2-dev`, or
PipeWire's own JACK-compatible package), the Linux Standalone build also gets a real JACK MIDI
input port (`D-110 Emulator:midi_in`) so a DAW's MIDI Out can be wired into it directly through
any JACK patchbay. Nothing to turn on - detected automatically, silently absent everywhere else
(Windows/macOS, or a Linux box without those headers). See
[`docs/host_compatibility.md`](host_compatibility.md) for the details.

## Optional: also building the MAME-backed `D110Emulator`

**MAME is not vendored here and must be built first**, because its libraries are what run the
firmware. From a [MAME 0.288](https://github.com/mamedev/mame/releases/tag/mame0288) tree,
apply the one required patch below - it fixes a real crash, an out-of-bounds array access
reachable from this project's own EXTINT workaround, not just a cosmetic difference - then
build:

Windows:
```
cd <mame-tree>
git apply <this-repo>/patches/mame_mcs96_stale_irq_level.patch
make vs2022 MSBUILD=1 PTR64=1 MINGW64=C:/msys64/mingw64 MINGW32=C:/msys64/mingw32 \
     NOWERROR=1 PYTHON_EXECUTABLE=python SUBTARGET=d110 \
     SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0
```

Linux (uses MAME's own default SDL OSD, no extra flags needed):
```
cd <mame-tree>
git apply <this-repo>/patches/mame_mcs96_stale_irq_level.patch
make SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp -j$(nproc)
```

See [`../patches/README.md`](../patches/README.md) for what the patch fixes and why it's required.

Then point `MAME_DIR` in [`../plugin/mame.cmake`](../plugin/mame.cmake) at that tree (or pass
`-DMAME_DIR=...` on the CMake command line), and turn the backend on:

```
cd plugin
cmake -B build -S . -DD110_BUILD_MAME_BACKEND=ON -DMAME_DIR=<mame-tree>
cmake --build build --config Release
```

On Windows, everything is built with the static runtime (`/MT`) to match MAME's release
libraries.

## Linux AppImage

An AppImage runs against the C library of the machine it is started on, so it has to be **built on an old
one**: built on a recent system it fails on older distributions with `version 'GLIBC_2.38' not found`
(that is what the AppImage catalog's automatic test reported for the first, hand-made one). The CI job
`.github/workflows/build-linux-appimage.yml` therefore builds in an `ubuntu:22.04` container (the oldest
Ubuntu LTS still supported - move it forward when that goes out of support) and packs the Standalone with
`scripts/make_appimage.sh`.

To do the same locally you need a 22.04 environment, for example Docker (your user must be in the `docker`
group):

```
docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 bash -c '
  apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake git curl ca-certificates file binutils pkg-config libasound2-dev \
    libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
    libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev \
    mesa-common-dev libcurl4-openssl-dev libgtk-3-dev libwebkit2gtk-4.1-dev &&
  cmake -S plugin -B /tmp/b -DCMAKE_BUILD_TYPE=Release &&
  cmake --build /tmp/b --target D110EmulatorNative_Standalone -j"$(nproc)" &&
  bash scripts/make_appimage.sh --name Di-111 --binary /tmp/b/D110EmulatorNative_artefacts/Release/Standalone/Di-111 \
    --icon docs/app_icon_256.png --comment "Multi-timbral sound module emulator" \
    --bundle libjack.so.0 --bundle libdb-5.3.so --out dist'
```

The script prints the highest glibc version the packed binary needs; on a 22.04 build it must not exceed 2.35.

