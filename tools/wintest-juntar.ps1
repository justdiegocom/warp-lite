# Joining windows (WARPLITE_DIR, like the real one): a tab or pane dragged out of a window and dropped on another window
# of the same Warp moves there, the way a tab dragged out moves to a new window. (1) A tab dropped on the other window's
# tabs becomes a tab there, where it was dropped, and what it was running (a fake ssh ... tmux, like the real one: it
# writes its arguments and stays) is launched again there. (2) Dropped on another pane of its own window, a pane is
# swapped as before; dragged over the other window, that one shows where it would go, and dropped on its pane it splits
# it on that side and its Claude Code (fake, like wintest-cerrar's) resumes its conversation. (3) Over a tab sidebar, the
# hint is on the tabs; the last tab of a window moved there: that window closes and its file is discarded. (4) A tab
# moved on its own window's tabs, as before; dropped on a window of another Warp (no target), it opens in a new window of
# its own Warp, as when dropped anywhere else, and the other Warp is untouched. (5) Windows overlapping: the one in view
# under the cursor takes it, also over the area of the one it comes from. Screenshots in juntar-*.bmp.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$wins = "$dir\wins-juntar"; $winsX = "$dir\wins-juntar-x"; $bin = "$dir\bin-juntar"; $log = "$dir\args-juntar.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude-juntar"
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $winsX, $bin, $log, $env:CLAUDE_CONFIG_DIR, "$dir\proj-juntar",
  "$dir\go-juntar-*.txt", "$dir\juntar-*.bmp"
