# Moving tabs and panes between the windows of one Warp (WARPLITE_DIR, like the real one): all of them live in one
# process, so what moves keeps running as it was: the same shell (same pid), what runs in it (a fake ssh ... tmux and a
# fake Claude Code, like the real ones: they log each start and stay) is never launched again, and its scrollback goes
# with it. (1) A tab dropped on another window's tabs becomes a tab there, where it was dropped. (2) Dropped on another
# pane of its own window, a pane is swapped as before; dragged over the other window, that one shows where it would go,
# and dropped on its pane it splits it on that side. (3) Over a tab sidebar, the hint is on the tabs; the last tab of a
# window moved there: that window closes and its file is discarded. (4) A tab moved on its own window's tabs, as before;
# dropped on a window of another Warp (no target), it opens in a new window of its own Warp, as when dropped anywhere
# else, and the other Warp is untouched. (5) Windows overlapping: the one in view under the cursor takes it, also over
# the area of the one it comes from. What moves keeps its attention pulse and what is typed but not sent in its input.
# (6) A tab with a live counter (a child process printing a number every 250 ms)
# dragged out to a new window and back: the counter goes on, the same process, its first lines still there.
# Screenshots in juntar-*.bmp; the frames of (6) in juntar-f*.bmp (as a desk: every window where it is).
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$wins = "$dir\wins-juntar"; $winsX = "$dir\wins-juntar-x"; $winsC = "$dir\wins-juntar-c"
$bin = "$dir\bin-juntar"; $log = "$dir\args-juntar.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude-juntar"
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $winsX, $winsC, $bin, $log, $env:CLAUDE_CONFIG_DIR, "$dir\proj-juntar",
  "$dir\go-juntar-*.txt", "$dir\juntar-*.bmp"
New-Item -ItemType Directory $wins, $winsX, $winsC, $bin, "$env:CLAUDE_CONFIG_DIR\sessions" | Out-Null
$proj = New-Item -ItemType Directory "$dir\proj-juntar"
# ssh and claude stand-ins: each start writes its arguments; claude also notes its conversation like the real one.
$fake = @'
$tool = $args[0]; $a = @($args | Select-Object -Skip 1)
# Several start at once (a restore): the log is shared, so a write that finds it busy tries again.
for ($k = 0; $k -lt 100; $k++) { try { Add-Content -Encoding UTF8 "$PSScriptRoot\..\args-juntar.txt" (("$tool " + ($a -join ' ')).Trim()) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 30 } }
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
# The counter of (6): a child process of the pane's shell, its own pid, a line every 250 ms.
Set-Content -Encoding ASCII "$bin\contador.cmd" "@powershell -NoProfile -Command `"for (`$i = 0;; `$i++) { Write-Host ('count-86 ' + `$i); Start-Sleep -Milliseconds 250 }`""
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
function Entry($line, $id) { if ($line -match "(^|\| |: )$id=([^|]*)") { $Matches[2].Trim() } else { '' } }  # one window of «windows»
function Pid1($line, $key) { if ($line -match " $([regex]::Escape($key))=(\d+)") { [int]$Matches[1] } else { 0 } }  # one pane of «pids»
function Counter { @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like '*count-86*' }) }
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }

# B: the window things are dropped on (the main one, opened first), top right. X: another Warp (its own WARPLITE_DIR),
# bottom right. A: where they come from, top left. A2: a window with one tab, bottom left. P (later): over A. Every one
# after B is a second start of B's Warp: it hands its window to B's process and ends.
$b = Warp $wins @('wait', 'label:B', 'rename:destino', 'move:512,0,500,380', 'wait', "shot:$dir\juntar-b-antes.bmp", (Wf 'b1'), 'windows', 'pids',
  'grep:marker-remoto-86', 'attention', "shot:$dir\juntar-b-pestana.bmp", (Wf 'hint'), 'join-hint', "shot:$dir\juntar-b-aviso.bmp", (Wf 'b2'),
  'windows', 'pids', 'grep:proj-juntar', "shot:$dir\juntar-b-panel.bmp", 'vtabs', (Wf 'hint2'), 'join-hint', "shot:$dir\juntar-b-lateral.bmp", (Wf 'b3'), 'windows',
  'window-count', (Wf 'b5'), 'windows', (Wf 'end'), 'quit-all')
