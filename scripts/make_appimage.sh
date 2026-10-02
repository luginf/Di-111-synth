#!/bin/bash
# Packs a built Standalone binary into an AppImage.
#
#   scripts/make_appimage.sh --name Di-111 --binary path/to/Di-111 --icon docs/app_icon.png \
#       --comment "..." [--version 0.9.5] [--out dir] [--bundle libjack.so.0 --bundle libdb-5.3.so]
#
# Run it on the OLDEST still-supported Ubuntu LTS (the CI job uses an ubuntu:22.04 container, see
# .github/workflows/build-linux-appimage.yml): an AppImage uses the host's C library, so what it needs is
# fixed by the system it was BUILT on. One built on a recent system fails on older ones with
# "version `GLIBC_2.38' not found" (that was the previous, hand-made AppImage).
#
# --bundle LIB copies LIB (found through the binary's own `ldd`) into the AppImage. Everything else (ALSA, X11,
# freetype, fontconfig...) is expected from the host, as for any AppImage.
set -euo pipefail

NAME="" BINARY="" ICON="" COMMENT="" VERSION="" OUT="." BUNDLE=()
while [ $# -gt 0 ]; do
	case "$1" in
	--name) NAME="$2"; shift 2 ;;
	--binary) BINARY="$2"; shift 2 ;;
	--icon) ICON="$2"; shift 2 ;;
	--comment) COMMENT="$2"; shift 2 ;;
	--version) VERSION="$2"; shift 2 ;;
	--out) OUT="$2"; shift 2 ;;
	--bundle) BUNDLE+=("$2"); shift 2 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done
[ -n "$NAME" ] && [ -f "$BINARY" ] && [ -f "$ICON" ] || { echo "need --name, --binary and --icon (existing files)" >&2; exit 2; }
[ -n "$COMMENT" ] || COMMENT="$NAME"
ID=$(echo "$NAME" | tr 'A-Z' 'a-z')
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/$NAME.AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

install -m755 "$BINARY" "$APPDIR/usr/bin/$NAME"
strip "$APPDIR/usr/bin/$NAME" 2>/dev/null || true

for lib in "${BUNDLE[@]}"; do
	path=$(ldd "$BINARY" | awk -v l="$lib" '$1 == l { print $3; exit }')
	# a library the binary only needs through another one (libdb through libjack) is not in its own ldd
	[ -n "$path" ] || path=$(ldconfig -p | awk -v l="$lib" '$1 == l { print $NF; exit }')
	[ -f "$path" ] || { echo "cannot find $lib to bundle" >&2; exit 1; }
	cp -L "$path" "$APPDIR/usr/lib/$lib"
done

cp "$ICON" "$APPDIR/$ID.png"
cp "$ICON" "$APPDIR/usr/share/icons/hicolor/256x256/apps/$ID.png"
cp "$ICON" "$APPDIR/.DirIcon"

cat > "$APPDIR/$ID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$NAME
Comment=$COMMENT
Exec=$NAME
Icon=$ID
Categories=AudioVideo;Audio;Midi;Music;
Terminal=false
X-AppImage-Name=$NAME
${VERSION:+X-AppImage-Version=$VERSION}
EOF
cp "$APPDIR/$ID.desktop" "$APPDIR/usr/share/applications/$ID.desktop"

cat > "$APPDIR/AppRun" <<EOF
#!/bin/sh
HERE="\$(dirname "\$(readlink -f "\$0")")"
export LD_LIBRARY_PATH="\$HERE/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "\$HERE/usr/bin/$NAME" "\$@"
EOF
chmod +x "$APPDIR/AppRun"

# appimagetool from the AppImage project: its runtime is the current static "type 2" one, which needs neither
# libfuse2 nor a particular C library on the machine that runs the result. Override with APPIMAGETOOL=path.
TOOL="${APPIMAGETOOL:-}"
TOOL_ARGS=()
if [ -z "$TOOL" ]; then
	TOOL="$WORK/appimagetool-x86_64.AppImage"
	curl -fsSL -o "$TOOL" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
	chmod +x "$TOOL"
	# no FUSE in a container: unpack and run instead of mounting
	TOOL_ARGS=(--appimage-extract-and-run)
fi

TARGET="$OUT/$NAME-x86_64.AppImage"
ARCH=x86_64 "$TOOL" "${TOOL_ARGS[@]}" --no-appstream "$APPDIR" "$TARGET"
echo "built $TARGET"
echo "glibc symbol versions it needs (should not exceed the build system's LTS):"
objdump -T "$APPDIR/usr/bin/$NAME" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
