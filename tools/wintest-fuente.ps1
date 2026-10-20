# Terminal font (title-bar right-click → Font): Cascadia Mono, a Google font downloaded for real, kept
# after a restart (loaded from the cache, with the download server unreachable), a failed download keeps
# the font, every downloadable font in the menu renders with its own face and a real bold, and another
# window of the same Warp follows the change. Results in session-fuente.txt.bench (and fuente-wins).
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$s = "$dir\session-fuente.txt"
$wins = "$dir\fuente-wins"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", $wins, "$dir\fuente-*.bmp"
Remove-Item Env:\WARPLITE_FONT_URL, Env:\WARPLITE_DIR, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
function Run($steps, $timeout = 180000) {
  $arg = ($steps -join '|').Replace('"', '\"')
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
  if (-not $p.WaitForExit($timeout)) { $p.Kill(); 'TIMEOUT' }
}
function Bench($f) { @(Get-Content -Encoding UTF8 "$f.bench" -ErrorAction SilentlyContinue) }
function Font($l) { if ($l -match '^font: \[([^\]]*)\] (\d+x\d+)') { @{ Face = $Matches[1]; Cell = $Matches[2] } } else { @{ Face = ''; Cell = '' } } }
$fail = 0
# A real bold is another face than the regular one (GDI gives a static font's unique name, a variable one's
# instance name); a synthesized one would repeat the regular face with weight 700.
function RealBold($l) {
  $st = @{}; foreach ($m in [regex]::Matches("$l", '(\w+)=\[([^\]]*) \d+\]')) { $st[$m.Groups[1].Value] = $m.Groups[2].Value }
  $st.bold -and $st.bold -ne $st.regular -and "$l" -match 'bold=\[[^\]]* 700\]'
}
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }
# Regular, bold, italic and bold italic in the terminal, to look at in the screenshots.
$sample = 'Write-Host ("Regular 0O1lI {}[]=> " + [char]27 + "[1mBold 0O1lI {}[]=>" + [char]27 + "[0m " + [char]27 + "[3mItalic 0O1lI" + [char]27 + "[0m " + [char]27 + "[1;3mBold Italic" + [char]27 + "[0m")'
# 'clear' first: when reopened, the last command is already waiting in the input box.
$intro = @('wait', 'wait', 'wait', 'clear', "edit:$sample", 'submit', 'wait')
$env:WARPLITE_SESSION = $s

# 1. Hack → Cascadia Mono → JetBrains Mono (downloaded).
Run ($intro + @('font', 'font:Cascadia Mono', 'font-wait', 'font', 'font:JetBrains Mono', 'font-wait', 'font', 'zoom:200', "shot:$dir\fuente-jetbrains.bmp", 'zoom:100', 'quit'))
$b1 = Bench $s
$b1 | ForEach-Object { "  $_" }
$hack = Font $b1[0]; $casc = Font $b1[1]; $jb = Font $b1[2]
Check ($hack.Face -eq 'Hack') "por defecto Hack ($($hack.Cell))"
Check ($casc.Face -eq 'Cascadia Mono' -and $casc.Cell -ne $hack.Cell) "Cascadia Mono, celda $($hack.Cell) -> $($casc.Cell)"
Check ($jb.Face -eq 'JetBrains Mono' -and "$($b1[2])".Contains('bold=[JetBrains Mono Bold 700]')) "JetBrains Mono descargada, negrita de verdad ($($jb.Cell))"
Check ((Get-Content -Encoding UTF8 "$s.font" -ErrorAction SilentlyContinue) -eq 'JetBrains Mono') 'guardada en la sesión de prueba'
Get-ChildItem "$s.fonts" -ErrorAction SilentlyContinue | ForEach-Object { "  caché: $($_.Name) $($_.Length)" }

# 2. Restart with the font server unreachable: JetBrains Mono comes from the cache; Fira Code fails and the font stays.
$env:WARPLITE_FONT_URL = 'https://fonts.invalid/'
Run @('wait', 'wait', 'wait', 'font', 'font:Fira Code', 'font-wait', 'toast', 'font', 'quit')
Remove-Item Env:\WARPLITE_FONT_URL
$b2 = @(Bench $s | Select-Object -Skip $b1.Count)
$b2 | ForEach-Object { "  $_" }
$re = Font $b2[0]; $after = Font $b2[2]
Check ($re.Face -eq 'JetBrains Mono' -and $re.Cell -eq $jb.Cell) 'al reabrir sigue JetBrains Mono (desde la caché, sin red)'
Check ("$($b2[1])" -eq "toast: Couldn't download Fira Code") 'aviso de descarga fallida'
Check ($after.Face -eq 'JetBrains Mono' -and "$($b2[2])".Contains('error=')) 'descarga fallida: se queda la fuente que había'
Check (-not (Test-Path -LiteralPath "$s.fonts\FiraCode[wght].ttf") -and -not (Get-ChildItem "$s.fonts\*.tmp" -ErrorAction SilentlyContinue)) 'sin ficheros a medias en la caché'

