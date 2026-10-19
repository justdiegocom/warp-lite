# Instala better-warp para el usuario actual (sin administrador):
# copia el .exe a %LOCALAPPDATA%\Programs\BetterWarp y crea accesos en Inicio y escritorio.
$ErrorActionPreference = 'Stop'
$src = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'better-warp.exe'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\BetterWarp'
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $dest 'better-warp.exe'
# Si está abierto, el .exe en uso se puede renombrar (no sobrescribir): la versión nueva
# se usa la próxima vez que se abra.
$old = Join-Path $dest 'better-warp.old.exe'
Remove-Item -Force -ErrorAction SilentlyContinue $old
if (Test-Path $old) { $old = Join-Path $dest ("better-warp.old-{0}.exe" -f (Get-Date -Format yyyyMMddHHmmss)) }
if (Get-Process better-warp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
  Rename-Item $exe (Split-Path -Leaf $old)
  Write-Output 'better-warp estaba abierto: la versión nueva se usará al volver a abrirlo.'
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
