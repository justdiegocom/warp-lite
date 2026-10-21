# Text brightness and font size (title-bar menu): a brightness level is applied to the default text at once, kept after
# a restart and followed by another window of the same Warp; a font size is applied and kept in the session. Then a
# Claude Code-like sample at every brightness level (screenshots brillo-<RRGGBB>.bmp) and with GDI ClearType text, and
# the "ink" of the text (sum of how much brighter than the background each pixel is) of DirectWrite against GDI.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$s = "$dir\session-brillo.txt"
$wins = "$dir\brillo-wins"
$sample = "$dir\brillo-sample.ps1"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", $wins, "$dir\brillo-*.bmp"
Remove-Item Env:\WARPLITE_FONT_URL, Env:\WARPLITE_DIR, Env:\WARPLITE_CHILD_SCRIPT, Env:\WARPLITE_GDI_TEXT -ErrorAction SilentlyContinue
$env:WARPLITE_SESSION = $s
function Run($steps, $timeout = 120000) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$(($steps -join '|').Replace('"', '\"'))`"" -PassThru
  if (-not $p.WaitForExit($timeout)) { $p.Kill(); 'TIMEOUT' }
}
function Bench($f) { @(Get-Content -Encoding UTF8 "$f.bench" -ErrorAction SilentlyContinue) }
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class BrilloPx {
  static int[] Load(string path, out int w, out int h) {
    using (var b = new Bitmap(path)) {
      w = b.Width; h = b.Height;
      var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
      var px = new int[w * h];
      for (int y = 0; y < h; y++) Marshal.Copy(d.Scan0 + y * d.Stride, px, y * w, w);
      b.UnlockBits(d);
      return px;
    }
  }
  // Longest horizontal run of pixels of exactly that color.
  public static int Run(string path, int rgb) {
    int w, h, best = 0;
    var px = Load(path, out w, out h);
    for (int y = 0; y < h; y++)
      for (int x = 0, run = 0; x < w; x++) {
        run = (px[y * w + x] & 0xffffff) == rgb ? run + 1 : 0;
        best = Math.Max(best, run);
      }
    return best;
  }
  // Ink of the rows y0..y1: how much brighter than the background (#050505) the pixels are, summed, in 1/255.
  public static long Ink(string path, int y0, int y1) {
    int w, h;
    var px = Load(path, out w, out h);
    long n = 0;
    for (int y = y0; y < Math.Min(y1, h); y++)
      for (int x = 0; x < w; x++) {
        int p = px[y * w + x], l = ((p >> 16 & 255) * 3 + (p >> 8 & 255) * 6 + (p & 255)) / 10;
        if (l > 5) n += l - 5;
      }
    return n;
  }
}
'@
# ASCII only (Windows PowerShell reads a .ps1 without BOM as ANSI): every other character by its code point.
@'
function U { -join ($args | ForEach-Object { [char]::ConvertFromUtf32($_) }) }
Write-Host ((U 0x256D) + (U 0x2500) * 44 + (U 0x256E))
Write-Host ((U 0x2502) + (' ' + (U 0x273B) + ' Welcome to Claude Code!').PadRight(44) + (U 0x2502))
Write-Host ((U 0x2570) + (U 0x2500) * 44 + (U 0x256F))
Write-Host '> explain how the terminal grid is drawn'
Write-Host ((U 0x25CF) + ' Each row is composed in a DIB with DirectWrite and copied with one BitBlt.')
Write-Host ('  ' + (U 0x23BF) + '  Read src/main.cpp (6217 lines)')
Write-Host 'The quick brown fox jumps over the lazy dog 0123456789 {}[]()<>=;:,.'
'@ | Set-Content -Encoding ASCII $sample

# 1. Default level, a level chosen and applied (a box drawing line in exactly that color), a font size chosen.
Run @('move:40,40,1100,700', 'wait', 'wait', 'wait', 'brightness', 'font-size', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$sample')", 'submit', 'wait',
      'brightness:AAAAAA', 'wait', 'brightness', "shot:$dir\brillo-applied.bmp", 'font-size:18', 'font-size', 'quit')
$b = Bench $s
$b | ForEach-Object { "  $_" }
Check ($b[0] -eq 'brightness: DDDDDD') "default level DDDDDD ($($b[0]))"
Check ($b[1] -like 'font-size: 13 *') "default font size 13 px ($($b[1]))"
Check ($b[2] -eq 'brightness: AAAAAA') "level chosen: $($b[2])"
$run = if (Test-Path "$dir\brillo-applied.bmp") { [BrilloPx]::Run("$dir\brillo-applied.bmp", 0xAAAAAA) } else { 0 }
Check ($run -ge 300) "applied to the default text: a line of $run px in #AAAAAA"
Check ($b[3] -like 'font-size: 18 *' -and $b[3] -ne $b[1]) "font size chosen, cell changed ($($b[3]))"
Check ((Get-Content "$s.brightness" -ErrorAction SilentlyContinue) -eq 'AAAAAA') 'level saved next to the session'

# 2. After a restart: both kept.
Remove-Item -Force -ErrorAction SilentlyContinue "$s.bench"
Run @('wait', 'wait', 'brightness', 'font-size', 'quit')
$b = Bench $s
Check ($b[0] -eq 'brightness: AAAAAA') "level kept after a restart ($($b[0]))"
Check ($b[1] -like 'font-size: 18 *') "font size kept after a restart ($($b[1]))"

# 3. Two windows of the same Warp (WARPLITE_DIR, like the real one): the other one follows the level.
Remove-Item Env:\WARPLITE_SESSION
$env:WARPLITE_DIR = $wins
$w = @('wait') * 12
$p2 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$((@('wait', 'wait', 'wait', 'brightness') + $w + @('brightness', 'quit')) -join '|')`"" -PassThru
$recv = $null
for ($t = 0; $t -lt 60 -and -not $recv; $t++) {
  Start-Sleep -Milliseconds 500
  $recv = Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Where-Object { (Get-Content $_.FullName -ErrorAction SilentlyContinue) -match '^brightness:' } | Select-Object -First 1
}
$p1 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|brightness:EEEEEE|brightness|quit`"" -PassThru
if (-not $p1.WaitForExit(90000)) { $p1.Kill(); 'TIMEOUT 1' }
if (-not $p2.WaitForExit(90000)) { $p2.Kill(); 'TIMEOUT 2' }
Remove-Item Env:\WARPLITE_DIR
$rb = if ($recv) { @(Get-Content -Encoding UTF8 $recv.FullName) } else { @() }
$rb | ForEach-Object { "  other: $_" }
Check ($rb[0] -eq 'brightness: DDDDDD' -and $rb[1] -eq 'brightness: EEEEEE') 'the other window goes from DDDDDD to EEEEEE'
Check ((Get-Content "$wins\brightness.txt" -ErrorAction SilentlyContinue) -eq 'EEEEEE') 'level saved in the instance folder'

# 4. The sample at every level, then with GDI ClearType text at CCCCCC and DDDDDD: ink of the text rows.
$env:WARPLITE_SESSION = $s
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*"
$levels = 'AAAAAA', 'CCCCCC', 'DDDDDD', 'EEEEEE', 'FFFFFF'
$steps = @('move:40,40,1100,700', 'wait', 'wait', 'wait', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$sample')", 'submit', 'wait', 'font')
foreach ($l in $levels) { $steps += @("brightness:$l", 'wait', "shot:$dir\brillo-$l.bmp") }
Run ($steps + @('quit'))
$font = "$(Bench $s)"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*"
$env:WARPLITE_GDI_TEXT = '1'
Run @('move:40,40,1100,700', 'wait', 'wait', 'wait', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$sample')", 'submit', 'wait',
      'brightness:CCCCCC', 'wait', "shot:$dir\brillo-gdi-CCCCCC.bmp", 'brightness:DDDDDD', 'wait', "shot:$dir\brillo-gdi-DDDDDD.bmp", 'quit')
Remove-Item Env:\WARPLITE_GDI_TEXT
"  " + ($font -replace '^.*(dwrite: .*)$', '$1')
foreach ($l in $levels + @('gdi-CCCCCC', 'gdi-DDDDDD')) {
  if (Test-Path "$dir\brillo-$l.bmp") { "  ink {0,-11} {1,9}" -f $l, [BrilloPx]::Ink("$dir\brillo-$l.bmp", 40, 300) } else { Check $false "screenshot $l" }
}
Check (-not (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$dir\warp-lite.exe" })) 'no test processes alive'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", $wins, $sample
"fallos: $fail"
