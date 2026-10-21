# Builtin glyphs and color emoji, like Windows Terminal: box drawing, blocks and the powerline separators are drawn over
# the whole cell, so lines join across rows and columns whatever the font and the line height, and emoji come in color.
# A Claude Code-style box, a table with light, heavy and double lines, every box drawing glyph, dashes, diagonals,
# blocks, shades, colors, powerline, emoji and CJK in Hack, Cascadia Mono and JetBrains Mono (downloaded) at 100 and
# 125 %: screenshots cajas-<tag>-<font>-<zoom>.bmp, the cells of the box and the emoji (alignment), the longest
# unbroken line, a selection and a find highlight over the emoji, emoji color, row painting against a full paint, the
# same with GDI text (WARPLITE_GDI_TEXT), and drawing time and CPU with a Claude Code-like stream.
# -Exe and -Tag run it with another build (the previous one, to compare).
param([string]$Exe = "$PSScriptRoot\warp-lite.exe", [string]$Tag = 'new')
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$s = "$dir\session-cajas-$Tag.txt"
$sample = "$dir\cajas-sample.ps1"
$emoji = "$dir\cajas-emoji.ps1"
$flood = "$dir\cajas-flood.ps1"
$done = "$dir\cajas-flood-$Tag.txt"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", "$dir\cajas-$Tag-*.bmp", $done
Remove-Item Env:\WARPLITE_FONT_URL, Env:\WARPLITE_DIR, Env:\WARPLITE_CHILD_SCRIPT, Env:\WARPLITE_GDI_TEXT -ErrorAction SilentlyContinue
$env:WARPLITE_SESSION = $s
function Start-Warp($steps) {
  Start-Process -FilePath $Exe -ArgumentList "--script `"$(($steps -join '|').Replace('"', '\"'))`"" -PassThru
}
function Bench { @(Get-Content -Encoding UTF8 "$s.bench" -ErrorAction SilentlyContinue) }
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class CajasPx {
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
  // Longest run of pixels of exactly that color, down a column or along a row.
  public static int Run(string path, int rgb, bool vertical) {
    int w, h, best = 0;
    var px = Load(path, out w, out h);
    for (int i = 0; i < (vertical ? w : h); i++)
      for (int j = 0, run = 0; j < (vertical ? h : w); j++) {
        int p = vertical ? px[j * w + i] : px[i * w + j];
        run = (p & 0xffffff) == rgb ? run + 1 : 0;
        best = Math.Max(best, run);
      }
    return best;
  }
  // Warm pixels (yellow, orange, red, skin): emoji in color; gray glyphs, the blue accent and green have none.
  public static int Warm(string path) {
    int w, h, n = 0;
    foreach (int p in Load(path, out w, out h)) {
      int r = p >> 16 & 255, b = p & 255;
      if (r >= 180 && b <= 110 && r - b >= 100) n++;
    }
    return n;
  }
}
'@
# ASCII only (Windows PowerShell reads a .ps1 without BOM as ANSI): every other character by its code point.
@'
$e = [char]27
function U { -join ($args | ForEach-Object { [char]::ConvertFromUtf32($_) }) }
function L($c, $n) { (U $c) * $n }
function T($l, $f, $m, $r) { (U $l) + (L $f 4) + (U $m) + (L $f 4) + (U $r) }
function C($v, $a, $b) { (U $v) + $a + (U $v) + $b + (U $v) }
Write-Host ((U 0x256D) + (L 0x2500 44) + (U 0x256E))
foreach ($t in @((' ' + (U 0x273B) + ' Welcome to Claude Code!'), '', '   /help for help, /status for your setup', '   cwd: C:\Users\diego\work')) {
  Write-Host ((U 0x2502) + $t.PadRight(44) + (U 0x2502))
}
Write-Host ((U 0x2570) + (L 0x2500 44) + (U 0x256F))
Write-Host (@((T 0x250F 0x2501 0x2533 0x2513), (T 0x2554 0x2550 0x2566 0x2557), (T 0x250C 0x2500 0x252C 0x2510), (T 0x2552 0x2550 0x2564 0x2555), (T 0x2553 0x2500 0x2565 0x2556), (T 0x250D 0x2501 0x252F 0x2511)) -join '  ')
Write-Host (@((C 0x2503 ' ab ' ' cd '), (C 0x2551 ' ab ' ' cd '), (C 0x2502 ' ab ' ' cd '), (C 0x2502 ' ab ' ' cd '), (C 0x2551 ' ab ' ' cd '), (C 0x2502 ' ab ' ' cd ')) -join '  ')
Write-Host (@((T 0x2523 0x2501 0x254B 0x252B), (T 0x2560 0x2550 0x256C 0x2563), (T 0x251C 0x2500 0x253C 0x2524), (T 0x255E 0x2550 0x256A 0x2561), (T 0x255F 0x2500 0x256B 0x2562), (T 0x251D 0x2501 0x253F 0x2525)) -join '  ')
Write-Host (@((C 0x2503 ' ef ' ' gh '), (C 0x2551 ' ef ' ' gh '), (C 0x2502 ' ef ' ' gh '), (C 0x2502 ' ef ' ' gh '), (C 0x2551 ' ef ' ' gh '), (C 0x2502 ' ef ' ' gh ')) -join '  ')
Write-Host (@((T 0x2517 0x2501 0x253B 0x251B), (T 0x255A 0x2550 0x2569 0x255D), (T 0x2514 0x2500 0x2534 0x2518), (T 0x2558 0x2550 0x2567 0x255B), (T 0x2559 0x2500 0x2568 0x255C), (T 0x2515 0x2501 0x2537 0x2519)) -join '  ')
for ($r = 0; $r -lt 4; $r++) { Write-Host ((0..31 | ForEach-Object { U (0x2500 + 32 * $r + $_) }) -join ' ') }
Write-Host (((0..31 | ForEach-Object { U (0x2580 + $_) }) -join ' ') + '   ' + ((0..15 | ForEach-Object { U (0xE0B0 + $_) }) -join ' '))
Write-Host ((L 0x2504 6) + '  ' + (L 0x2505 6) + '  ' + (L 0x2508 6) + '  ' + (L 0x2509 6) + '  ' + (L 0x254C 6) + '  ' + (L 0x254D 6) + '  ' + (U 0x2571 0x2572 0x2571 0x2572) + '  ' + (U 0x2573 0x2573))
foreach ($i in 0, 1) { Write-Host ((U 0x2506 0x20 0x2507 0x20 0x250A 0x20 0x250B 0x20 0x254E 0x20 0x254F) + (' ' * 60) + $(if ($i) { U 0x2571 0x2572 0x2571 0x2572 } else { U 0x2572 0x2571 0x2572 0x2571 })) }
foreach ($i in 0, 1) { Write-Host ((L 0x2591 8) + (L 0x2592 8) + (L 0x2593 8) + (L 0x2588 8) + '  ' + (U 0x2580 0x2584 0x2580 0x2584 0x258C 0x2590 0x258C 0x2590 0x2596 0x2597 0x2598 0x259D)) }
Write-Host ("$e[7m" + (T 0x250C 0x2500 0x252C 0x2510) + "$e[0m $e[2m" + (L 0x2500 8) + "$e[0m $e[1m" + (L 0x2501 8) + "$e[0m $e[31m" + (L 0x2550 8) + "$e[0m $e[33;44m" + (U 0x256C 0x256C 0x2591 0x2592 0x2593 0x256D 0x256E) + "$e[0m $e[38;2;255;140;0m" + (U 0x2570 0x2500 0x2500 0x256F) + "$e[0m")
Write-Host ("$e[30;44m main $e[34;42m" + (U 0xE0B0) + "$e[30m feat $e[32;49m" + (U 0xE0B0) + "$e[0m  " + (U 0xE0B2) + "$e[30;45m left $e[0;35m" + (U 0xE0B4) + "$e[0m  " + (U 0xE0B1 0x20 0xE0B3 0x20 0xE0B5 0x20 0xE0B7))
Write-Host ('emoji: ' + (U 0x1F600) + ' ' + (U 0x1F680) + ' ' + (U 0x2705) + ' ' + (U 0x2764 0xFE0F) + ' ' + (U 0x1F44D 0x1F3FD) + ' ' + (U 0x1F468 0x200D 0x1F469 0x200D 0x1F467) + ' ' + (U 0x1F469 0x200D 0x1F4BB) + ' ' + (U 0x1F3F3 0xFE0F 0x200D 0x1F308) + ' ' + (U 0x1F1EA 0x1F1F8) + ' | CJK: ' + (U 0x6F22 0x5B57 0x304B 0x306A 0xD55C 0xAE00) + ' end')
'@ | Set-Content -Encoding ASCII $sample
@'
function U { -join ($args | ForEach-Object { [char]::ConvertFromUtf32($_) }) }
Write-Host ('emoji: ' + (U 0x1F600) + ' ' + (U 0x1F680) + ' ' + (U 0x2705) + ' ' + (U 0x2764 0xFE0F) + ' ' + (U 0x1F44D 0x1F3FD) + ' ' + (U 0x1F468 0x200D 0x1F469 0x200D 0x1F467) + ' ' + (U 0x1F469 0x200D 0x1F4BB) + ' ' + (U 0x1F3F3 0xFE0F 0x200D 0x1F308) + ' ' + (U 0x1F1EA 0x1F1F8) + ' ' + (U 0x2B50 0x20 0x26A1 0x20 0x26A0 0xFE0F))
'@ | Set-Content -Encoding ASCII $emoji
@'
$e = [char]27
function U { -join ($args | ForEach-Object { [char]::ConvertFromUtf32($_) }) }
Set-Content -Path $args[0] -Value 'start'
$top = (U 0x256D) + (U 0x2500) * 76 + (U 0x256E)
$bot = (U 0x2570) + (U 0x2500) * 76 + (U 0x256F)
$v = U 0x2502
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt 20000; $i++) {
  [Console]::Out.WriteLine("$e[38;2;215;119;87m" + (U 0x25CF) + "$e[0m Update(src/main.cpp) $i")
  [Console]::Out.WriteLine('  ' + (U 0x23BF) + "  Updated src/main.cpp with $e[1m12$e[0m additions and $e[1m3$e[0m removals")
  [Console]::Out.WriteLine($top)
  [Console]::Out.WriteLine("$v $e[32m+   const result = await client.messages.create({ model, max_tokens: 1024 });$e[0m $v")
  [Console]::Out.WriteLine($bot)
}
Set-Content -Path $args[0] -Value $sw.ElapsedMilliseconds
'@ | Set-Content -Encoding ASCII $flood

# 1. The sample in each font at 100 and 125 %: the font report, a screenshot, and in Hack the cells; then a find
# highlight on the check mark and a selection from "emoji" to the end of its row (after the find: with a selection,
# find searches for it), row painting and the drawing time.
$fonts = 'Hack', 'Cascadia Mono', 'JetBrains Mono'
$steps = @('move:40,40,1500,1000', 'wait', 'wait', 'wait', 'brightness', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$sample')", 'submit', 'wait', 'cells')
foreach ($f in $fonts) { foreach ($z in 100, 125) { $steps += @("font:$f", 'font-wait', "zoom:$z", 'wait', 'font', "shot:$dir\cajas-$Tag-$($f.Replace(' ', ''))-$z.bmp") } }
$steps += @('font:Hack', 'zoom:100', 'wait', "find:$([char]0x2705)", 'select:emoji', 'wait', "shot:$dir\cajas-$Tag-selfind.bmp", 'find-close', 'prime', 'wait', 'verify', 'bench', 'quit')
$p = Start-Warp $steps
if (-not $p.WaitForExit(300000)) { $p.Kill(); 'TIMEOUT' }
$b = Bench
# The default text color: lines in it are measured below (builds without the brightness step: #CCCCCC).
$fg = if ("$($b | Where-Object { $_ -like 'brightness: *' })" -match 'brightness: ([0-9A-F]{6})') { [Convert]::ToInt32($Matches[1], 16) } else { 0xCCCCCC }
$cells = @{}
foreach ($l in $b) {
  if ($l -match '^cells (\d+): (.*)$') {
    $row = @{}
    foreach ($c in $Matches[2].Trim() -split ' ') { $kv = $c -split ':', 2; $row[[int]$kv[0]] = $kv[1] }
    $cells[[int]$Matches[1]] = $row
  } else { "  $l" }
}
# The box: corners and sides in the same columns on every row.
$top = $cells.Keys | Sort-Object | Where-Object { $cells[$_].Values -contains '256D/1' } | Select-Object -First 1
if ($null -ne $top) {
  $c0 = ($cells[$top].GetEnumerator() | Where-Object { $_.Value -eq '256D/1' } | Select-Object -First 1).Key
  $c1 = ($cells[$top].GetEnumerator() | Where-Object { $_.Value -eq '256E/1' } | Select-Object -First 1).Key
  $ok = $c1 - $c0 -eq 45
  foreach ($r in ($top + 1)..($top + 4)) { $ok = $ok -and $cells[$r][$c0] -eq '2502/1' -and $cells[$r][$c1] -eq '2502/1' }
  $ok = $ok -and $cells[$top + 5][$c0] -eq '2570/1' -and $cells[$top + 5][$c1] -eq '256F/1'
  Check $ok "box in the same columns on its 6 rows (columns $c0 and $c1)"
} else { Check $false 'box found in the cells' }
$er = $cells.Keys | Where-Object { $cells[$_].Values -contains '1F600/2' } | Select-Object -First 1
if ($null -ne $er) { "  emoji row: " + (($cells[$er].GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key):$($_.Value)" }) -join ' ') }
$fr = @($b | Where-Object { $_ -like 'font: *' })
for ($i = 0; $i -lt $fr.Count; $i++) {
  $f = $fonts[[math]::Floor($i / 2)]
  $z = @(100, 125)[$i % 2]
  $bmp = "$dir\cajas-$Tag-$($f.Replace(' ', ''))-$z.bmp"
  if ($fr[$i] -match '\] (\d+)x(\d+)' -and (Test-Path $bmp)) {
    $cw = [int]$Matches[1]; $ch = [int]$Matches[2]
    $vr = [CajasPx]::Run($bmp, $fg, $true); $hr = [CajasPx]::Run($bmp, $fg, $false)
    Check ($fr[$i] -like "font: ``[$f``]*") "${f}: GDI finds it"
    Check ($vr -ge 4 * $ch) "$f $z %: cell ${cw}x$ch, longest vertical line $vr px, horizontal $hr px (unbroken across rows: >= $(4 * $ch))"
  } else { Check $false "$f ${z}: font report and screenshot" }
}
Check ("$($b | Where-Object { $_ -like 'p*xeles distintos*' })" -match ': 0$') 'row painting = full painting'
"  " + ($b | Where-Object { $_ -like '*completo*' })

# 2. Only emoji, in Hack at 100 %: warm pixels, and the emoji face really drawing in color.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*"
$p = Start-Warp @('move:40,40,1500,1000', 'wait', 'wait', 'wait', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$emoji')", 'submit', 'wait', 'font', "shot:$dir\cajas-$Tag-emoji.bmp", 'join:1F44D,1F3FD', 'join:1F469,200D,1F4BB', 'join:1F468,200D,1F469,200D,1F467', 'join:1F1EA,1F1F8', 'quit')
if (-not $p.WaitForExit(120000)) { $p.Kill(); 'TIMEOUT' }
$warm = if (Test-Path "$dir\cajas-$Tag-emoji.bmp") { [CajasPx]::Warm("$dir\cajas-$Tag-emoji.bmp") } else { 0 }
Check ("$(Bench)" -like '*emoji: color*') "DirectWrite draws Segoe UI Emoji in color ($("$(Bench)" -replace '^.* / (emoji: \w+).*$', '$1'))"
Check ($warm -ge 300) "emoji in color: $warm warm pixels"
Bench | Where-Object { $_ -like 'join *' } | ForEach-Object { "  $_" }
Check ("$(Bench | Where-Object { $_ -like 'join 1F44D,1F3FD:*' })" -match 'joined=[1-9]') 'thumbs up with a skin tone: one glyph'
Check ("$(Bench | Where-Object { $_ -like 'join 1F469,200D,1F4BB:*' })" -match 'joined=[1-9]') 'woman technologist (ZWJ): one glyph'

# 3. GDI text (WARPLITE_GDI_TEXT): the sample, row painting and the builtin glyphs joining too.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*"
$env:WARPLITE_GDI_TEXT = '1'
$p = Start-Warp @('move:40,40,1500,1000', 'wait', 'wait', 'wait', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$sample')", 'submit', 'wait', 'font', "shot:$dir\cajas-$Tag-gdi.bmp", 'prime', 'wait', 'verify', 'quit')
if (-not $p.WaitForExit(120000)) { $p.Kill(); 'TIMEOUT' }
Remove-Item Env:\WARPLITE_GDI_TEXT
$b = Bench
Check ("$($b[0])" -like '* dwrite: off*') 'GDI text'
if ("$($b[0])" -match '\] (\d+)x(\d+)' -and (Test-Path "$dir\cajas-$Tag-gdi.bmp")) {
  $ch = [int]$Matches[2]; $vr = [CajasPx]::Run("$dir\cajas-$Tag-gdi.bmp", $fg, $true)
  Check ($vr -ge 4 * $ch) "GDI: longest vertical line $vr px (unbroken across rows: >= $(4 * $ch))"
}
Check ("$($b[1])" -match ': 0$') "GDI: row painting = full painting ($($b[1]))"

# 4. A Claude Code-like stream (boxes, bullets, colored diff) in a big window: CPU of warp-lite while it lasts, row
# painting during it, and the drawing time of the screen it leaves.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*"
$waits = @('wait') * 25
$p = Start-Warp (@('move:0,0,1920,1080', 'wait', 'wait', 'wait', "edit:& ([scriptblock]::Create((Get-Content -Raw '$flood'))) '$done'", 'submit', 'prime', 'wait', 'verify') + $waits + @('bench', 'quit'))
for ($t = 0; $t -lt 600 -and -not (Test-Path $done); $t++) { Start-Sleep -Milliseconds 50 }
$cpu0 = $p.TotalProcessorTime.TotalMilliseconds
$w0 = [Diagnostics.Stopwatch]::StartNew()
for ($t = 0; $t -lt 1200 -and "$(Get-Content $done -ErrorAction SilentlyContinue)" -notmatch '^\d+$'; $t++) { Start-Sleep -Milliseconds 50 }
$p.Refresh()
$cpu = $p.TotalProcessorTime.TotalMilliseconds - $cpu0
$wall = $w0.ElapsedMilliseconds
"CLAUDE-LIKE STREAM: 100000 lines in $(Get-Content $done) ms; warp-lite $([math]::Round($cpu)) ms CPU in $wall ms ($([math]::Round(100 * $cpu / [math]::Max(1, $wall), 1)) % of a core)"
if (-not $p.WaitForExit(120000)) { $p.Kill(); 'TIMEOUT' }
$b = Bench
$b | ForEach-Object { "  $_" }
Check ("$($b[0])" -match ': 0$') 'row painting = full painting during the stream'
Check (-not (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe })) 'no test processes alive'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", $sample, $emoji, $flood, $done
Get-ChildItem "$dir\cajas-$Tag-*.bmp" | ForEach-Object { "  $($_.Name) $($_.Length)" }
"fallos: $fail"
