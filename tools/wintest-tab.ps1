# Autocompletado con Tab en la caja de entrada: orden única, menú de rutas (↓ + Enter), carpeta con
# espacios, filtrar escribiendo, parámetros (pedidos a PowerShell) y ~\. Resultado en session-tab.txt.bench.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-tab.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$dir\tab.bmp"
$ct = "$dir\ct"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ct
New-Item -ItemType Directory -Force "$ct\Program Files Test", "$ct\alpine" | Out-Null
Set-Content "$ct\alpha.txt" 'x'
$steps = @(
  'wait', 'wait', 'wait', "edit:Set-Location '$ct'", 'submit', 'wait', 'wait', 'wait',
  'clear', 'edit:Get-ChildI', 'key:tab', 'edit-text',
  'clear', 'edit:ls al', 'key:tab', 'comp', 'key:down', 'key:enter', 'edit-text',
  'clear', 'edit:ls Prog', 'key:tab', 'edit-text',
  'clear', 'edit:ls a', 'key:tab', 'comp', 'edit:lpi', 'comp', 'key:enter', 'edit-text',
  'clear', 'edit:Get-ChildItem -Rec', 'key:tab', 'wait', 'wait', 'wait', 'wait', 'edit-text',
  'clear', 'edit:Get-ChildItem -Fi', 'key:tab', 'comp', "shot:$dir\tab.bmp", 'key:esc', 'comp',
  'clear', 'edit:ls ~\', 'key:tab', 'comp',
  'quit'
)
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$want = @(
  'edit: `[Get-ChildItem `]',
  'comp: alpine\ alpha.txt', 'edit: `[ls alpha.txt`]',
  "edit: ``[ls 'Program Files Test\'``]",
  'comp: alpine\ alpha.txt', 'comp: alpine\', 'edit: `[ls alpine\`]',
  'edit: `[Get-ChildItem -Recurse `]',
  'comp: *-Filter*-File*', 'comp: (cerrado)',
  'comp: *\*'
)
$fail = 0
for ($i = 0; $i -lt $want.Count; $i++) {
  $ok = $i -lt $got.Count -and $got[$i] -like $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ct
"fallos: $fail"
