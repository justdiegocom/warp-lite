# Con una imagen en el portapapeles, pegar dentro de un programa (cat -v por ssh) le manda Ctrl+V (^V).
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$bmp = New-Object System.Drawing.Bitmap 8, 8
[System.Windows.Forms.Clipboard]::SetImage($bmp)
$env:WARPLITE_SESSION = "$dir\session-pegar.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\pegar.bmp"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|edit:ssh -t fr cat -v|submit|wait|wait|wait|paste|wait|shot:$dir\pegar.bmp|quit`"" -PassThru
if (-not $p.WaitForExit(90000)) { $p.Kill(); Write-Output "TIMEOUT" }
Get-ChildItem "$dir\pegar.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
