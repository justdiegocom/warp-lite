# Pestañas verticales / ocultas y varias ventanas (sacar una pestaña, salir de todo y reabrir: only the
# main window comes back, the one with the tab moved out doesn't reopen by default).
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\wins", "$dir\vt-*.bmp", "$dir\mw-*.bmp", "$dir\session-vt.txt"
function Run($script, [int]$wait = 120000) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit($wait)) { $p.Kill(); Write-Output "TIMEOUT" }
}
# 1) Pestañas verticales y ocultas (sesión fija)
$env:WARPLITE_SESSION = "$dir\session-vt.txt"
Run "wait|tab|wait|rename:servidor|tab|wait|rename:logs|vtabs|wait|shot:$dir\vt-1-vertical.bmp|hide-tab|shot:$dir\vt-2-oculta.bmp|vtabs|wait|shot:$dir\vt-3-horizontal.bmp|vtabs|quit"
Get-Content $env:WARPLITE_SESSION | Select-String '^(vtabs|tab )' | ForEach-Object Line
Run "wait|wait|shot:$dir\vt-4-reabierta.bmp|quit"
Remove-Item Env:\WARPLITE_SESSION
# 2) Varias ventanas (carpeta propia de pruebas)
$env:WARPLITE_DIR = "$dir\wins"
$env:WARPLITE_CHILD_SCRIPT = "wait|wait|wait|shot:$dir\mw-2-nueva-ventana.bmp"
Run "wait|wait|edit:cd C:\Windows|submit|tab|wait|wait|rename:sacada|edit:echo soy la pestana sacada|submit|wait|detach-tab|wait|wait|shot:$dir\mw-1-original.bmp|wait|wait|quit-all"
Start-Sleep 3
"ventanas guardadas: " + ((Get-ChildItem "$dir\wins\window-*.txt").Count) + " (1: only the main one)"
$env:WARPLITE_CHILD_SCRIPT = "wait|wait|wait|wait|shot:$dir\mw-4-no-debe-volver.bmp"
Run "wait|wait|wait|wait|wait|shot:$dir\mw-3-reabierta.bmp|wait|quit-all"
Start-Sleep 3
"ventanas guardadas tras reabrir: " + ((Get-ChildItem "$dir\wins\window-*.txt").Count) + " (1)"
"the moved-out tab's window came back: " + (Test-Path "$dir\mw-4-no-debe-volver.bmp") + " (False)"
"procesos warp-lite de prueba vivos: " + ((Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" }).Count)
Get-ChildItem "$dir\vt-*.bmp", "$dir\mw-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
