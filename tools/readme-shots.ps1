# Capturas del README: contenido genérico (C:\, C:\Windows), sin nada personal.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-readme.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\readme-*.bmp"
$s = @(
  'wait', 'wait', 'rename:hellomatik', 'edit:cd C:\', 'submit', 'edit:git --version', 'submit',
  'splitR', 'wait', 'wait', 'edit:cd C:\Windows', 'submit', 'edit:Get-ChildItem -Name', 'submit',
  'splitD', 'wait', 'wait', 'edit:cd C:\', 'submit', 'edit:ping -n 4 127.0.0.1', 'submit',
  'wait', 'wait', 'wait', 'wait', 'wait', 'left', 'edit:git status', 'wait', "shot:$dir\readme-panes.bmp",
  'tab', 'wait', 'wait', 'rename:build', 'edit:cd C:\', 'submit', 'tab', 'wait', 'wait', 'rename:logs', 'edit:cd C:\', 'submit',
  'goto:0', 'vtabs', 'wait', "shot:$dir\readme-vertical.bmp", 'vtabs', 'quit'
) -join '|'
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$s`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT" }
Get-ChildItem "$dir\readme-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