# 3. Every other downloadable font: its own face and a real bold.
$cat = 'Fira Code', 'Source Code Pro', 'Roboto Mono', 'IBM Plex Mono', 'Ubuntu Mono', 'Inconsolata'
$steps = $intro
foreach ($f in $cat) { $steps += @("font:$f", 'font-wait', 'font', 'zoom:200', "shot:$dir\fuente-$($f.Replace(' ', '')).bmp", 'zoom:100') }
Run ($steps + @('font-others', 'font:JetBrains Mono', 'font', 'quit')) 300000
$b3 = @(Bench $s | Select-Object -Skip ($b1.Count + $b2.Count))
$b3 | ForEach-Object { "  $_" }
for ($i = 0; $i -lt $cat.Count; $i++) {
  $f = Font $b3[$i]
  Check ($f.Face -eq $cat[$i] -and (RealBold $b3[$i])) "$($cat[$i]): su fuente y negrita de verdad ($($f.Cell))"
}
$others = "$($b3[$cat.Count])"
$dup = @('Hack', 'Cascadia Mono', 'Consolas', 'JetBrains Mono', 'Symbols Nerd Font Mono') + $cat | Where-Object { $others.Contains("[$_]") }
Check ($others -like 'font-others: *' -and -not $others.Contains('[@') -and -not $dup) "otras instaladas sin @ ni repetidas: $($others.Substring(0, [math]::Min(120, $others.Length)))"
Check ((Font $b3[$cat.Count + 1]).Face -eq 'JetBrains Mono') 'vuelta a JetBrains Mono (ya en caché)'

# 4. Two windows of the same Warp (WARPLITE_DIR, like the real one): the other one follows the change.
Remove-Item Env:\WARPLITE_SESSION
$env:WARPLITE_DIR = $wins
$w = @('wait') * 20
$p2 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$((@('wait', 'wait', 'wait', 'font') + $w + @('font', 'quit')) -join '|')`"" -PassThru
$recv = $null
for ($t = 0; $t -lt 60 -and -not $recv; $t++) {
  Start-Sleep -Milliseconds 500
  $recv = Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Where-Object { (Get-Content $_.FullName -ErrorAction SilentlyContinue) -match '^font:' } | Select-Object -First 1
}
$p1 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|font:JetBrains Mono|font-wait|font|quit`"" -PassThru
if (-not $p1.WaitForExit(90000)) { $p1.Kill(); 'TIMEOUT 1' }
if (-not $p2.WaitForExit(90000)) { $p2.Kill(); 'TIMEOUT 2' }
Remove-Item Env:\WARPLITE_DIR
$rb = if ($recv) { @(Get-Content -Encoding UTF8 $recv.FullName) } else { @() }
$sb = @(Get-ChildItem "$wins\*.bench" | Where-Object { -not $recv -or $_.FullName -ne $recv.FullName } | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName })
$rb | ForEach-Object { "  otra: $_" }
$sb | ForEach-Object { "  elige: $_" }
Check ((Font $sb[0]).Face -eq 'JetBrains Mono') 'la ventana que elige: JetBrains Mono'
Check ((Font $rb[0]).Face -eq 'Hack' -and (Font $rb[1]).Face -eq 'JetBrains Mono' -and (Font $rb[1]).Cell -eq $jb.Cell) 'la otra ventana pasa de Hack a JetBrains Mono'
Check ((Get-Content -Encoding UTF8 "$wins\font.txt" -ErrorAction SilentlyContinue) -eq 'JetBrains Mono' -and (Test-Path -LiteralPath "$wins\fonts\JetBrainsMono[wght].ttf")) 'ajuste y caché en la carpeta de la instancia'
Check (-not (Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" })) 'sin procesos de prueba vivos'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s*", $wins
"fallos: $fail"
