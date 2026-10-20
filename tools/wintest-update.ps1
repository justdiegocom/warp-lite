# Actualizaciones contra una release falsa (servida por http en $Base): huella mala → no instala;
# huella buena con «Actualizar automáticamente» → instala sola y «Reiniciar» abre el Warp instalado
# con --relaunch; con la versión al día → nada. Instala de verdad el setup servido (debe ser este).
param([Parameter(Mandatory)][string]$Base)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-upd.txt"
$env:WARPLITE_NO_UPDATE = '1'
$inst = "$env:LOCALAPPDATA\Programs\warp-lite\Warp.exe"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$env:WARPLITE_SESSION.update", "$dir\upd-*.bmp"
function Run($url, $fake, $steps) {
  $env:WARPLITE_UPDATE_URL = $url
  if ($fake) { $env:WARPLITE_FAKE_VERSION = $fake } else { Remove-Item Env:WARPLITE_FAKE_VERSION -ErrorAction SilentlyContinue }
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps -join '|')`"" -PassThru
  if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT" }
}
# Punto de partida: la 0.2.0 publicada instalada (como quien actualiza desde ella).
Invoke-WebRequest -UseBasicParsing 'https://github.com/justdiegocom/warp-lite/releases/download/v0.2.0/warp-lite-setup.exe' -OutFile "$dir\setup-020.exe"
Start-Process "$dir\setup-020.exe" -ArgumentList '--silent' -Wait
Remove-Item "$dir\setup-020.exe"
"partida: $((Get-Item $inst).VersionInfo.FileVersion)"
$h0 = (Get-FileHash $inst).Hash
Run "$Base/bad.json" '0.0.1' @('wait', 'auto-update:off', 'update-check', 'update-state', 'update-click', 'wait', 'wait', 'wait', 'update-state', "shot:$dir\upd-1-fallo.bmp", 'quit')
"instalado sin tocar tras la huella mala: $((Get-FileHash $inst).Hash -eq $h0)"
Run "$Base/latest.json" '0.0.1' @('wait', 'auto-update:on', 'update-check', 'wait', 'wait', 'wait', 'wait', 'wait', 'update-state', "shot:$dir\upd-2-listo.bmp", 'update-click', 'wait', 'wait', 'quit')
"instalado = el servido: $((Get-FileHash $inst).Hash -eq (Get-FileHash "$dir\warp-lite.exe").Hash) (versión $((Get-Item $inst).VersionInfo.FileVersion))"
Start-Sleep 3
$re = @(Get-CimInstance Win32_Process -Filter "Name='Warp.exe'" | Where-Object { $_.CommandLine -like '*--relaunch*' })
"reinicio con --relaunch: $($re.Count)"
foreach ($r in $re) { Stop-Process -Id $r.ProcessId -Force; Get-CimInstance Win32_Process -Filter "ParentProcessId=$($r.ProcessId)" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } }
Run "$Base/latest.json" $null @('wait', 'update-check', 'update-state', 'quit')
Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue
