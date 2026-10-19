# La cabecera del panel muestra la orden en marcha y vuelve al título al terminar.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session-proceso.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\proc-*.bmp"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"splitR|type:ping -n 6 127.0.0.1|enter|wait|shot:$dir\proc-1-en-marcha.bmp|wait|wait|wait|wait|wait|shot:$dir\proc-2-terminado.bmp|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); Write-Output "TIMEOUT" }
Get-ChildItem "$dir\proc-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
