# URLs partidas de Claude Code (listas, «(sirve…» debajo), OSC 52 → portapapeles y arrastrar
# pestañas para reordenarlas. Todo deja su resultado en session-cup.txt.bench; se compara aquí.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-cup.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$dir\cup.bmp"
$urls = 'cls; $w = $Host.UI.RawUI.WindowSize.Width; ' +
  'Write-Host (''  - https://github.com/Zas-Player/'' + (''z'' * ($w - 35))); Write-Host ''    as/releases/tag/v1.21.24''; ' +
  'Write-Host (''Vista previa: http://100.90.50.98:8680/'' + (''y'' * ($w - 40))); Write-Host ''(sirve la libreria reexportada).''; ' +
  'Write-Host (''https://ejemplo.com/'' + (''q'' * ($w - 21))); Write-Host ''Prueba a volver a un traje''; ' +
  'Write-Host (''  https://github.com/example/'' + (''x'' * ($w - 35)) + ''-li''); Write-Host ''  te''; Write-Host ''  - otra linea'''
# «https» al final de la línea + «://…» abajo, y una URL partida dentro de un panel de tmux (borde │).
$tmux = 'cls; $w = $Host.UI.RawUI.WindowSize.Width; $m = [math]::Floor($w / 2); $b = [string][char]0x2502; ' +
  'Write-Host ((''x'' * ($w - 12)) + '' Repo https''); Write-Host ''://github.com/justdiegocom/better-warp''; ' +
  'Write-Host ((''Ver https://github.com/justdiegocom/'' + (''a'' * ($m - 36))) + $b + ''otro panel''); ' +
  'Write-Host ((''warp-lite.'' + ('' '' * ($m - 10))) + $b + ''mas texto aqui'')'
# OSC 52 como lo manda tmux al copiar, y un OSC 9;9 sin token con una ruta de red (debe ignorarse).
$osc = '$e = [char]27; $b = [char]7; [Console]::Write($e + '']52;c;'' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(''hola'' + [char]10 + ''mundo ñ'')) + $b); [Console]::Write($e + '']9;9;\\evil\share'' + $b)'
$steps = @(
  'wait', 'wait', 'wait', "edit:$urls", 'submit', 'wait',
  'hover-text:as/releases/tag/v1.21.24', 'hover-text:(sirve la libreria reexportada).', 'hover-text:Prueba a volver a un traje', 'hover-text:te',
  "shot:$dir\cup.bmp",
  'clear', "edit:$tmux", 'submit', 'wait', 'hover:2,1', 'hover:6,2', 'hover:2,3', 'hover:2,4',
  "edit:$osc", 'submit', 'wait', 'clipboard', 'edit:pwd', 'submit', 'wait',
  'rename:A', 'tab', 'rename:B', 'tab', 'rename:C', 'tabs', 'drag-tab:0,2', 'tabs', 'drag-tab:2,0', 'tabs', 'drag-tab:1,2', 'tabs',
  'quit'
)
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(90000)) { $p.Kill(); "TIMEOUT" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$want = @(
  'hover-text as/releases/tag/v1.21.24: https://github.com/Zas-Player/*as/releases/tag/v1.21.24',
  'hover-text (sirve la libreria reexportada).: (sin enlace)',
  'hover-text Prueba a volver a un traje: (sin enlace)',
  'hover-text te: https://github.com/example/*-lite',
  'hover 2,1: https://github.com/justdiegocom/better-warp',
  'hover 6,2: https://github.com/justdiegocom/aaa*aaawarp-lite',
  'hover 2,3: https://github.com/justdiegocom/aaa*aaawarp-lite',
  'hover 2,4: (sin enlace)',
  'clipboard: hola\nmundo ñ',
  'tabs: A B *C', 'tabs: B C *A', 'tabs: *A B C', 'tabs: A C *B'
)
$fail = 0
for ($i = 0; $i -lt $want.Count; $i++) {
  $ok = $i -lt $got.Count -and $got[$i] -like $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
}
# La ruta de red del OSC 9;9 falso no debe haber llegado a la sesión guardada.
if ((Get-Content -Raw $env:WARPLITE_SESSION -ErrorAction SilentlyContinue) -match '5c5c6576696c') { $fail++; 'FALLO la ruta \\evil\share llegó a la sesión' }
"fallos: $fail"
