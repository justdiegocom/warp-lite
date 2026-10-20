# Con una imagen en el portapapeles, pegar escribe la ruta del PNG guardado (en la caja y dentro de ssh).
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$bmp = New-Object System.Drawing.Bitmap 8, 8
[System.Windows.Forms.Clipboard]::SetImage($bmp)
$env:WARPLITE_SESSION = "$dir\session-pegar.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\pegar*.bmp"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|paste|wait|shot:$dir\pegar-1-caja.bmp|clear|edit:ls|drop:C:\Program Files;C:\Windows\win.ini|shot:$dir\pegar-0-soltar.bmp|clear|edit:ssh -t fr cat|submit|wait|wait|wait|paste|wait|shot:$dir\pegar-2-ssh.bmp|quit`"" -PassThru
if (-not $p.WaitForExit(90000)) { $p.Kill(); Write-Output "TIMEOUT" }
Get-ChildItem "$dir\pegar*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
"PNG guardados: " + ((Get-ChildItem "$env:TEMP\warp-lite\paste-*.png" -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name) $($_.Length)" }) -join ", ")
