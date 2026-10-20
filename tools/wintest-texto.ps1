# Terminal text (DirectWrite, like Windows Terminal): a sample with regular, bold, italic, colors, box drawing,
# Nerd Font icons, emoji and CJK in Hack (embedded), Consolas, Cascadia Mono and JetBrains Mono (downloaded) and
# Courier New (other installed); screenshots texto-<tag>-<font>.bmp, the face DirectWrite really draws each style
# with, the row painting check, and drawing time and CPU with a flood of text (like Claude Code streaming).
# -Exe and -Tag run it with another build (the previous one, to compare).
param([string]$Exe = "$PSScriptRoot\warp-lite.exe", [string]$Tag = 'new')
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$s = "$dir\session-texto-$Tag.txt"
$sample = "$dir\texto-sample.ps1"
$flood = "$dir\texto-flood.ps1"
$done = "$dir\texto-flood-$Tag.txt"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", "$dir\texto-$Tag-*.bmp", $done
Remove-Item Env:\WARPLITE_FONT_URL, Env:\WARPLITE_DIR, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
$env:WARPLITE_SESSION = $s
function Start-Warp($steps) {
  Start-Process -FilePath $Exe -ArgumentList "--script `"$(($steps -join '|').Replace('"', '\"'))`"" -PassThru
}
function Bench { @(Get-Content -Encoding UTF8 "$s.bench" -ErrorAction SilentlyContinue) }
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }
# ASCII only (Windows PowerShell reads a .ps1 without BOM as ANSI): every other character by its code point.
@'
$e = [char]27
function U { -join ($args | ForEach-Object { [char]::ConvertFromUtf32($_) }) }
Write-Host "Regular 0O1lI {}[]=> $e[1mBold 0O1lI {}[]=>$e[0m $e[3mItalic 0O1lI$e[0m $e[1;3mBold Italic$e[0m"
Write-Host "$e[31mred $e[32mgreen $e[33myellow $e[34mblue $e[35mmagenta $e[36mcyan $e[91mbright $e[94mblue $e[38;2;255;140;0mtruecolor$e[0m $e[7m inverse $e[0m $e[4munderline$e[0m $e[9mstrike$e[0m $e[2mdim$e[0m"
Write-Host ((U 0x250C 0x2500 0x2500 0x2500 0x2500 0x252C 0x2500 0x2500 0x2500 0x2500 0x2510) + '  ' + (U 0x2591 0x2592 0x2593 0x2588 0x2580 0x2584 0x258C 0x2590) + '  ' + (U 0x256D 0x2500 0x2500 0x256E))
Write-Host ((U 0x2502) + ' box' + (U 0x2502) + ' ab ' + (U 0x2502) + '  ' + (U 0x2554 0x2550 0x2550 0x2557) + '  ' + (U 0x2570 0x2500 0x2500 0x256F))
Write-Host ((U 0x2514 0x2500 0x2500 0x2500 0x2500 0x2534 0x2500 0x2500 0x2500 0x2500 0x2518) + '  ' + (U 0x255A 0x2550 0x2550 0x255D))
Write-Host ((U 0x23BF) + ' Claude ' + (U 0x273B) + '  nerd: ' + (U 0xE0B0 0x20 0xF115 0x20 0xE718 0x20 0xF0A1E) + '  emoji: ' + (U 0x1F600 0x20 0x1F680 0x20 0x26A0 0xFE0F) + '  CJK: ' + (U 0x6F22 0x5B57 0x304B 0x306A 0xD55C 0xAE00) + '  https://example.com/x')
'@ | Set-Content -Encoding ASCII $sample
@'
$e = [char]27
Set-Content -Path $args[0] -Value 'start'
$w = 'const result = await client.messages.create({ model, max_tokens: 1024, messages }); // streaming'
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt 100000; $i++) { [Console]::Out.WriteLine("$e[32m$i$e[0m  $w $e[1mbold$e[0m $e[36m$i$e[0m") }
Set-Content -Path $args[0] -Value $sw.ElapsedMilliseconds
'@ | Set-Content -Encoding ASCII $flood

# 1. The sample in each font: a screenshot, the font report and, at the end, row painting against a full paint.
# Then, in Hack, a command in the input box (green: it exists) and cmd running with the block cursor.
$fonts = 'Hack', 'Consolas', 'Cascadia Mono', 'JetBrains Mono', 'Courier New'
$steps = @('move:40,40,1400,700', 'wait', 'wait', 'wait', 'clear', "edit:Invoke-Expression (Get-Content -Raw '$sample')", 'submit', 'wait')
foreach ($f in $fonts) { $steps += @("font:$f", 'font-wait', 'font', "shot:$dir\texto-$Tag-$($f.Replace(' ', '')).bmp") }
$steps += @('font:Hack', 'edit:Get-ChildItem -Name C:\Windows', "shot:$dir\texto-$Tag-editor.bmp", 'clear', 'edit:cmd /k ver', 'submit', 'type:dir', 'wait', "shot:$dir\texto-$Tag-cmd.bmp")
$p = Start-Warp ($steps + @('prime', 'wait', 'verify', 'quit'))
if (-not $p.WaitForExit(300000)) { $p.Kill(); 'TIMEOUT' }
$b = Bench
$b | ForEach-Object { "  $_" }
for ($i = 0; $i -lt $fonts.Count; $i++) {
  $l = "$($b[$i])"
  Check ($l -like "font: ``[$($fonts[$i])``]*") "$($fonts[$i]): GDI la encuentra"
  if ($l -match ' dwrite: (.*?)(?: error=\[|$)') {
    $st = $Matches[1] -split ' / '
    Check ($st[1] -like "$($fonts[$i]) *" -and $st[2] -like "$($fonts[$i]) *") "$($fonts[$i]): DirectWrite dibuja con su propia cara ($($st[1]); $($st[2]))"
    Check ($st[2] -match ' (7\d\d|\d+ \+bold)$' -and $st[3] -match 'italic|\+oblique' -and $st[4] -match 'italic|\+oblique') "$($fonts[$i]): negrita y cursiva ($($st[2]); $($st[3]); $($st[4]))"
    Check ($st[0] -like 'natural*') "$($fonts[$i]): modo $($st[0])"
  }
}
Check ("$($b[$fonts.Count])" -match ': 0$') "pintado por filas = completo ($($b[$fonts.Count]))"
Get-ChildItem "$dir\texto-$Tag-*.bmp" | ForEach-Object { "  $($_.Name) $($_.Length)" }

# 2. A flood of text in a big window: CPU of warp-lite while it lasts, row painting during it, and the drawing
# time of a screen full of text afterwards.
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
"FLOOD: 100000 lines in $(Get-Content $done) ms; warp-lite $([math]::Round($cpu)) ms CPU in $wall ms ($([math]::Round(100 * $cpu / [math]::Max(1, $wall), 1)) % of a core)"
if (-not $p.WaitForExit(120000)) { $p.Kill(); 'TIMEOUT' }
$b = Bench
$b | ForEach-Object { "  $_" }
Check ("$($b[0])" -match ': 0$') 'pintado por filas = completo durante el chorro de texto'
Check (-not (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe })) 'sin procesos de prueba vivos'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", $sample, $flood, $done
"fallos: $fail"
