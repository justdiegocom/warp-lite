# Which windows come back when Warp opens again (WARPLITE_DIR, like the real one): the main window (the
# first one) always; the others only with «Reopen on restart» (reopen:on, as in the title-bar menu),
# whether closed with their X or with quit-all (Ctrl+Shift+Q); after an update restart all of them, and
# they keep their setting. Also: a tab moved out to a new window keeps its Claude Code running (fake
# claude, like the real one: its conversation in sessions\<pid>.json), never launched again; windows saved by a
# version before the main window; a main window whose file is gone; the main window closed with its last tab.
# Every window of a Warp is in one process: a second start hands its window to it and ends.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$wins = "$dir\wins"
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_DIR = $wins
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude"
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, "$dir\fake-claude", "$dir\proj", "$dir\bin", "$dir\args.txt"
$proj = New-Item -ItemType Directory "$dir\proj"
New-Item -ItemType Directory "$dir\bin", "$env:CLAUDE_CONFIG_DIR\sessions" | Out-Null
$fake = @'
$a = @($args); $model = 'x'; $res = ''
for ($i = 0; $i -lt $a.Count; $i++) {
  if ($a[$i] -eq '--model') { $model = $a[$i + 1] }
  if ($a[$i] -eq '--resume') { $res = $a[$i + 1] }
}
# Several start at once (a restore): the log is shared, so a write that finds it busy tries again.
for ($k = 0; $k -lt 100; $k++) { try { Add-Content -Encoding UTF8 "$PSScriptRoot\..\args.txt" ('claude ' + ($a -join ' ')) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 30 } }
$id = if ($res) { $res } else { $model.PadLeft(8, '0') + '-0000-4000-8000-000000000000' }
Set-Content "$env:CLAUDE_CONFIG_DIR\sessions\$PID.json" "{`"pid`":$PID,`"sessionId`":`"$id`",`"cwd`":`"x`"}"
Start-Sleep 300
'@
Set-Content -Encoding UTF8 "$dir\bin\fake.ps1" $fake
Set-Content -Encoding ASCII "$dir\bin\claude.cmd" "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake.ps1`" %*"
$env:PATH = "$dir\bin;$env:PATH"
$m1 = '000000m1-0000-4000-8000-000000000000'
$cdir = New-Item -ItemType Directory -Force ("$env:CLAUDE_CONFIG_DIR\projects\" + ($proj.FullName -replace '[^A-Za-z0-9]', '-'))
Set-Content "$cdir\$m1.jsonl" '{}'

function Warp($steps) { Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps -join '|')`"" -PassThru }
function Bench { @(Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName }) }
function WaitFor($pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2; $t++) { if (@(@(Bench) -match $pat).Count -ge $n) { return }; Start-Sleep -Milliseconds 500 }
}
function Files { @(Get-ChildItem "$wins\window-*.txt" -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object) }
function Hex([string]$t) { -join ([Text.Encoding]::UTF8.GetBytes($t) | ForEach-Object { $_.ToString('x2') }) }
function TabNames($f) {
  @(Get-Content $f -ErrorAction SilentlyContinue | Where-Object { $_ -like 'tab *' } | ForEach-Object {
    $h = ($_ -split ' ')[2]
    if ($h -eq '-') { '' } else { -join ($h -split '(..)' | Where-Object { $_ } | ForEach-Object { [char][Convert]::ToInt32($_, 16) }) }
  })
}
function Alive { @(Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) }
Add-Type -Namespace WarpTest -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
public static int Count(uint pid) {
  int n = 0;
  EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var s = new System.Text.StringBuilder(32); GetClassName(h, s, 32);
                          if (p == pid && s.ToString() == "WarpLite") n++; return true; }, System.IntPtr.Zero);
  return n;
}
'@
function Windows { $n = 0; foreach ($p in Alive) { $n += [WarpTest.Win]::Count([uint32]$p.Id) }; $n }  # Warp windows open in this test
function WaitAll($procs, $ms = 90000) { foreach ($p in $procs) { if (-not $p.WaitForExit($ms)) { $p.Kill(); 'TIMEOUT' } } }
function WaitNoneAlive($sec = 30) { for ($t = 0; $t -lt $sec * 2 -and (Alive).Count; $t++) { Start-Sleep -Milliseconds 500 } }
function Fresh { WaitNoneAlive; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins; New-Item -ItemType Directory $wins | Out-Null }
# Opens Warp again; windows that come back log what they are (window-info) and close.
function Reopen {
  Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue
  $env:WARPLITE_CHILD_SCRIPT = 'wait|window-info|close'
  $p = Warp @('wait', 'wait', 'window-info', 'wait', 'wait', 'wait', 'close')
  WaitAll $p
  WaitNoneAlive
  Remove-Item Env:\WARPLITE_CHILD_SCRIPT
  @(Bench | Where-Object { $_ -like 'window: *' } | Sort-Object)
}
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }

# 0. Before any quit-all: what it would close (dry run, sends nothing). With only this window of this Warp
#    open it is nothing; the real Warp's windows, if open, count as others.
Fresh
WaitAll (Warp @('wait', 'quit-all-targets', 'close'))
$dry = @(@(Bench) -match '^quit-all-targets')[0]
Check ($dry -match '^quit-all-targets: 0 of this Warp, \d+ of others$') "dry run: $dry"
if ($dry -notmatch '^quit-all-targets: 0 of this Warp,') { 'quit-all could reach other windows: test stopped'; "fallos: $fail"; exit }

# 1. Main + 2 others (a tab with Claude Code moved out, and a second Warp): the others closed with their
#    X, then the main one. Opened again: only the main one. The moved tab resumed its conversation.
Fresh
$env:WARPLITE_CHILD_SCRIPT = ((@('wait') * 8) + @('window-info', 'close')) -join '|'
$m = Warp (@('wait', 'rename:principal', 'tab', 'wait', "edit:cd '$($proj.FullName)'", 'submit', 'wait', 'edit:claude --model m1', 'submit',
  'wait', 'wait', 'wait', 'rename:sacada', 'detach-tab', 'wait', 'window-info') + @('wait') * 20 + @('close'))
Start-Sleep 4
$s = Warp @('wait', 'rename:segunda', 'window-info', 'wait', 'close')
WaitAll $m, $s
WaitNoneAlive
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$b = @(Bench)
Check ($b -contains 'window: main reopen=0 [principal]') 'main window: the tab moved out is gone from it'
Check ($b -contains 'window: other reopen=0 [sacada]') 'the tab moved out opened in a new window'
Check ($b -contains 'window: other reopen=0 [segunda]') 'second Warp: another window'
$args1 = @(Get-Content -Encoding UTF8 "$dir\args.txt" -ErrorAction SilentlyContinue)
Check ("$args1" -eq 'claude --model m1') "the moved tab's Claude Code went on running, never launched again: $($args1 -join ' / ')"
Check ("$(Files)" -eq 'window-main.txt') "saved after closing them with their X: $(Files)"
$r = Reopen
Check ("$r" -eq 'window: main reopen=0 [principal]') "opened again: $($r -join ' / ')"

# 2. Main + one with «Reopen on restart» + one without; quit-all from the main one. Opened again: the main
#    one and the checked one (and quit-all again keeps both).
Fresh
$m = Warp (@('wait', 'rename:principal') + @('wait') * 12 + @('quit-all'))
Start-Sleep 4
$x1 = Warp @('wait', 'rename:marcada', 'reopen:on', 'window-info')
$x2 = Warp @('wait', 'rename:sinmarcar', 'window-info')
WaitAll $m, $x1, $x2
WaitNoneAlive
Check (@(Bench) -contains 'window: other reopen=1 [marcada]') 'reopen:on in the second window'
$f = Files
$other = $f | Where-Object { $_ -ne 'window-main.txt' }
Check (($f.Count -eq 2) -and ($f -contains 'window-main.txt') -and ("$(TabNames "$wins\$other")" -eq 'marcada')) "saved after quit-all: $f"
Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue
$env:WARPLITE_CHILD_SCRIPT = 'wait|window-info'
WaitAll (Warp @('wait', 'wait', 'window-info', 'wait', 'wait', 'wait', 'quit-all'))
WaitNoneAlive
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$r = @(Bench | Sort-Object)
Check ("$r" -eq 'window: main reopen=0 [principal] window: other reopen=1 [marcada]') "opened again: $($r -join ' / ')"
Check ((Files).Count -eq 2) "quit-all again keeps both: $(Files)"

# 3. The main window closed first while the others stay open, then the others: the main one comes back,
#    the others don't.
Fresh
$env:WARPLITE_CHILD_SCRIPT = ((@('wait') * 14) + @('close')) -join '|'
$m = Warp @('wait', 'rename:principal', 'tab', 'wait', 'rename:sacada', 'detach-tab', 'wait', 'close')
Start-Sleep 3
$s = Warp (@('wait', 'rename:segunda') + @('wait') * 16 + @('close'))
for ($t = 0; $t -lt 60 -and (Windows) -ne 3; $t++) { Start-Sleep -Milliseconds 500 }  # all three open
for ($t = 0; $t -lt 60 -and (Windows) -ne 2; $t++) { Start-Sleep -Milliseconds 500 }  # then the main one closed
Check ((Test-Path "$wins\window-main.txt") -and (Windows) -eq 2) "main closed first: kept, the other two still open ($(Windows))"
WaitAll $m, $s
WaitNoneAlive
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
Check ("$(Files)" -eq 'window-main.txt') "then the others closed: $(Files)"
$r = Reopen
Check ("$r" -eq 'window: main reopen=0 [principal]') "opened again: $($r -join ' / ')"

# 4. Update restart (the same exe, like «Restart» relaunches the installed one): every window comes back,
#    each as it was; a later quit-all keeps only the main one and the checked one.
Fresh
$env:WARPLITE_CHILD_SCRIPT = 'wait|window-info'
$m = Warp (@('wait', 'rename:principal', 'tab', 'wait', 'rename:sacada', 'detach-tab') + @('wait') * 10 + @('update-restart'))
Start-Sleep 4
$x1 = Warp @('wait', 'rename:marcada', 'reopen:on')
$x2 = Warp @('wait', 'rename:sinmarcar')
WaitAll $m, $x1, $x2
WaitFor '^window: other' 4 60   # sacada before the restart, then the three restored ones
Start-Sleep 2
$b = @(Bench | Sort-Object)
Check ((Alive).Count -eq 1 -and (Windows) -eq 4) "after the update restart: $(Windows) windows open (main + 3) in $((Alive).Count) process"
Check ("$b" -eq 'window: other reopen=0 [sacada] window: other reopen=0 [sacada] window: other reopen=0 [sinmarcar] window: other reopen=1 [marcada]') "restored: $($b -join ' / ')"
Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue
WaitAll (Warp @('wait', 'wait', 'window-info', 'quit-all'))
WaitNoneAlive
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$q = @(Bench)
Check (($q.Count -eq 1) -and ($q[0] -like 'window: other reopen=0 `[*`]')) "with the main one open, a new Warp is another window: $q"
$f = Files
$other = $f | Where-Object { $_ -ne 'window-main.txt' }
Check (($f.Count -eq 2) -and ("$(TabNames "$wins\window-main.txt")" -eq 'principal') -and ("$(TabNames "$wins\$other")" -eq 'marcada')) "then quit-all: main + checked kept ($f)"

# 5. Windows saved by the version before (no «reopen» line): the first one (window-1.txt) becomes the main
#    one and the other comes back this once; closing it with quit-all then discards it.
Fresh
$legacy = "BW1`nactive 0`nui 100 13`nlang en`nvtabs 0`ntab 0 {0}`nL - -`n"
[IO.File]::WriteAllText("$wins\window-1.txt", ($legacy -f (Hex 'uno')))
[IO.File]::WriteAllText("$wins\window-2ab.txt", ($legacy -f (Hex 'dos')))
$env:WARPLITE_CHILD_SCRIPT = 'wait|window-info'
WaitAll (Warp @('wait', 'wait', 'window-info', 'wait', 'wait', 'wait', 'quit-all'))
WaitNoneAlive
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$r = @(Bench | Sort-Object)
Check ("$r" -eq 'window: main reopen=0 [uno] window: other reopen=0 [dos]') "previous version's windows: $($r -join ' / ')"
Check ("$(Files)" -eq 'window-main.txt') "after quit-all: $(Files)"

# 6. The main window's file is gone: of the saved ones, the one that comes back becomes the main one; one
#    left open when Warp was killed (reopen 0) is discarded.
Fresh
$new = "BW1`nactive 0`nui 100 13`nlang en`nvtabs 0`ntab 0 {0}`nL - -`nreopen {1}`n"
[IO.File]::WriteAllText("$wins\window-aaa.txt", ($new -f (Hex 'aaa'), 0))
[IO.File]::WriteAllText("$wins\window-bbb.txt", ($new -f (Hex 'bbb'), 1))
$env:WARPLITE_CHILD_SCRIPT = 'wait|window-info'
WaitAll (Warp @('wait', 'wait', 'window-info', 'wait', 'wait', 'wait', 'quit-all'))
WaitNoneAlive
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$r = @(Bench | Sort-Object)
Check ("$r" -eq 'window: main reopen=1 [bbb]') "main file gone: $($r -join ' / ')"
Check ("$(Files)" -eq 'window-main.txt') "left: $(Files)"

# 7. The main window closed with its last tab: it comes back where it was, with a new tab.
Fresh
WaitAll (Warp @('wait', 'move:120,90,900,600', 'wait', 'close-pane'))
$win1 = (Select-String -Path "$wins\window-main.txt" -Pattern '^win ').Line
Check ($win1 -and -not (Select-String -Path "$wins\window-main.txt" -Pattern '^tab ' -Quiet)) "last tab closed: kept without tabs ($win1)"
$r = Reopen
$win2 = (Select-String -Path "$wins\window-main.txt" -Pattern '^win ').Line
Check ((@($r).Count -eq 1) -and ("$r" -like 'window: main reopen=0 `[*`]') -and ($win2 -eq $win1)) "opened again: $r, $win2"

WaitNoneAlive
Check ((Alive).Count -eq 0) "no test processes alive ($((Alive).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$dir\bin*" } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, "$dir\fake-claude", "$dir\proj", "$dir\bin", "$dir\args.txt"
"fallos: $fail"
