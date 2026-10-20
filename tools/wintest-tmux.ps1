# Colores a través de ssh y de tmux (contra el servidor «fr» de ~/.ssh/config).
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:WARPLITE_SESSION = "$dir\session-tmux.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$dir\tmux-*.bmp"
$script = "wait|wait|wait|edit:ssh -t fr bash .cache/wl-colortest.sh|submit|wait|wait|wait|shot:$dir\tmux-1-ssh.bmp|wait|wait|wait|wait|edit:ssh -t fr tmux new -s wl-colortest bash .cache/wl-colortest.sh|submit|wait|wait|wait|shot:$dir\tmux-2-tmux.bmp|wait|wait|wait|wait|quit"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$script`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Output "TIMEOUT" }
Get-ChildItem "$dir\tmux-*.bmp" | ForEach-Object { "$($_.Name) $($_.Length)" }
