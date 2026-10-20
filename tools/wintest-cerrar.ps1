# Varias ventanas cerradas cada una con su X: al reabrir vuelven todas.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\wins"
$env:WARPLITE_DIR = "$dir\wins"
function Run($script) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT" }
}
$env:WARPLITE_CHILD_SCRIPT = "wait|wait|wait|wait|wait|wait|close"   # la nueva ventana se cierra sola, después
Run "wait|wait|tab|wait|wait|rename:sacada|detach-tab|wait|close"
Start-Sleep 12
"guardadas tras cerrar una a una: " + (Get-ChildItem "$dir\wins\window-*.txt").Count
$env:WARPLITE_CHILD_SCRIPT = "wait|wait|wait|close"
Run "wait|wait|wait|close"
Start-Sleep 8
"guardadas tras reabrir y cerrar: " + (Get-ChildItem "$dir\wins\window-*.txt").Count
"procesos de prueba vivos: " + ((Get-Process Warp, warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" }).Count)
