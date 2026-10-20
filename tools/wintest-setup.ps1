# Instalador: instala en silencio, comprueba ficheros/accesos/registro, desinstala y vuelve a comprobar.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$inst = "$env:LOCALAPPDATA\Programs\warp-lite"
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\warp-lite'
$links = @("$([Environment]::GetFolderPath('Programs'))\warp-lite.lnk", "$([Environment]::GetFolderPath('Desktop'))\warp-lite.lnk")
function State($t) {
  "$t | exe=$(Test-Path "$inst\warp-lite.exe") uninstall=$(Test-Path "$inst\uninstall.exe") accesos=$((($links | Where-Object { Test-Path $_ }).Count)) registro=$(Test-Path $key)"
}
Start-Process "$dir\warp-lite-setup.exe" -ArgumentList '--silent' -Wait
State 'tras instalar'
(Get-ItemProperty $key) | Select-Object DisplayName, DisplayVersion, UninstallString | Format-List | Out-String
Start-Process "$inst\uninstall.exe" -ArgumentList '--uninstall --silent' -Wait
Start-Sleep 6
State 'tras desinstalar'
