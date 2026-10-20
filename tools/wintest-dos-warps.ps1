# Dos Warp abiertos por separado (dos veces el acceso directo), cerrados cada uno con su X:
# al abrir Warp una sola vez deben volver los dos, cada uno con lo suyo.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\wins", "$dir\dos-*.bmp"
$env:WARPLITE_DIR = "$dir\wins"
Remove-Item Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
$w = (1..8 | ForEach-Object { 'wait' }) -join '|'
$a = Start-Process "$dir\warp-lite.exe" -ArgumentList "--script `"wait|rename:hellomatik|edit:cd C:\Windows|submit|$w|$w|close`"" -PassThru
Start-Sleep 4
$b = Start-Process "$dir\warp-lite.exe" -ArgumentList "--script `"wait|rename:proyectos|edit:cd C:\Users|submit|$w|close`"" -PassThru
$a.WaitForExit(90000) | Out-Null; $b.WaitForExit(90000) | Out-Null
"ventanas guardadas: " + (Get-ChildItem "$dir\wins\window-*.txt").Count
foreach ($f in Get-ChildItem "$dir\wins\window-*.txt") {
  $t = (Get-Content $f.FullName | Where-Object { $_ -like 'tab *' }) -replace '^tab \d+ ', ''
  $name = -join ($t -split '(..)' | Where-Object { $_ -and $_ -ne '-' } | ForEach-Object { [char][Convert]::ToInt32($_, 16) })
  "  $($f.Name): pestaña «$name»"
}
# Abrir Warp una sola vez: la primera ventana la abre él y la otra la lanza (con su propio guion de prueba)
$env:WARPLITE_CHILD_SCRIPT = "wait|wait|wait|shot:$dir\dos-2-otra.bmp|wait|close"
$c = Start-Process "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|shot:$dir\dos-1-primera.bmp|wait|close`"" -PassThru
$c.WaitForExit(90000) | Out-Null
Start-Sleep 8
"ventanas abiertas al reabrir: " + ((Get-ChildItem "$dir\dos-*.bmp").Count)
"guardadas al final: " + (Get-ChildItem "$dir\wins\window-*.txt").Count
"procesos de prueba vivos: " + ((Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" }).Count)
