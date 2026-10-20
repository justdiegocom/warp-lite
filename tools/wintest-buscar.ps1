# Find bar buttons: previous, next (wrapping around at the end) and × to close, clicked with the mouse.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_SESSION = "$dir\session-buscar.txt"
Remove-Item -Force -ErrorAction SilentlyContinue "$env:WARPLITE_SESSION*", "$dir\buscar.bmp"
$steps = @('wait', 'wait', 'wait', 'edit:"mar" + "ca1"; "mar" + "ca2"; "mar" + "ca3"', 'submit', 'wait', 'find:marca', 'find-state',
  "shot:$dir\buscar.bmp", 'find-click:prev', 'find-state', 'find-click:prev', 'find-state', 'find-click:next', 'find-state',
  'find-click:next', 'find-state', 'find-click:next', 'find-state', 'find-click:close', 'find-state', 'quit')
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); 'TIMEOUT' }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
# 3 results (the command line does not match), the newest selected first.
$want = @('find: open 3/3', 'find: open 2/3', 'find: open 1/3', 'find: open 2/3', 'find: open 3/3', 'find: open 1/3', 'find: closed 0/0')
$fail = 0
for ($i = 0; $i -lt $want.Count; $i++) {
  $ok = $i -lt $got.Count -and $got[$i] -eq $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { "$($got[$i]) (esperado $($want[$i]))" } else { "(falta) $($want[$i])" })
}
"fallos: $fail"
