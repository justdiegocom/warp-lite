# Instala better-warp para el usuario actual (sin administrador):
# copia el .exe a %LOCALAPPDATA%\Programs\BetterWarp y crea accesos en Inicio y escritorio.
$ErrorActionPreference = 'Stop'
$src = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'better-warp.exe'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\BetterWarp'
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $dest 'better-warp.exe'
if (Get-Process better-warp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
  throw 'better-warp está abierto: ciérralo y vuelve a instalar.'
}
Copy-Item -Force $src $exe
$shell = New-Object -ComObject WScript.Shell
$links = @(
  (Join-Path ([Environment]::GetFolderPath('Programs')) 'better-warp.lnk'),
  (Join-Path ([Environment]::GetFolderPath('Desktop')) 'better-warp.lnk')
)
foreach ($l in $links) {
  $s = $shell.CreateShortcut($l)
  $s.TargetPath = $exe
  $s.WorkingDirectory = $env:USERPROFILE
  $s.Description = 'Terminal con paneles al estilo Warp'
  $s.Save()
}
Write-Output "Instalado en $exe"
$links | ForEach-Object { Write-Output "Acceso: $_" }
