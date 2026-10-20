# Zoom and font size come back after a restart: the terminal cell must match the one before closing.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_SESSION = "$dir\session-zoom.txt"
Remove-Item -Force -ErrorAction SilentlyContinue "$env:WARPLITE_SESSION*"
function Run($steps) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps -join '|')`"" -PassThru
  if (-not $p.WaitForExit(60000)) { $p.Kill(); 'TIMEOUT' }
}
Run @('wait', 'font', 'zoom:150', 'font', 'quit')
Run @('wait', 'font', 'quit')
$b = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench")
$b | ForEach-Object { "  $_" }
$cell = $b | ForEach-Object { if ($_ -match '\] (\d+x\d+)') { $Matches[1] } }
$fail = 0
if ($cell.Count -ne 3 -or $cell[1] -eq $cell[0] -or $cell[2] -ne $cell[1]) { $fail++; "FALLO celda $($cell -join ' -> ')" } else { "OK   celda $($cell -join ' -> ')" }
"fallos: $fail"