Start-Sleep 4
$x = Warp $winsX @('wait', 'rename:otro-warp', 'move:512,390,500,370', (Wf 'end'), 'window-info', 'close')
$env:WARPLITE_CHILD_SCRIPT = 'wait|window-info|edit-text'  # the window a tab dropped outside opens says what it is
$a = Warp $wins @('wait', 'label:A', 'rename:uno', 'move:0,0,500,380', 'tab', 'wait', 'rename:remoto', 'edit:Write-Output marker-remoto-86', 'submit',
  'edit:ssh -t devbox "tmux new -A -s main"', 'submit', 'wait', 'wait', "shot:$dir\juntar-a-antes.bmp", 'pids',
  'goto:0', 'feed:1:1b5d393b7807', 'notes', (Wf 'a1'), 'drop-tab:1,B,60,20', 'window-info',
  'splitR', 'wait', "edit:cd '$($proj.FullName)'", 'submit', 'wait', 'edit:claude --model m1', 'submit', 'wait', 'wait', 'pids',
  (Wf 'a2'), 'drop-pane:1,self,120,205', 'where', 'drag-start:pane:0', 'drag-to:B,430,210', (Wf 'drop'), 'drag-drop', 'window-info',
  "shot:$dir\juntar-a-despues.bmp", 'tab', 'wait', 'rename:ajena', 'edit:sin-enviar-86', 'drag-tab:1,0', 'tabs', (Wf 'a4'), "drop-tab:0,$($x.Id),250,200", 'window-info',
  'tab', 'wait', 'rename:encima', (Wf 'a5'), 'drag-start:tab:1', (Wf 'a5b'), 'drag-to:0,368,120', 'drag-drop', 'window-info', (Wf 'end'))
$aExit = $a.WaitForExit(15000)
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
Check ($aExit -and $a.ExitCode -eq 0 -and (Alive).Count -eq 2) "a second start hands its window to the running Warp and ends ($((Alive).Count) processes: B's and X's)"
$a2 = Warp $wins @('wait', 'label:A2', 'rename:solo', 'move:0,390,500,370', (Wf 'a3'), 'drag-start:tab:0', 'drag-to:B,100,50', (Wf 'a3drop'),
  'drag-to:B,100,300', 'drag-drop', 'wait', 'wait', 'wait')

# 1. A's tab «remoto», running ssh ... tmux, dropped at the start of B's tabs: the same shell, ssh not launched again,
#    its scrollback there.
$n = WaitLaunched 'ssh *'
Check ($n -eq 1) "ssh running in A's tab: $(@(Launched) -join ' / ')"
$p0 = WaitFor '^pids: '
Start-Sleep 3  # what a pane runs is looked at every second
Go 'a1'
$wa = WaitFor '^window: other reopen=0 \[uno'
Check ($wa -eq 'window: other reopen=0 [uno]') "A without the tab: $wa"
Start-Sleep 4
Check (@(@(Launched) -like 'ssh *').Count -eq 1) "ssh not launched again: $(@(Launched) -join ' / ')"
Go 'b1'
$w = WaitFor '^windows: '
Check ((Entry $w 'B') -match '^\[remoto:1:ssh -t devbox "tmux new -A -s main"\] \[destino:1\]$') "B: the tab first, its ssh running: $(Entry $w 'B')"
Check ((Entry $w 'A') -match '^\[uno:\d\]$') "A, without it: $(Entry $w 'A')"
$p1 = WaitFor '^pids:.* B/remoto/0=\d+'
$before = Pid1 $p0 'A/remoto/0'; $after = Pid1 $p1 'B/remoto/0'
Check ($before -gt 0 -and $before -eq $after -and (Get-Process -Id $after -ErrorAction SilentlyContinue)) "the same shell in B: pid $before -> $after"
$g = WaitFor '^grep marker-remoto-86:'
Check ($g -match ' B/remoto/0=\d+\[') "its scrollback came with it: $g"
$nt = WaitFor '^notes: '
$at = WaitFor '^attention: '
Check ($nt -eq 'notes: alert:counted' -and $at -match '^attention: tab0/pane0=1 tab1/pane0=0 ') "a program said it finished while it wasn't in view: it pulses in B too ($nt; $at)"
WaitShot 'b-pestana'

