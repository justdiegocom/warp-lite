# «Iniciar con Windows»: activa y desactiva el valor Warp de HKCU\...\Run (deja el estado que hubiera).
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$before = (Get-ItemProperty $key -Name Warp -ErrorAction SilentlyContinue).Warp
$env:WARPLITE_SESSION = "$dir\session-inicio.txt"
function Run($script) { $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru; $p.WaitForExit(60000) | Out-Null }
Run "startup:on|quit"
"activado: " + (Get-ItemProperty $key -Name Warp -ErrorAction SilentlyContinue).Warp
Run "startup:off|quit"
"desactivado: existe=" + [bool](Get-ItemProperty $key -Name Warp -ErrorAction SilentlyContinue)
if ($before) { Set-ItemProperty $key -Name Warp -Value $before }
