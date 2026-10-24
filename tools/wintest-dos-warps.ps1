# Two Warps at once. (1) Two instances (different WARPLITE_DIR, like a test next to the real Warp): quit-all
# (Ctrl+Shift+Q) closes only the windows of its own Warp. First a dry run that sends nothing; if it would
# reach a window of another Warp, no quit-all is sent at all. (2) The shortcut opened twice at the same
# time: only one of the two windows becomes the main one. (3) Two Warps opened one after the other (the
# shortcut twice), each closed with its X: opening Warp again brings back only the first one, the main one.
# (4) Every window of a Warp lives in its first process: a second start (the shortcut, «Open Warp here» with --cd)
# hands its request to it and ends at once, never to the other Warp; --cd opens a new tab in that folder in its window
# in front.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$winsA = "$dir\wins-a"; $winsB = "$dir\wins-b"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $winsA, $winsB
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
function Warp($wins, $steps) {
  $env:WARPLITE_DIR = $wins
  Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps -join '|')`"" -PassThru
}
function Bench($wins) { @(Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName }) }
function WaitFor($wins, $pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2; $t++) { if (@(@(Bench $wins) -match $pat).Count -ge $n) { return }; Start-Sleep -Milliseconds 500 }
}
function Files($wins) { @(Get-ChildItem "$wins\window-*.txt" -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object) }
function TabNames($f) {
  @(Get-Content $f -ErrorAction SilentlyContinue | Where-Object { $_ -like 'tab *' } | ForEach-Object {
    $h = ($_ -split ' ')[2]
    if ($h -eq '-') { '' } else { -join ($h -split '(..)' | Where-Object { $_ } | ForEach-Object { [char][Convert]::ToInt32($_, 16) }) }
  })
}
function Alive { @(Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) }
function WaitAll($procs, $ms = 90000) { foreach ($p in $procs) { if (-not $p.WaitForExit($ms)) { $p.Kill(); 'TIMEOUT' } } }
function WaitNoneAlive($sec = 30) { for ($t = 0; $t -lt $sec * 2 -and (Alive).Count; $t++) { Start-Sleep -Milliseconds 500 } }
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }
$w = @('wait') * 24

# 1. Warp B open; Warp A with two windows. Dry run in A: quit-all would reach one other window of A and
#    none of B (nor of the real Warp, if it is open). Then quit-all from a third window of A.
$b = Warp $winsB (@('wait', 'rename:otro-warp') + $w + @('window-info', 'quit'))
Start-Sleep 4
$a1 = Warp $winsA @('wait', 'rename:a-principal', 'window-info')
Start-Sleep 4
$a2 = Warp $winsA @('wait', 'rename:a-segunda', 'window-info', 'quit-all-targets')
$handed = $a2.WaitForExit(15000)
Check ($handed -and $a2.ExitCode -eq 0 -and (Alive).Count -eq 2) "A's second start handed its window to A's process and ended ($((Alive).Count) processes: A and B)"
WaitFor $winsA '^quit-all-targets'
$dry = @(@(Bench $winsA) -match '^quit-all-targets')[0]
Check ($dry -match '^quit-all-targets: 1 of this Warp, [1-9]\d* of others$') "dry run in A: $dry"
if ($dry -notmatch '^quit-all-targets: 1 of this Warp,') {
  'quit-all could reach windows of another Warp: not sent, test stopped'
  Alive | Stop-Process -Force -ErrorAction SilentlyContinue  # only this test's (warp-lite.exe in its folder)
  "fallos: $fail"
  exit
}
$a3 = Warp $winsA @('wait', 'quit-all')
WaitAll $a1, $a2, $a3 30000
Check (-not $b.HasExited) 'quit-all in A: Warp B is still open'
Check ("$(Files $winsA)" -eq 'window-main.txt') "A after quit-all: only the main window saved ($(Files $winsA))"
WaitAll $b
$bb = @(Bench $winsB)
Check ($bb -contains 'window: main reopen=0 [otro-warp]') "B kept working after A's quit-all: $bb"
$ab = @(Bench $winsA)
Check (($ab -contains 'window: main reopen=0 [a-principal]') -and ($ab -contains 'window: other reopen=0 [a-segunda]')) "A: $($ab -join ' / ')"

# 2. The shortcut opened twice at the same time: one main window and one other.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $winsA
$p1 = Warp $winsA @('wait', 'window-info', 'wait', 'close')
$p2 = Warp $winsA @('wait', 'window-info', 'wait', 'close')
WaitAll $p1, $p2
$d = @(Bench $winsA)
Check ((@($d -like 'window: main *').Count -eq 1) -and (@($d -like 'window: other *').Count -eq 1)) "double start: $($d -join ' / ')"
Check ("$(Files $winsA)" -eq 'window-main.txt') "double start, both closed with their X: only the main one saved ($(Files $winsA))"

# 3. Two Warps opened one after the other and closed each with its X; then Warp once: only the first one.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $winsA
$p1 = Warp $winsA (@('wait', 'rename:hellomatik', 'edit:cd C:\Windows', 'submit') + @('wait') * 10 + @('close'))
Start-Sleep 4
$p2 = Warp $winsA (@('wait', 'rename:proyectos', 'edit:cd C:\Users', 'submit') + @('wait') * 3 + @('close'))
WaitAll $p1, $p2
Check ("$(Files $winsA)" -eq 'window-main.txt') "saved after closing both: $(Files $winsA)"
Check ("$(TabNames "$winsA\window-main.txt")" -eq 'hellomatik') "the main one is the first: [$(TabNames "$winsA\window-main.txt")]"
Remove-Item "$winsA\*.bench" -ErrorAction SilentlyContinue
$env:WARPLITE_CHILD_SCRIPT = 'window-info|close'  # a window that should not come back would say so
$p3 = Warp $winsA @('wait', 'wait', 'window-info', 'close')
WaitAll $p3
Start-Sleep 6
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$r = @(Bench $winsA)
Check ("$r" -eq 'window: main reopen=0 [hellomatik]') "opened again: $($r -join ' / ')"

# 4. «Open Warp here» with Warp open: a new tab in that folder, in A's window; the starts end at once.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $winsA
$p1 = Warp $winsA @('wait', 'rename:a-cd', 'window-info', "wait-file:$dir\go-dos-cd.txt", 'wait', 'active', 'windows', 'close')
WaitFor $winsA '^window: main reopen=0 \[a-cd\]'
$env:WARPLITE_DIR = $winsA
$cd = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList '--cd "C:\Windows"' -PassThru
$ended = $cd.WaitForExit(15000)
Check ($ended -and $cd.ExitCode -eq 0 -and (Alive).Count -eq 1) "--cd handed to the running Warp, ended at once ($((Alive).Count) process)"
New-Item -ItemType File -Force "$dir\go-dos-cd.txt" | Out-Null
WaitAll $p1
$d = @(Bench $winsA | Where-Object { $_ -notlike 'window: *' })
Check (($d -match '^active: tab 1 \[.*\] C:\\Windows').Count -eq 1 -and ($d -match '^windows: w\d+=\[a-cd:1\] \[[^\]]*:1\]$').Count -eq 1) "a new tab in C:\Windows, in that window: $($d -join ' / ')"
Remove-Item -Force -ErrorAction SilentlyContinue "$dir\go-dos-cd.txt"

WaitNoneAlive
Check ((Alive).Count -eq 0) "no test processes alive ($((Alive).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $winsA, $winsB
"fallos: $fail"
