# URL partida como la parte Claude Code (sangría y corte antes del borde): Ctrl sobre el trozo de la
# segunda línea debe dar la URL entera.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-url.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\url.bmp"
$cmd = 'cls; $w = $Host.UI.RawUI.WindowSize.Width; Write-Host (''  https://github.com/example/'' + (''x'' * ($w - 35)) + ''-li''); Write-Host ''  te''; Write-Host ''  - otra linea''; Write-Host ''''; Write-Host ''''; Write-Host '''''
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|edit:$($cmd.Replace('"','\"'))|submit|wait|wait|hover-text:te|shot:$dir\url.bmp|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); "TIMEOUT" }
Get-ChildItem "$dir\url.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
