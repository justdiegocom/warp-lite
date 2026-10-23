#!/usr/bin/env bash
# Compila warp-lite.exe (x86_64 Windows) con zig. Se ejecuta en el Mac:
#   tools/build.sh            -> desde este box: copia al Mac, compila allí y trae dist/warp-lite.exe
#   tools/build.sh --local    -> compila aquí mismo (necesita zig en el PATH)
# warp-remote.exe (the phone sidecar, Rust in warp-remote/) is cross-compiled with cargo-zigbuild and goes into
# the installer next to Warp.exe; it needs rustup's x86_64-pc-windows-gnu target and cargo-zigbuild.
set -euo pipefail
cd "$(dirname "$0")/.."
REMOTE=mac
RDIR='~/agent-work/warp-lite-build'

build_here() {
  ZIG=${ZIG:-$(command -v zig || echo /opt/homebrew/bin/zig)}
  T=x86_64-windows-gnu
  mkdir -p build dist
  # A preview (see the end): its version into this copy of the sources, never into a checkout.
  if [[ -n "${WARP_LITE_VERSION:-}" && ! -e .git ]]; then
    V=$WARP_LITE_VERSION
    [[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "bad WARP_LITE_VERSION: $V" >&2; exit 1; }
    perl -pi -e "s/kVersion = L\"[0-9.]*\"/kVersion = L\"$V\"/" src/setup.cpp
    perl -pi -e "s/(FILEVERSION|PRODUCTVERSION) [0-9,]+/\$1 ${V//./,},0/; s/(\"(File|Product)Version\", )\"[0-9.]+\"/\$1\"$V\"/" src/warp-lite.rc
    echo "preview version $V"
  fi
  objs=()
  for c in third_party/libvterm/src/*.c; do
    o=build/$(basename "$c" .c).o
    "$ZIG" cc -target $T -O2 -std=c99 -Ithird_party/libvterm/include -c "$c" -o "$o" &
    objs+=("$o")
  done
  "$ZIG" c++ -target $T -O2 -std=c++17 -c third_party/qrcodegen/qrcodegen.cpp -o build/qrcodegen.o &
  objs+=(build/qrcodegen.o)
  "$ZIG" c++ -target $T -O2 -std=c++17 -municode -Ithird_party/libvterm/include -Ithird_party/qrcodegen \
    -c src/main.cpp -o build/main.o &
  (cd warp-remote && cargo zigbuild --release --locked --target x86_64-pc-windows-gnu) &
  remote=$!
  wait "$remote"  # its failure stops the build (set -e does not see background jobs)
  wait
  cp "${CARGO_TARGET_DIR:-warp-remote/target}/x86_64-pc-windows-gnu/release/warp-remote.exe" dist/warp-remote.exe
  # zig ignora -mwindows: sin --subsystem,windows el .exe abre una consola al arrancar.
  "$ZIG" c++ -target $T -O2 -municode -Wl,--subsystem,windows build/main.o "${objs[@]}" src/warp-lite.rc \
    -lgdi32 -luser32 -lkernel32 -lshell32 -lole32 -luuid -lwindowscodecs -ladvapi32 -lwinhttp -lbcrypt -lrstrtmgr -static -o dist/warp-lite.exe
  python3 - <<'PY'
import struct, sys
for exe in ('dist/warp-lite.exe', 'dist/warp-remote.exe'):  # neither may open a console
    d = open(exe, 'rb').read()
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    sub = struct.unpack('<H', d[pe + 24 + 68:pe + 24 + 70])[0]
    if sub != 2:
        sys.exit(f'{exe} no es una app de ventana (subsistema {sub})')
PY
  # Instalador: lleva dentro dist/warp-lite.exe y dist/warp-remote.exe (por eso va después).
  # La huella de los .exe entra en la clave de caché de zig (que no sigue el .incbin).
  STAMP=$(cat dist/warp-lite.exe dist/warp-remote.exe | shasum | cut -c1-16)
  "$ZIG" c++ -target $T -O2 -std=c++17 -municode -DWARP_LITE_STAMP=$STAMP -Wl,--subsystem,windows src/setup.cpp src/warp-lite.rc \
    -lole32 -lshell32 -luuid -ladvapi32 -luser32 -static -o dist/warp-lite-setup.exe
  ls -l dist/warp-lite.exe dist/warp-remote.exe dist/warp-lite-setup.exe
}

if [[ "${1:-}" == "--local" ]]; then build_here; exit; fi
# A preview built from here (the release workflow stamps its own version and builds with --local) carries the version
# of the latest release: the updater only installs a strictly newer one, so an installed preview is not replaced by
# that release (it was, 0.3.7 < the latest), and the release made once it is merged, a higher version, does replace it.
VER=$(git ls-remote --tags --refs origin 'v*' 2>/dev/null | sed 's#.*refs/tags/v##' | sort -V | tail -n1 || true)
[[ -n "$VER" ]] || echo "warning: no release tag found: this build keeps the version in the sources" >&2
ssh -o BatchMode=yes "$REMOTE" "mkdir -p $RDIR"
rsync -az --delete --exclude .git --exclude dist --exclude build --exclude target --exclude mobile ./ "$REMOTE:$RDIR/"
ssh -o BatchMode=yes "$REMOTE" "cd $RDIR && WARP_LITE_VERSION=$VER PATH=/opt/homebrew/bin:\$HOME/.cargo/bin:\$PATH bash tools/build.sh --local"
mkdir -p dist
rsync -az "$REMOTE:$RDIR/dist/warp-lite.exe" "$REMOTE:$RDIR/dist/warp-remote.exe" "$REMOTE:$RDIR/dist/warp-lite-setup.exe" dist/
