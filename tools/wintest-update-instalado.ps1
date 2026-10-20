# Actualización desde el Warp INSTALADO (como en el PC de verdad): se instala base-setup.exe (esta
# versión), se arranca %LOCALAPPDATA%\Programs\warp-lite\Warp.exe y se actualiza a la 9.9.9 servida
# en $Base (mismo código). Comprueba que no confunde la versión del archivo nuevo con la suya.
param([Parameter(Mandatory)][string]$Base)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$inst = "$env:LOCALAPPDATA\Programs\warp-lite\Warp.exe"
$env:WARPLITE_SESSION = "$dir\session-updi.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_UPDATE_URL = "$Base/latest.json"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$env:WARPLITE_SESSION.updlog", "$dir\updi-*.bmp"
Invoke-WebRequest -UseBasicParsing "$Base/base-setup.exe" -OutFile "$dir\base-setup.exe"
Start-Process "$dir\base-setup.exe" -ArgumentList '--silent' -Wait
"partida: $((Get-Item $inst).VersionInfo.FileVersion)"
$steps = @('wait', 'auto-update:on', 'update-check', 'update-state', "shot:$dir\updi-1.bmp", 'wait', 'wait', 'wait', 'wait', 'wait', 'wait', 'wait', 'wait', 'update-state', "shot:$dir\updi-2.bmp", 'update-click', 'wait', 'wait', 'quit')
$p = Start-Process -FilePath $inst -ArgumentList "--script `"$($steps -join '|')`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT" }
"instalada: $((Get-Item $inst).VersionInfo.FileVersion)"
Start-Sleep 3
$re = @(Get-CimInstance Win32_Process -Filter "Name='Warp.exe'" | Where-Object { $_.CommandLine -like '*--relaunch*' })
"reinicio con --relaunch: $($re.Count)"
foreach ($r in $re) { Get-CimInstance Win32_Process -Filter "ParentProcessId=$($r.ProcessId)" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }; Stop-Process -Id $r.ProcessId -Force }
Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue
Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.updlog" -ErrorAction SilentlyContinue
# Se deja instalada la última publicada.
Invoke-WebRequest -UseBasicParsing 'https://github.com/justdiegocom/warp-lite/releases/latest/download/warp-lite-setup.exe' -OutFile "$dir\setup-last.exe"
Start-Process "$dir\setup-last.exe" -ArgumentList '--silent' -Wait
Remove-Item "$dir\setup-last.exe", "$dir\base-setup.exe"
"al acabar queda instalada: $((Get-Item $inst).VersionInfo.FileVersion)"
