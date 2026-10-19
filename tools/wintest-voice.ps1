# Prueba del dictado contra un servidor que imita a Wispr (wispr_url en config.ini).
#   wintest-voice.ps1 <url-del-mock>   (tono.wav junto a este script)
param([string]$MockUrl)
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:BETTERWARP_SESSION = "$dir\session-voz.txt"
Remove-Item -Force -ErrorAction SilentlyContinue "$dir\session-voz.txt", "$dir\voz-*.bmp", "$dir\*.ini"
function Run($script) {
  $p = Start-Process -FilePath "$dir\better-warp.exe" -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit(60000)) { $p.Kill(); Write-Output "TIMEOUT" }
}
# 1) Con clave: el WAV va al "Wispr" de prueba y el texto queda escrito en el prompt.
$env:BETTERWARP_CONFIG = "$dir\con-clave.ini"
Set-Content -Encoding ascii $env:BETTERWARP_CONFIG "wispr_api_key=clave-de-prueba`r`nwispr_url=$MockUrl`r`nvoice_language=es"
Run "wait|voice-wav:$dir\tono.wav|shot:$dir\voz-1-dictado.bmp|quit"
# 2) Sin clave: el micrófono avisa de dónde ponerla.
$env:BETTERWARP_CONFIG = "$dir\sin-clave.ini"
Run "wait|mic|shot:$dir\voz-2-sin-clave.bmp|quit"
Write-Output "--- plantilla creada:"; Get-Content "$dir\sin-clave.ini"
Get-ChildItem "$dir\voz-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
