# Compila warp-lite.exe y warp-lite-setup.exe en Windows con el zig de fp-tools (equivale a
# tools/build.sh --local; se usa cuando el Mac está ocupado). Se ejecuta desde la raíz del repo.
$ErrorActionPreference = 'Stop'
$zig = "$env:LOCALAPPDATA\fp-tools\zig\zig.exe"
$T = 'x86_64-windows-gnu'
New-Item -ItemType Directory -Force build, dist | Out-Null
$objs = @()
foreach ($c in Get-ChildItem third_party\libvterm\src\*.c) {
  $o = "build\$($c.BaseName).o"
  & $zig cc -target $T -O2 -std=c99 -Ithird_party/libvterm/include -c $c.FullName -o $o
  if ($LASTEXITCODE) { throw "falló $($c.Name)" }
  $objs += $o
}
& $zig c++ -target $T -O2 -std=c++17 -c third_party/qrcodegen/qrcodegen.cpp -o build/qrcodegen.o
if ($LASTEXITCODE) { throw 'falló qrcodegen.cpp' }
$objs += 'build\qrcodegen.o'
& $zig c++ -target $T -O2 -std=c++17 -municode -Ithird_party/libvterm/include -Ithird_party/qrcodegen -c src/main.cpp -o build/main.o
if ($LASTEXITCODE) { throw 'falló main.cpp' }
& $zig c++ -target $T -O2 -municode '-Wl,--subsystem,windows' build/main.o @objs src/warp-lite.rc `
  -lgdi32 -luser32 -lkernel32 -lshell32 -lole32 -luuid -lwindowscodecs -ladvapi32 -lwinhttp -lbcrypt -lrstrtmgr -static -o dist/warp-lite.exe
if ($LASTEXITCODE) { throw 'falló el enlace de warp-lite.exe' }
# The installer also carries dist\warp-remote.exe (Rust, cross-compiled by tools/build.sh): this script doesn't build it.
if (-not (Test-Path dist\warp-remote.exe)) { throw 'falta dist\warp-remote.exe: compílalo con tools/build.sh (cargo zigbuild) y cópialo a dist' }
# La huella de los .exe entra en la clave de caché de zig (que no sigue el .incbin).
$sha = [Security.Cryptography.SHA1]::Create()
$both = [IO.File]::ReadAllBytes("$PWD\dist\warp-lite.exe") + [IO.File]::ReadAllBytes("$PWD\dist\warp-remote.exe")
$stamp = [BitConverter]::ToString($sha.ComputeHash([byte[]]$both)).Replace('-', '').Substring(0, 16)
& $zig c++ -target $T -O2 -std=c++17 -municode "-DWARP_LITE_STAMP=$stamp" '-Wl,--subsystem,windows' src/setup.cpp src/warp-lite.rc `
  -lole32 -lshell32 -luuid -ladvapi32 -luser32 -static -o dist/warp-lite-setup.exe
if ($LASTEXITCODE) { throw 'falló warp-lite-setup.exe' }
Get-Item dist\warp-lite.exe, dist\warp-remote.exe, dist\warp-lite-setup.exe | ForEach-Object { "$($_.Name) $($_.Length)" }
