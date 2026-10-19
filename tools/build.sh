#!/usr/bin/env bash
# Compila better-warp.exe (x86_64 Windows) con zig. Se ejecuta en el Mac:
#   tools/build.sh            -> desde este box: copia al Mac, compila allí y trae dist/better-warp.exe
#   tools/build.sh --local    -> compila aquí mismo (necesita zig en el PATH)
set -euo pipefail
cd "$(dirname "$0")/.."
REMOTE=mac
RDIR='~/agent-work/better-warp-build'

build_here() {
  ZIG=${ZIG:-$(command -v zig || echo /opt/homebrew/bin/zig)}
  T=x86_64-windows-gnu
  mkdir -p build dist
  objs=()
  for c in third_party/libvterm/src/*.c; do
    o=build/$(basename "$c" .c).o
    "$ZIG" cc -target $T -O2 -std=c99 -Ithird_party/libvterm/include -c "$c" -o "$o" &
    objs+=("$o")
  done
  "$ZIG" c++ -target $T -O2 -std=c++17 -municode -Ithird_party/libvterm/include \
    -c src/main.cpp -o build/main.o &
  wait
  # zig ignora -mwindows: sin --subsystem,windows el .exe abre una consola al arrancar.
  "$ZIG" c++ -target $T -O2 -municode -Wl,--subsystem,windows build/main.o "${objs[@]}" \
    -lgdi32 -luser32 -lkernel32 -lshell32 -static -o dist/better-warp.exe
  python3 - <<'PY'
import struct, sys
d = open('dist/better-warp.exe', 'rb').read()
pe = struct.unpack('<I', d[0x3c:0x40])[0]
sub = struct.unpack('<H', d[pe + 24 + 68:pe + 24 + 70])[0]
if sub != 2:
    sys.exit(f'better-warp.exe no es una app de ventana (subsistema {sub})')
PY
  ls -l dist/better-warp.exe
}

if [[ "${1:-}" == "--local" ]]; then build_here; exit; fi
ssh -o BatchMode=yes "$REMOTE" "mkdir -p $RDIR"
rsync -az --delete --exclude .git --exclude dist --exclude build ./ "$REMOTE:$RDIR/"
ssh -o BatchMode=yes "$REMOTE" "cd $RDIR && PATH=/opt/homebrew/bin:\$PATH bash tools/build.sh --local"
mkdir -p dist
rsync -az "$REMOTE:$RDIR/dist/better-warp.exe" dist/
