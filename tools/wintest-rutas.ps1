# Paths as links (Ctrl+hover / Ctrl+click, like URLs): the quoted path Claude Code wrapped over three rows
# (spaces dropped at the breaks, 2-space indent, no closing quote) with every row resolving to the whole
# path, an unquoted path with spaces, paths relative to the pane folder, src\main.cpp:120, file.txt:12:3,
# a folder, a missing path, a path followed by . or ), file:///, a path the terminal wrapped mid-word, a
# URL that stays a URL, and what Ctrl+click would do (open / explorer / select; nothing is opened). Also
# how long a hover takes with the path cache empty and full. Results in session-rutas.txt.bench.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_SESSION = "$dir\session-rutas.txt"
$t = "$dir\rutas"
$sample = "$dir\rutas-sample.txt"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$env:WARPLITE_SESSION*", $t, $sample
$shotDir = "$t\Diego Rodriguez\Pictures\Zas"
New-Item -ItemType Directory -Force $shotDir, "$t\Mis Notas", "$t\src" | Out-Null
$shot = "$shotDir\Screenshot 2026-10-21 14.24.19.png"
foreach ($f in $shot, "$t\Mis Notas\nota final.txt", "$t\src\main.cpp", "$t\file.txt", "$t\tool.exe", "$t\run.ps1") {
  Set-Content -Encoding ASCII $f 'x'
}
# What the pane shows after cls (one row each). Row 0-2 exactly like Claude Code's prompt echo.
$q = [string][char]0x276F
$fileUrl = 'file:///' + (($t -replace '\\', '/') -replace ' ', '%20') + '/file.txt'
$lines = @(
  "$q `"$t\Diego",                                   # 0
  '  Rodriguez\Pictures\Zas\Screenshot',              # 1
  '  2026-10-21 14.24.19.png',                        # 2
  "Abre $t\Mis Notas\nota final.txt ahora",           # 3
  'Mira src\main.cpp:120 luego',                      # 4
  'error en file.txt:12:3 aqui',                      # 5
  "Carpeta $t\src",                                   # 6
  "Falta $t\no-existe.txt",                           # 7
  "Guardado en $t\file.txt.",                         # 8
  '(ver src\main.cpp)',                               # 9
  "Lanza $t\tool.exe o run.ps1 ya",                   # 10
  "Abre $fileUrl",                                    # 11
  'Web https://example.com/a/b',                      # 12
  '',                                                 # 13
  '- The quick brown fox jumps over the lazy dog while the build keeps running and the',  # 14
  '  tests wait for the next machine to be free so nothing else starts on it right now',  # 15
  '  and then the report is written with every result and the time each step took ok'    # 16
)
[IO.File]::WriteAllLines($sample, $lines, (New-Object Text.UTF8Encoding $true))
# Rows 17-18: a path the terminal itself wraps at the right edge, mid-word and without indent. Row 19: the
# same file through \\localhost\C$ if that share answers here (checked on the worker thread: the first
# hover doesn't wait for it and finds nothing, the next one has the answer).
$env:WLT_DIR = $t
$env:WLT_SAMPLE = $sample
$env:WLT_FILE = "$t\file.txt"
$unc = '\\localhost\' + $t.Substring(0, 1) + '$' + $t.Substring(2) + '\file.txt'
$hasUnc = Test-Path $unc
$env:WLT_UNC = if ($hasUnc) { "Red $unc" } else { 'Red (no share)' }
$show = 'edit:cls; Get-Content -Encoding UTF8 $env:WLT_SAMPLE; $w = $Host.UI.RawUI.WindowSize.Width; Write-Host ((''x'' * ($w - 20)) + '' '' + $env:WLT_FILE); Write-Host $env:WLT_UNC'
$cu = ("Red $unc").Length - 3
$rod = 3 + [math]::Max(0, $t.IndexOf(' ')) + 2  # row 0, inside the part of $t after its first space
$c = @{
  final = ("Abre $t\Mis Notas\nota ").Length + 1; ahora = ("Abre $t\Mis Notas\nota final.txt ").Length + 1
  line = 'Mira src\main.cpp:'.Length + 1; luego = 'Mira src\main.cpp:120 '.Length + 1
  folder = ("Carpeta $t\src").Length - 2; missing = ("Falta $t\no-existe.txt").Length - 5
  saved = ("Guardado en $t\file.txt.").Length - 7; dot = ("Guardado en $t\file.txt.").Length - 1
  tool = ("Lanza $t\").Length + 1; run = ("Lanza $t\tool.exe o ").Length + 1
  url = ("Abre $fileUrl").Length - 6
}
$steps = @(
  'wait', 'wait', 'wait', 'edit:Set-Location $env:WLT_DIR', 'submit', 'wait', $show, 'submit', 'wait', 'wait',
  'hover:5,0', "hover:$rod,0", 'hover:4,1', 'hover:4,2', 'hover:15,2',
  "hover:$($c.final),3", "hover:$($c.ahora),3",
  'hover:6,4', "hover:$($c.line),4", "hover:$($c.luego),4",
  'hover:10,5', "hover:$($c.folder),6", "hover:$($c.missing),7", "hover:$($c.saved),8", "hover:$($c.dot),8",
  'hover:6,9', "hover:$($c.tool),10", "hover:$($c.run),10", "hover:$($c.url),11", 'hover:6,12', 'hover:2,18'
)
if ($hasUnc) { $steps += @("hover:$cu,19", 'wait', "hover:$cu,19") }
$steps += @(
  "link-action:$($c.tool),10", "link-action:$($c.run),10", "link-action:$($c.folder),6", 'link-action:10,5',
  'link-action:6,12', "link-action:$($c.missing),7",
  'hover-time:4,1', 'hover-time:30,15', 'hover-time:2,18',
  'quit'
)
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); 'TIMEOUT' }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$none = '(sin enlace)'
$want = @(
  "hover 5,0: path $shot", "hover $rod,0: path $shot", "hover 4,1: path $shot", "hover 4,2: path $shot",
  "hover 15,2: path $shot",
  "hover $($c.final),3: path $t\Mis Notas\nota final.txt", "hover $($c.ahora),3: $none",
  "hover 6,4: path $t\src\main.cpp", "hover $($c.line),4: path $t\src\main.cpp", "hover $($c.luego),4: $none",
  "hover 10,5: path $t\file.txt", "hover $($c.folder),6: path $t\src", "hover $($c.missing),7: $none",
  "hover $($c.saved),8: path $t\file.txt", "hover $($c.dot),8: $none",
  "hover 6,9: path $t\src\main.cpp", "hover $($c.tool),10: path $t\tool.exe", "hover $($c.run),10: path $t\run.ps1",
  "hover $($c.url),11: path $t\file.txt", 'hover 6,12: https://example.com/a/b', "hover 2,18: path $t\file.txt"
)
if ($hasUnc) { $want += @("hover $cu,19: $none", "hover $cu,19: path $unc") } else { '(\\localhost share not available: UNC case skipped)' }
$want += @(
  "link-action $($c.tool),10: select $t\tool.exe", "link-action $($c.run),10: select $t\run.ps1",
  "link-action $($c.folder),6: explorer $t\src", "link-action 10,5: open $t\file.txt",
  'link-action 6,12: open https://example.com/a/b', "link-action $($c.missing),7: $none"
)
$fail = 0
for ($i = 0; $i -lt $want.Count; $i++) {
  $ok = $i -lt $got.Count -and $got[$i] -eq $want[$i]
  if (-not $ok) { $fail++ }
  '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { if ($ok) { $got[$i] } else { "$($got[$i]) (esperado $($want[$i]))" } } else { "(falta) $($want[$i])" })
}
# Hover cost: cold = first hover with the path cache empty (disk), warm = worked out again with it full
# (every mouse move over a new cell). Generous limits: it's about not freezing the UI.
foreach ($l in $got | Where-Object { $_ -like 'hover-time *' }) {
  $ok = $l -match 'cold (\d+)us warm (\d+)us' -and [int]$Matches[1] -lt 50000 -and [int]$Matches[2] -lt 3000
  if (-not $ok) { $fail++ }
  '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $l
}
if (@($got | Where-Object { $_ -like 'hover-time *' }).Count -ne 3) { $fail++; 'FALLO faltan los hover-time' }
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $t, $sample
"fallos: $fail"
