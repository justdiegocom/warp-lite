# Caja de entrada: se escribe abajo, Enter lo manda a la shell, la ruta sale en su apartado;
# al reabrir, cada panel vuelve a su carpeta con su última orden en la caja.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session-editor.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\ed-*.bmp"
function Run($script) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit(90000)) { $p.Kill(); Write-Output "TIMEOUT" }
}
Run "wait|edit:cd C:\Windows|submit|edit:Get-Date|submit|splitR|wait|wait|edit:ping -n 5 127.0.0.1|submit|shot:$dir\ed-1-ejecutando.bmp|wait|wait|wait|wait|wait|edit:git sta|shot:$dir\ed-2-escribiendo.bmp|quit"
Write-Output "--- sesión"; Get-Content $env:WARPLITE_SESSION
Run "wait|wait|shot:$dir\ed-3-reabierto.bmp|quit"
Get-ChildItem "$dir\ed-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
