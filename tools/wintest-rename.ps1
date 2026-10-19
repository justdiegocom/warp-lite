# Renombrar pestaña: al empezar el nombre sale seleccionado entero y lo que se escribe lo reemplaza.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session-rename.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\ren-*.bmp"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"rename-start|shot:$dir\ren-1-seleccionado.bmp|rename-type:servidor|shot:$dir\ren-2-reemplazado.bmp|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); Write-Output "TIMEOUT" }
Get-ChildItem "$dir\ren-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