New-Item -ItemType Directory $wins, $winsX, $bin, "$env:CLAUDE_CONFIG_DIR\sessions" | Out-Null
$proj = New-Item -ItemType Directory "$dir\proj-juntar"
# ssh and claude stand-ins: each start writes its arguments; claude also notes its conversation like the real one.
$fake = @'
$tool = $args[0]; $a = @($args | Select-Object -Skip 1)
Add-Content -Encoding UTF8 "$PSScriptRoot\..\args-juntar.txt" (("$tool " + ($a -join ' ')).Trim())
if ($tool -eq 'claude') {
  $model = 'x'; $res = ''
  for ($i = 0; $i -lt $a.Count; $i++) { if ($a[$i] -eq '--model') { $model = $a[$i + 1] }; if ($a[$i] -eq '--resume') { $res = $a[$i + 1] } }
  $id = if ($res) { $res } else { $model.PadLeft(8, '0') + '-0000-4000-8000-000000000000' }
  Set-Content "$env:CLAUDE_CONFIG_DIR\sessions\$PID.json" "{`"pid`":$PID,`"sessionId`":`"$id`",`"cwd`":`"x`"}"
}
Start-Sleep 300
'@
Set-Content -Encoding UTF8 "$bin\fake.ps1" $fake
foreach ($n in 'ssh', 'claude') { Set-Content -Encoding ASCII "$bin\$n.cmd" "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake.ps1`" $n %*" }
$env:PATH = "$bin;$env:PATH"
$m1 = '000000m1-0000-4000-8000-000000000000'
$cdir = New-Item -ItemType Directory -Force ("$env:CLAUDE_CONFIG_DIR\projects\" + ($proj.FullName -replace '[^A-Za-z0-9]', '-'))
Set-Content "$cdir\$m1.jsonl" '{}'

function Warp($wdir, $steps) {
  $env:WARPLITE_DIR = $wdir
  Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$(($steps -join '|').Replace('"', '\"'))`"" -PassThru
}
function Bench($wdir = $wins) { @(Get-ChildItem "$wdir\*.bench" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName }) }
function WaitFor($pat, $n = 1, $sec = 60, $wdir = $wins) {
  for ($t = 0; $t -lt $sec * 2; $t++) { if (@(@(Bench $wdir) -match $pat).Count -ge $n) { break }; Start-Sleep -Milliseconds 500 }
  @(@(Bench $wdir) -match $pat)[$n - 1]
}
function Launched { @(Get-Content -Encoding UTF8 $log -ErrorAction SilentlyContinue) }
function WaitLaunched($pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2 -and @(@(Launched) -like $pat).Count -lt $n; $t++) { Start-Sleep -Milliseconds 500 }
  @(@(Launched) -like $pat).Count
}
function WaitShot($name) { for ($t = 0; $t -lt 60 -and -not (Test-Path "$dir\juntar-$name.bmp"); $t++) { Start-Sleep -Milliseconds 500 } }
function Go($name) { New-Item -ItemType File -Force "$dir\go-juntar-$name.txt" | Out-Null }
function Wf($name) { "wait-file:$dir\go-juntar-$name.txt" }
function Files { @(Get-ChildItem "$wins\window-*.txt" -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object) }
function Alive { @(Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) }
function WaitNoneAlive($sec = 30) { for ($t = 0; $t -lt $sec * 2 -and (Alive).Count; $t++) { Start-Sleep -Milliseconds 500 } }
function Entry($line, $id) { if ($line -match "(^|\| |windows: )$id=([^|]*)") { $Matches[2].Trim() } else { '' } }  # one window of «windows»
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }

# B: the window things are dropped on (the main one, opened first), top right. X: another Warp (its own WARPLITE_DIR),
# bottom right. A: where they come from, top left. A2: a window with one tab, bottom left. P (later): over A.
$b = Warp $wins @('wait', 'rename:destino', 'move:512,0,500,380', 'wait', "shot:$dir\juntar-b-antes.bmp", (Wf 'b1'), 'windows',
  "shot:$dir\juntar-b-pestana.bmp", (Wf 'hint'), 'join-hint', "shot:$dir\juntar-b-aviso.bmp", (Wf 'b2'), 'windows',
  "shot:$dir\juntar-b-panel.bmp", 'vtabs', (Wf 'hint2'), 'join-hint', "shot:$dir\juntar-b-lateral.bmp", (Wf 'b3'), 'windows',
  (Wf 'b5'), 'windows', (Wf 'end'), 'quit-all')
Start-Sleep 4
$x = Warp $winsX @('wait', 'rename:otro-warp', 'move:512,390,500,370', (Wf 'end'), 'window-info', 'close')
$env:WARPLITE_CHILD_SCRIPT = 'wait|window-info'  # the window a tab dropped outside opens says what it is
$a = Warp $wins @('wait', 'rename:uno', 'move:0,0,500,380', 'tab', 'wait', 'rename:remoto', 'edit:ssh -t devbox "tmux new -A -s main"', 'submit',
  'wait', 'wait', "shot:$dir\juntar-a-antes.bmp", (Wf 'a1'), "drop-tab:1,$($b.Id),60,20", 'window-info',
  'splitR', 'wait', "edit:cd '$($proj.FullName)'", 'submit', 'wait', 'edit:claude --model m1', 'submit', 'wait', 'wait',
  (Wf 'a2'), 'drop-pane:1,self,120,205', 'where', 'drag-start:pane:0', "drag-to:$($b.Id),430,210", (Wf 'drop'), 'drag-drop', 'window-info',
  "shot:$dir\juntar-a-despues.bmp", 'tab', 'wait', 'rename:ajena', 'drag-tab:1,0', 'tabs', (Wf 'a4'), "drop-tab:0,$($x.Id),250,200", 'window-info',
  'tab', 'wait', 'rename:encima', (Wf 'a5'), 'drop-tab:1,0,368,120', 'window-info', (Wf 'end'))
Start-Sleep 3
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$a2 = Warp $wins @('wait', 'rename:solo', 'move:0,390,500,370', (Wf 'a3'), 'drag-start:tab:0', "drag-to:$($b.Id),100,50", (Wf 'a3drop'),
  "drag-to:$($b.Id),100,300", 'drag-drop', 'window-info', 'wait', 'wait', 'wait')

# 1. A's tab «remoto», running ssh ... tmux, dropped at the start of B's tabs.
$n = WaitLaunched 'ssh *'
Check ($n -eq 1) "ssh running in A's tab: $(@(Launched) -join ' / ')"
Start-Sleep 3  # what a pane runs is looked at every second
Go 'a1'
$wa = WaitFor '^window: other reopen=0 \[uno'
Check ($wa -eq 'window: other reopen=0 [uno]') "A without the tab: $wa"
$n = WaitLaunched 'ssh *' 2 30
$l = @(@(Launched) -like 'ssh *')
Check ($n -eq 2 -and $l[1] -eq $l[0] -and $l[0] -eq 'ssh -t devbox tmux new -A -s main') "launched again in B: $($l -join ' / ')"
Start-Sleep 3
Go 'b1'
$w = WaitFor '^windows: '
Check ((Entry $w $b.Id) -match '^\[remoto:1:ssh -t devbox "tmux new -A -s main"\] \[destino:1\]$') "B: the tab first, its ssh running: $(Entry $w $b.Id)"
Check ((Entry $w $a.Id) -match '^\[uno:\d\]$') "A, without it: $(Entry $w $a.Id)"
WaitShot 'b-pestana'

# 2. A's split: its Claude Code pane (right) dropped on its other pane: swapped. Then dragged over B's pane (right
#    half) and dropped there.
$n = WaitLaunched 'claude *'
Start-Sleep 3
Go 'a2'
$s = WaitFor '^where: '
$d = WaitFor '^drag-to: ' 2  # the first one was the tab's (drop-tab)
Check ($s -eq 'where: tab0/pane0' -and $d -match "join=none under=$($a.Id) outside=0$") "on its own other pane: swapped ($s; $d)"
$d = WaitFor '^drag-to: ' 3
Check ($d -match "join=$($b.Id) under=$($b.Id) outside=1$") "dragging over B: $d"
Go 'hint'
$h = WaitFor '^join-hint: '
Check ($h -eq 'join-hint: pane 0 right') "B shows where it would go: $h"
WaitShot 'b-aviso'
Go 'drop'
$wa = WaitFor '^window: other reopen=0 \[uno' 2
Check ($wa -eq 'window: other reopen=0 [uno]') "A without the pane: $wa"
$n = WaitLaunched 'claude *' 2 30
Check (@(Launched) -contains "claude --model m1 --resume $m1") "Claude Code resumed its conversation in B: $(@(Launched) -like 'claude *' -join ' / ')"
Start-Sleep 3
Go 'b2'
$w = WaitFor '^windows: ' 2
Check ((Entry $w $b.Id) -match "^\[remoto:2:ssh -t devbox `"tmux new -A -s main`",claude --model m1 --resume $m1\] \[destino:1\]$") "B: its pane split, Claude Code on the right: $(Entry $w $b.Id)"
WaitShot 'b-panel'

# 3. B with its tabs at the side. A2's only tab over them: the hint is on the tabs; dropped below them, A2 closes and
#    its file is discarded.
$filesBefore = @(Files)
Go 'a3'
$d = WaitFor '^drag-to: ' 4
Go 'hint2'
$h = WaitFor '^join-hint: ' 2
Check ($h -eq 'join-hint: tab 0') "B's tab sidebar shows where it would go: $h ($d)"
WaitShot 'b-lateral'
Go 'a3drop'
$gone = $a2.WaitForExit(20000)
Check $gone "A2 closed after its last tab moved"
Start-Sleep 1
Check (@(Files).Count -eq $filesBefore.Count - 1) "its window file discarded: $(@(Files) -join ', ') (before: $($filesBefore -join ', '))"
Go 'b3'
$w = WaitFor '^windows: ' 3
Check ((Entry $w $b.Id) -match '\[destino:1\] \[solo:1\]$' -and -not (Entry $w $a2.Id)) "B: the tab last: $(Entry $w $b.Id)"

# 4. A's tab «ajena» moved before «uno» on A's own tabs (as before), then dropped on X (another Warp): a new window of
#    A's Warp, X untouched.
$t = WaitFor '^tabs: '
Check ($t -eq 'tabs: *ajena uno') "on its own tabs: moved there ($t)"
Go 'a4'
$c = WaitFor '^window: other reopen=0 \[ajena\]'
Check ($c) "dropped on another Warp: a new window of its own: $c"
$wa = WaitFor '^window: other reopen=0 \[uno' 3
Check ($wa -eq 'window: other reopen=0 [uno]') "A without it: $wa"

# 5. P opens over A (in front of it, overlapping it): A's tab «encima» dropped on P's tabs where P covers A.
$p = Warp $wins @('wait', 'rename:encima-destino', 'move:300,100,500,380', 'window-info', (Wf 'end'))
$null = WaitFor '^window: other reopen=0 \[encima-destino\]'
Go 'a5'
$d = WaitFor '^drag-to: 368,120 '
Check ($d -match "join=$($p.Id) under=$($p.Id) outside=0$") "over A's area where P is in front: $d"
$wa = WaitFor '^window: other reopen=0 \[uno' 4
Check ($wa -eq 'window: other reopen=0 [uno]') "A without it: $wa"
Go 'b5'
$w = WaitFor '^windows: ' 4
Check ((Entry $w $p.Id) -eq '[encima:1] [encima-destino:1]') "P took it, first: $(Entry $w $p.Id)"

Go 'end'
$wx = WaitFor '^window: ' 1 30 $winsX
Check ($wx -eq 'window: main reopen=0 [otro-warp]') "the other Warp untouched: $wx"
foreach ($q in $b, $a, $x, $p) { if (-not $q.WaitForExit(30000)) { $q.Kill(); 'TIMEOUT' } }
WaitNoneAlive
Check ((Alive).Count -eq 0) "no test processes alive ($((Alive).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$bin*" } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $winsX, $bin, $log, $env:CLAUDE_CONFIG_DIR, "$dir\proj-juntar", "$dir\go-juntar-*.txt"
"fallos: $fail"