# 2. A's split: its Claude Code pane (right) dropped on its other pane: swapped. Then dragged over B's pane (right
#    half) and dropped there: the same Claude Code, not resumed or launched again.
$n = WaitLaunched 'claude *'
$p2 = WaitFor '^pids:.* A/uno/1=\d+'
Start-Sleep 3
Go 'a2'
$s = WaitFor '^where: '
$d = WaitFor '^drag-to: ' 2  # the first one was the tab's (drop-tab)
Check ($s -eq 'where: tab0/pane0' -and $d -match 'join=none under=A outside=0$') "on its own other pane: swapped ($s; $d)"
$d = WaitFor '^drag-to: ' 3
Check ($d -match 'join=B under=B outside=1$') "dragging over B: $d"
Go 'hint'
$h = WaitFor '^join-hint: '
Check ($h -eq 'join-hint: pane 0 right') "B shows where it would go: $h"
WaitShot 'b-aviso'
Go 'drop'
$wa = WaitFor '^window: other reopen=0 \[uno' 2
Check ($wa -eq 'window: other reopen=0 [uno]') "A without the pane: $wa"
Start-Sleep 4
Check ("$(@(Launched) -like 'claude *')" -eq 'claude --model m1') "Claude Code not launched again: $(@(Launched) -like 'claude *' -join ' / ')"
Go 'b2'
$w = WaitFor '^windows: ' 2
Check ((Entry $w 'B') -match '^\[remoto:2:ssh -t devbox "tmux new -A -s main",claude --model m1\] \[destino:1\]$') "B: its pane split, Claude Code on the right: $(Entry $w 'B')"
$p3 = WaitFor '^pids:.* B/remoto/1=\d+'
$before = Pid1 $p2 'A/uno/1'; $after = Pid1 $p3 'B/remoto/1'
Check ($before -gt 0 -and $before -eq $after) "the same shell for Claude Code: pid $before -> $after"
$g = WaitFor '^grep proj-juntar:'
Check ($g -match ' B/remoto/1=\d+\[' -and $g -notmatch ' B/remoto/0=') "the pane's scrollback came with it: $g"
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
Start-Sleep 4
Go 'b3'
$w = WaitFor '^windows: ' 3
$c = WaitFor '^window-count: '
Check ((Entry $w 'B') -match '\[destino:1\] \[solo:1\]$' -and -not (Entry $w 'A2') -and $c -eq 'window-count: 2') "B: the tab last, A2 closed: $(Entry $w 'B') ($c)"
Check (@(Files).Count -eq $filesBefore.Count - 1) "its window file discarded: $(@(Files) -join ', ') (before: $($filesBefore -join ', '))"

# 4. A's tab «ajena» moved before «uno» on A's own tabs (as before), then dropped on X (another Warp): a new window of
#    A's Warp, X untouched.
$t = WaitFor '^tabs: '
Check ($t -eq 'tabs: *ajena uno') "on its own tabs: moved there ($t)"
Go 'a4'
$c = WaitFor '^window: other reopen=0 \[ajena\]'
Check ($c) "dropped on another Warp: a new window of its own: $c"
$e = WaitFor '^edit: '
Check ($e -eq 'edit: [sin-enviar-86]') "what was typed but not sent came with it: $e"
$wa = WaitFor '^window: other reopen=0 \[uno' 3
Check ($wa -eq 'window: other reopen=0 [uno]') "A without it: $wa"

# 5. P opens over A (in front of it, overlapping it): A's tab «encima» dropped on P's tabs where P covers A.
$p = Warp $wins @('wait', 'label:P', 'rename:encima-destino', 'move:300,100,500,380', 'window-info', (Wf 'p5'), 'raise', (Wf 'end'))
$null = WaitFor '^window: other reopen=0 \[encima-destino\]'
# Pressing the mouse on A's tab brings A to front, as a click does; P comes back over it before the drag goes on.
Go 'a5'
Start-Sleep 2
Go 'p5'
Start-Sleep 2
Go 'a5b'
$d = WaitFor '^drag-to: 368,120 '
Check ($d -match 'join=P under=P outside=0$') "over A's area where P is in front: $d"
$wa = WaitFor '^window: other reopen=0 \[uno' 4
Check ($wa -eq 'window: other reopen=0 [uno]') "A without it: $wa"
Go 'b5'
$w = WaitFor '^windows: ' 4
Check ((Entry $w 'P') -eq '[encima:1] [encima-destino:1]') "P took it, first: $(Entry $w 'P')"

Go 'end'
$wx = WaitFor '^window: ' 1 30 $winsX
Check ($wx -eq 'window: main reopen=0 [otro-warp]') "the other Warp untouched: $wx"
foreach ($q in $b, $x) { if (-not $q.WaitForExit(30000)) { $q.Kill(); 'TIMEOUT' } }
WaitNoneAlive

# 6. A live counter dragged out and back (its own Warp, WARPLITE_DIR wins-juntar-c). C: the window, top left; the tab
#    goes out to the right (N, a new window, which runs the child script: it drags it back onto C's tabs).
$area = '0,0,1280,640'
$frames = 0
function Frame { $script:frames++; 'desk-shot:{0},{1}\juntar-f{2:d3}.bmp' -f $area, $dir, $script:frames }
# The frames in the order they are taken: C before and while it drags the tab out, both windows after; N dragging it
# back; C after.
$cOut = @('wait', 'label:C', 'rename:main', 'move:10,10,620,300', 'edit:cd C:\', 'submit', 'tab', 'wait', 'rename:contador', 'edit:cd C:\', 'submit',
  'edit:contador', 'submit', 'wait', 'wait',
  'pids', 'grep:count-86 ', (Frame), 'wait', (Frame), 'wait', (Frame), (Wf 'out'), 'drag-start:tab:1', (Frame)) +
  (@(1..6) | ForEach-Object { "drag-to:0,$(308 + 75 * $_),$(30 + 52 * $_)"; (Frame) }) +
  @('drag-drop', (Frame), 'wait', (Frame), 'wait', (Frame), 'wait', (Frame), 'window-count')
