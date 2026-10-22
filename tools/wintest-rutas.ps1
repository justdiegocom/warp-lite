# Paths as links (Ctrl+hover / Ctrl+click, like URLs): the quoted path Claude Code wrapped over three rows
# (spaces dropped at the breaks, 2-space indent, no closing quote) with every row resolving to the whole
# path, an unquoted path with spaces, paths relative to the pane folder, src\main.cpp:120, file.txt:12:3,
# a folder, a missing path, a path followed by . or ), file:///, a path the terminal wrapped mid-word, a
# URL that stays a URL, and what Ctrl+click would do (open / explorer / select; nothing is opened). Shares
# (\\host\share, //host/share) and \\?\ \\.\ device paths are never links and are never even looked up
# (that would connect to the host and leak the NTLM hash): an existing file through \\localhost\C$, a
# \\evil\share path, and the path-probes counter of lookups that reached the network (must be 0). Also
# how long a hover takes with the path cache empty and full, and the right-click menu on a file, a folder and
# a URL (and its copy item). Highlighting, before any hover: what each row draws in the path color (the paths
# above, also wrapped and with spaces; nothing for the missing path, the URL, the sentence and the network and
# device paths) and how long a pass takes. -Capture <file.bmp> also saves a screenshot of it. Results in
# session-rutas.txt.bench.
param([string]$Capture = '')
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_SESSION = "$dir\session-rutas.txt"
$t = "$dir\rutas"
$sample = "$dir\rutas-sample.txt"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$env:WARPLITE_SESSION*", $t, $sample
$shotDir = "$t\Diego Rodriguez\Pictures\Zas"
New-Item -ItemType Directory -Force $shotDir, "$t\Mis Notas", "$t\Mis", "$t\src" | Out-Null
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
  "Carpeta $t\src /s",                                # 6
  "Falta $t\no-existe.txt",                           # 7
  "Guardado en $t\file.txt.",                         # 8
  '(ver src\main.cpp)',                               # 9
  "Lanza $t\tool.exe o run.ps1 ya",                   # 10
  "Abre $fileUrl",                                    # 11
  'Web https://example.com/a/b',                      # 12
  "Nueva $t\Mis Notas\nueva.txt",                      # 13
  '- The quick brown fox jumps over the lazy dog while the build keeps running and the',  # 14
  '  tests wait for the next machine to be free so nothing else starts on it right now',  # 15
  '  and then the report is written with every result and the time each step took ok'    # 16
)
[IO.File]::WriteAllLines($sample, $lines, (New-Object Text.UTF8Encoding $true))
# Rows 17-18: a path the terminal itself wraps at the right edge, mid-word and without indent. Row 19: the
# same file through \\localhost\C$ (it exists if that share answers here, and still isn't a link: hovered
# twice, in case an answer came later). Row 20: \\evil\share and //evil/share. Row 21: \\.\ and \\?\.
$env:WLT_DIR = $t
$env:WLT_SAMPLE = $sample
$env:WLT_FILE = "$t\file.txt"
$unc = '\\localhost\' + $t.Substring(0, 1) + '$' + $t.Substring(2) + '\file.txt'
"(\\localhost share answers here: $(Test-Path $unc))"
$env:WLT_UNC = "Red $unc"
$env:WLT_EVIL = 'Leer \\evil\share\file.txt y //evil/share/x.txt'
$env:WLT_DEV = "Dev \\.\$t\file.txt o \\?\$t\file.txt"
$show = 'edit:cls; Get-Content -Encoding UTF8 $env:WLT_SAMPLE; $w = $Host.UI.RawUI.WindowSize.Width; Write-Host ((''x'' * ($w - 20)) + '' '' + $env:WLT_FILE); Write-Host $env:WLT_UNC; Write-Host $env:WLT_EVIL; Write-Host $env:WLT_DEV'
$cu = ("Red $unc").Length - 3
$ce = @(8, ('Leer \\evil\share\file.txt y //').Length + 1)
$cd = @(9, ("Dev \\.\$t\file.txt o \\?\").Length + 1)
$rod = 3 + [math]::Max(0, $t.IndexOf(' ')) + 2  # row 0, inside the part of $t after its first space
$c = @{
  final = ("Abre $t\Mis Notas\nota ").Length + 1; ahora = ("Abre $t\Mis Notas\nota final.txt ").Length + 1
  line = 'Mira src\main.cpp:'.Length + 1; luego = 'Mira src\main.cpp:120 '.Length + 1
  folder = ("Carpeta $t\src").Length - 2; missing = ("Falta $t\no-existe.txt").Length - 5
  saved = ("Guardado en $t\file.txt.").Length - 7; dot = ("Guardado en $t\file.txt.").Length - 1
  tool = ("Lanza $t\").Length + 1; run = ("Lanza $t\tool.exe o ").Length + 1
  url = ("Abre $fileUrl").Length - 6
}
$steps = @('wait', 'wait', 'wait', 'edit:Set-Location $env:WLT_DIR', 'submit', 'wait', $show, 'submit', 'wait', 'wait')
$steps += 0..21 | ForEach-Object { "path-spans:$_" }
if ($Capture) { $steps += "shot:$Capture" }
$steps += @(
  'hover:5,0', "hover:$rod,0", 'hover:4,1', 'hover:4,2', 'hover:15,2',
  "hover:$($c.final),3", "hover:$($c.ahora),3",
  'hover:6,4', "hover:$($c.line),4", "hover:$($c.luego),4",
  'hover:10,5', "hover:$($c.folder),6", "hover:$($c.missing),7", "hover:$($c.saved),8", "hover:$($c.dot),8",
  'hover:6,9', "hover:$($c.tool),10", "hover:$($c.run),10", "hover:$($c.url),11", 'hover:6,12', 'hover:2,18'
)
$steps += @("hover:$cu,19", 'wait', "hover:$cu,19", "hover:$($ce[0]),20", "hover:$($ce[1]),20", "hover:$($cd[0]),21",
  "hover:$($cd[1]),21")
$steps += @(
  "link-action:$($c.tool),10", "link-action:$($c.run),10", "link-action:$($c.folder),6", 'link-action:10,5',
  'link-action:6,12', "link-action:$($c.missing),7", "link-action:$cu,19",
  "link-menu:$($c.final),3", "link-menu:$($c.folder),6", 'link-menu:6,12', "link-menu:$($c.missing),7",
  "link-menu:$($c.final),3,2", 'clipboard', 'link-menu:6,12,2', 'clipboard', 'path-probes', 'path-scan-time',
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
$want += @("hover $cu,19: $none", "hover $cu,19: $none", "hover $($ce[0]),20: $none", "hover $($ce[1]),20: $none",
  "hover $($cd[0]),21: $none", "hover $($cd[1]),21: $none")
$want += @(
  "link-action $($c.tool),10: select $t\tool.exe", "link-action $($c.run),10: select $t\run.ps1",
  "link-action $($c.folder),6: explorer $t\src", "link-action 10,5: open $t\file.txt",
  'link-action 6,12: open https://example.com/a/b', "link-action $($c.missing),7: $none", "link-action $cu,19: $none",
  "link-menu $($c.final),3: Open | Copy path | Open file location", "link-menu $($c.folder),6: Open | Copy path | Open file location",
  'link-menu 6,12: Open | Copy link', "link-menu $($c.missing),7: $none",
  "link-menu $($c.final),3: Open | Copy path | Open file location", "clipboard: $t\Mis Notas\nota final.txt",
  'link-menu 6,12: Open | Copy link', 'clipboard: https://example.com/a/b'
)
# Highlighting: the text of each path drawn in its color, row by row (17-18: the path the terminal wrapped).
# Missing files draw nothing, not even a folder that exists before a space in them ($t\Mis in row 13).
$file = $env:WLT_FILE
$wantSpans = @(
  "$t\Diego", 'Rodriguez\Pictures\Zas\Screenshot', '2026-10-21 14.24.19.png', "$t\Mis Notas\nota final.txt",
  'src\main.cpp:120', 'file.txt:12:3', "$t\src", '(none)',
  "$t\file.txt", 'src\main.cpp', "$t\tool.exe | run.ps1", $fileUrl, '(none)', '(none)', '(none)', '(none)', '(none)',
  $file.Substring(0, 19), $file.Substring(19), '(none)', '(none)', '(none)'
)
$spans = @($got | Where-Object { $_ -like 'path-spans *' })
$scan = @($got | Where-Object { $_ -like 'path-scan-time*' })
$got = @($got | Where-Object { $_ -notlike 'path-spans *' -and $_ -notlike 'path-scan-time*' })
$fail = 0
for ($i = 0; $i -lt $wantSpans.Count; $i++) {
  $w = "path-spans ${i}: $($wantSpans[$i])"
  $ok = $i -lt $spans.Count -and $spans[$i] -eq $w
  if (-not $ok) { $fail++ }
  '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $spans.Count) { if ($ok) { $spans[$i] } else { "$($spans[$i]) (esperado $w)" } } else { "(falta) $w" })
}
# A pass over the whole view with the cache full: generous limit, it's about not freezing the UI.
$ok = $scan.Count -eq 1 -and $scan[0] -match '^path-scan-time: (\d+)us$' -and [int]$Matches[1] -lt 5000
if (-not $ok) { $fail++ }
'{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($scan.Count) { $scan[0] } else { '(falta) path-scan-time' })
# Lookups so far: some (the local paths above), none of them to the network.
$probes = @($got | Where-Object { $_ -like 'path-probes:*' })
$ok = $probes.Count -eq 1 -and $probes[0] -match '^path-probes: (\d+) remote 0$' -and [int]$Matches[1] -gt 0
if (-not $ok) { $fail++ }
'{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($probes.Count) { $probes[0] } else { '(falta) path-probes' })
$got = @($got | Where-Object { $_ -notlike 'path-probes:*' })
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
