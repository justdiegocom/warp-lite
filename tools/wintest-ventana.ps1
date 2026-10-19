# La ventana se reabre con el mismo tamaño y estado (en ventana o maximizada) que al cerrarla.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session-ventana.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\win-*.bmp"
function Run($script) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit(60000)) { $p.Kill(); Write-Output "TIMEOUT" }
}
Run "move:120,80,900,600|shot:$dir\win-1-ventana.bmp|quit"
Write-Output ("guardado: " + (Select-String -Path $env:WARPLITE_SESSION -Pattern '^win ').Line)
Run "shot:$dir\win-2-reabierta.bmp|maximize|shot:$dir\win-3-maximizada.bmp|quit"
Write-Output ("guardado: " + (Select-String -Path $env:WARPLITE_SESSION -Pattern '^win ').Line)
Run "shot:$dir\win-4-reabierta.bmp|quit"
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$dir\win-*.bmp" | ForEach-Object { $i = [Drawing.Image]::FromFile($_.FullName); "$($_.Name) $($i.Width)x$($i.Height)"; $i.Dispose() }