$nBack = @('wait', 'label:N', 'pids', 'grep:count-86 ', (Wf 'back'), 'drag-start:tab:0', (Frame)) +
  (@(1..6) | ForEach-Object { "drag-to:C,$(640 - 90 * $_),$(320 - 50 * $_)"; (Frame) }) + @('drag-to:C,90,20', (Frame), 'drag-drop')
$cBack = @((Wf 'joined'), (Frame), 'wait', (Frame), 'pids', 'grep:count-86 ', 'windows', 'wait', (Frame), 'wait', 'grep:count-86 ', (Frame), (Wf 'endc'), 'quit-all')
$env:WARPLITE_CHILD_SCRIPT = $nBack -join '|'
$cSteps = $cOut + $cBack
$c = Warp $winsC $cSteps
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
# Each window's results are in its own file (N's sorts before C's): lines are picked by the panes they name.
$g0 = WaitFor '^grep count-86 :.* C/contador/0=' 1 60 $winsC
$p0 = WaitFor '^pids:.* C/contador/0=' 1 60 $winsC
$k0 = @(Counter)
Check ($k0.Count -eq 1 -and $g0 -match ' C/contador/0=\d+\[count-86 0\]') "C: the counter running ($($k0.Count) process, $g0)"
Go 'out'
$g1 = WaitFor '^grep count-86 :.* N/contador/0=' 1 60 $winsC
$p1 = WaitFor '^pids:.* N/contador/0=' 1 60 $winsC
$k1 = @(Counter)
$first = if ($g0 -match '\[count-86 (\d+)\]$') { [int]$Matches[1] } else { -1 }
$moved = if ($g1 -match 'N/contador/0=\d+\[count-86 0\]\[count-86 (\d+)\]') { [int]$Matches[1] } else { -1 }
Check ($moved -gt $first -and $first -ge 0) "in the new window it goes on from where it was, its first line still there: $g1"
Check ($k1.Count -eq 1 -and $k1[0].ProcessId -eq $k0[0].ProcessId) "the same counter process: $($k0.ProcessId) -> $($k1.ProcessId)"
$shell = Pid1 $p0 'C/contador/0'
Check ($shell -gt 0 -and $shell -eq (Pid1 $p1 'N/contador/0')) "the same shell: $p0 / $p1"
$cnt = WaitFor '^window-count: ' 1 60 $winsC
Check ($cnt -eq 'window-count: 2') "two windows now: $cnt"
Start-Sleep 2
Go 'back'
Start-Sleep 6
Go 'joined'
$w = WaitFor '^windows: ' 1 60 $winsC
$g2 = WaitFor '^grep count-86 :.* C/contador/0=' 2 60 $winsC
$p2 = WaitFor '^pids:.* C/contador/0=' 2 60 $winsC
$g3 = WaitFor '^grep count-86 :.* C/contador/0=' 3 60 $winsC
$k2 = @(Counter)
$back = if ($g2 -match 'C/contador/0=\d+\[count-86 0\]\[count-86 (\d+)\]') { [int]$Matches[1] } else { -1 }
$later = if ($g3 -match 'C/contador/0=\d+\[count-86 0\]\[count-86 (\d+)\]') { [int]$Matches[1] } else { -1 }
Check ($w -match '^windows: C=\[main:1\] \[contador:1:contador\]$' -or $w -match '^windows: C=\[contador:1:contador\] \[main:1\]$') "back in C, N closed: $w"
Check ($back -gt $moved -and $later -gt $back) "it goes on counting back in C ($moved -> $back -> $later)"
Check ($k2.Count -eq 1 -and $k2[0].ProcessId -eq $k0[0].ProcessId -and $shell -gt 0 -and (Pid1 $p2 'C/contador/0') -eq $shell) "the same counter process and shell after joining it back ($($k2.ProcessId); $p2)"
Go 'endc'
if (-not $c.WaitForExit(30000)) { $c.Kill(); 'TIMEOUT' }
for ($t = 0; $t -lt 20 -and @(Counter).Count; $t++) { Start-Sleep -Milliseconds 500 }
Check (@(Counter).Count -eq 0) "the counter ended with its window"
Check ((Test-Path "$dir\juntar-f001.bmp") -and $frames -ge 20) "$frames frames of the counter moving out and back"

WaitNoneAlive
Check ((Alive).Count -eq 0) "no test processes alive ($((Alive).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$bin*" -or $_.CommandLine -like '*count-86*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $winsX, $winsC, $bin, $log, $env:CLAUDE_CONFIG_DIR, "$dir\proj-juntar", "$dir\go-juntar-*.txt"
"fallos: $fail"
