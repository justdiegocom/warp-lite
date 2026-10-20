# Buscar (Ctrl+F), selector de carpetas y relanzar al reabrir lo que estaba en marcha (ssh + tmux).
param([string]$Exe = "$PSScriptRoot\warp-lite.exe")
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-batch.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\fb-*.bmp"
function Run($script) {
  $p = Start-Process -FilePath $Exe -ArgumentList "--script `"$script`"" -PassThru
  if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Output "TIMEOUT" }
}
Run "wait|wait|wait|edit:cd C:\Windows|submit|edit:Get-ChildItem -Name|submit|wait|find:system|shot:$dir\fb-1-buscar.bmp|find-close|picker|picker-type:sys|shot:$dir\fb-2-carpetas.bmp|picker-enter|wait|shot:$dir\fb-3-cd.bmp|splitR|wait|wait|edit:ssh -t fr tmux new -A -s wl-resume-test|submit|wait|wait|wait|wait|shot:$dir\fb-4-tmux.bmp|quit"
Write-Output '--- sesión'; Get-Content $env:WARPLITE_SESSION
Run "wait|wait|wait|wait|wait|wait|shot:$dir\fb-5-reabierto.bmp|quit"
Get-ChildItem "$dir\fb-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
