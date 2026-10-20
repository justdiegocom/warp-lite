# Inglés por defecto, órdenes en verde/rojo, atenuado y colores, TERM, enlaces y cambio a español.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session-color.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\col-*.bmp"
$e = '$([char]27)'
$show = "Write-Host `"${e}[2mdim${e}[0m normal ${e}[38;5;208m256${e}[0m ${e}[38;2;120;200;255mtruecolor${e}[0m TERM=`$env:TERM https://github.com/warpdotdev/warp`""
$script = "wait|wait|wait|shot:$dir\col-1-ingles.bmp|edit:$show|submit|wait|hover:60,1|edit:git status|shot:$dir\col-2-verde.bmp|submit|wait|edit:asdsada asd|shot:$dir\col-3-rojo.bmp|lang:es|wait|shot:$dir\col-4-espanol.bmp|quit"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($script.Replace('"','\"'))`"" -PassThru
if (-not $p.WaitForExit(90000)) { $p.Kill(); Write-Output "TIMEOUT" }
Get-ChildItem "$dir\col-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
