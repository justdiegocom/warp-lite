# Instala warp-lite para el usuario actual (sin administrador):
# copia el .exe a %LOCALAPPDATA%\Programs\warp-lite y crea accesos en Inicio y escritorio.
$ErrorActionPreference = 'Stop'
$src = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'warp-lite.exe'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\warp-lite'
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $dest 'warp-lite.exe'
# Si está abierto, el .exe en uso se puede renombrar (no sobrescribir): la versión nueva
# se usa la próxima vez que se abra.
$old = Join-Path $dest 'warp-lite.old.exe'
Remove-Item -Force -ErrorAction SilentlyContinue $old
if (Test-Path $old) { $old = Join-Path $dest ("warp-lite.old-{0}.exe" -f (Get-Date -Format yyyyMMddHHmmss)) }
if (Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
  Rename-Item $exe (Split-Path -Leaf $old)
  Write-Output 'warp-lite estaba abierto: la versión nueva se usará al volver a abrirlo.'
}
Copy-Item -Force $src $exe
$shell = New-Object -ComObject WScript.Shell
$links = @(
  (Join-Path ([Environment]::GetFolderPath('Programs')) 'warp-lite.lnk'),
  (Join-Path ([Environment]::GetFolderPath('Desktop')) 'warp-lite.lnk')
)
foreach ($l in $links) {
  $s = $shell.CreateShortcut($l)
  $s.TargetPath = $exe
  $s.WorkingDirectory = $env:USERPROFILE
  $s.Description = 'Terminal con paneles al estilo Warp'
  $s.Save()
}
# Antes se llamaba better-warp: quita sus accesos y, si no está en uso, su carpeta.
foreach ($l in @('Programs', 'Desktop')) {
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path ([Environment]::GetFolderPath($l)) 'better-warp.lnk')
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $env:LOCALAPPDATA 'Programs\BetterWarp')
Write-Output "Instalado en $exe"
$links | ForEach-Object { Write-Output "Acceso: $_" }
