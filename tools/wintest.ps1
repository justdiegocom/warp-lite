# Prueba de humo en Windows: dos arranques seguidos con guion y capturas.
# 1) divide en 3 paneles y ejecuta una orden en cada uno; sale (guarda sesión)
# 2) reabre: debe recuperar la disposición y dejar escrita la última orden de cada panel
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session.txt"
Remove-Item -Force -ErrorAction SilentlyContinue "$dir\session.txt", "$dir\*.bmp"
function Run($script) {
  $p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit(60000)) { $p.Kill(); Write-Output "TIMEOUT" }
}
Run "type:echo hola desde el panel 1|enter|tab|type:git --version|enter|splitR|type:Get-Date|enter|splitR|type:dir|enter|splitR|shot:$dir\0a-cuatro-iguales.bmp|close-pane|shot:$dir\0b-tres-iguales.bmp|splitD|type:echo frhm1|enter|shot:$dir\1-antes.bmp|quit"
Write-Output "--- session.txt"; Get-Content "$dir\session.txt"
Run "wait|shot:$dir\2-reabierto.bmp|swap-left|wait|shot:$dir\3-movido.bmp|quit"
Get-ChildItem "$dir\*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
