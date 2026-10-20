# Símbolos que Hack no tiene (⎿ de Claude, iconos de Nerd Fonts, emoji, CJK) y URLs en blanco azulado.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-glifos.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\glifos.bmp"
$cmd = 'Write-Host ([char]0x23BF + " Claude " + [char]0x273B + " " + [char]0x2733 + "  nerd: " + [char]0xE0B0 + " " + [char]0xF115 + " " + [char]0xE718 + " " + [char]::ConvertFromUtf32(0xF0A1E) + "  emoji: " + [char]::ConvertFromUtf32(0x1F600) + "  CJK: " + [char]0x6F22 + [char]0x5B57 + "  url: http://100.90.50.98:8680/ fin")'
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|edit:$($cmd.Replace('"','\"'))|submit|wait|wait|shot:$dir\glifos.bmp|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); "TIMEOUT" }
Get-ChildItem "$dir\glifos.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
