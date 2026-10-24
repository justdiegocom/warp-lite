# A crash brings every window back (WARPLITE_DIR, like the real one). Every window of a Warp lives in one process, so a
# crash takes them all: their sessions are saved a moment after each change, and the crash relaunches Warp at once,
# with every window as it was, ssh + tmux launched again and Claude Code resumed (fake ones, like the real ones: they
# log each start and stay), a minidump and a line in crash.log. (1) Three windows (the main one, a second start, a tab
# moved out to a new one), a split pane, a renamed and a hidden tab; a test step crashes the process. (2) A second
# crash within a minute is not relaunched: only its line in crash.log.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$wins = "$dir\wins-recuperar"; $bin = "$dir\bin-recuperar"; $log = "$dir\args-recuperar.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude-recuperar"
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $bin, $log, $env:CLAUDE_CONFIG_DIR, "$dir\proj-recuperar", "$dir\go-recuperar-*.txt"
New-Item -ItemType Directory $wins, $bin, "$env:CLAUDE_CONFIG_DIR\sessions" | Out-Null
$proj = New-Item -ItemType Directory "$dir\proj-recuperar"
$fake = @'
$tool = $args[0]; $a = @($args | Select-Object -Skip 1)
# Several start at once (a restore): the log is shared, so a write that finds it busy tries again.
for ($k = 0; $k -lt 100; $k++) { try { Add-Content -Encoding UTF8 "$PSScriptRoot\..\args-recuperar.txt" (("$tool " + ($a -join ' ')).Trim()) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 30 } }
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

function Warp($steps) {
  $env:WARPLITE_DIR = $wins
  Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$(($steps -join '|').Replace('"', '\"'))`"" -PassThru
}
function Bench { @(Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName }) }
function WaitFor($pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2; $t++) { if (@(@(Bench) -match $pat).Count -ge $n) { break }; Start-Sleep -Milliseconds 500 }
  @(@(Bench) -match $pat)[$n - 1]
}
function Launched { @(Get-Content -Encoding UTF8 $log -ErrorAction SilentlyContinue) }
function WaitLaunched($pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2 -and @(@(Launched) -like $pat).Count -lt $n; $t++) { Start-Sleep -Milliseconds 500 }
  @(@(Launched) -like $pat).Count
}
function Go($name) { New-Item -ItemType File -Force "$dir\go-recuperar-$name.txt" | Out-Null }
function Wf($name) { "wait-file:$dir\go-recuperar-$name.txt" }
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
function WaitNoneAlive($sec = 30) { for ($t = 0; $t -lt $sec * 2 -and (Alive).Count; $t++) { Start-Sleep -Milliseconds 500 } }
function CrashLog { @(Get-Content "$wins\crash.log" -ErrorAction SilentlyContinue) }
function Entry($line, $tab) { if ($line -match "(w\d+)=([^|]*\[$tab[:\]][^|]*)") { $Matches[2].Trim() } else { '' } }  # the window with that tab
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }

# 1. The windows: the main one (ssh + tmux; a tab moved out to a new window), a second start (a split: Claude Code on the
#    right; a hidden tab). The windows that come back after the crash say what they are and list them all.
$env:WARPLITE_CHILD_SCRIPT = 'wait|wait|window-info|windows'
$m = Warp @('wait', 'rename:principal', 'edit:ssh -t devbox "tmux new -A -s main"', 'submit', 'tab', 'wait', 'rename:sacada', 'detach-tab',
  'wait', (Wf 'crash'), 'crash')
Start-Sleep 4
$s = Warp @('wait', 'rename:segunda', 'splitR', 'wait', "edit:cd '$($proj.FullName)'", 'submit', 'wait', 'edit:claude --model m1', 'submit',
  'tab', 'wait', 'rename:oculta', 'hide-tab', 'wait', 'window-info')
$n1 = WaitLaunched 'ssh *'
$n2 = WaitLaunched 'claude *'
Check ($n1 -eq 1 -and $n2 -eq 1) "ssh and Claude Code running: $(@(Launched) -join ' / ')"
$null = WaitFor '^window: other reopen=0 \[segunda\] \[oculta\]'  # the second window has done its steps
Start-Sleep 4  # what each pane runs is looked at every second, and the sessions are saved a second after a change
$before = @(Alive)
$files = @(Get-ChildItem "$wins\window-*.txt" | ForEach-Object Name)
Check ($before.Count -eq 1 -and $files.Count -eq 3) "one process, three windows saved: $($files -join ', ')"
Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue  # what the window moved out said before the crash
$t0 = Get-Date
Go 'crash'
$after = @()
for ($t = 0; $t -lt 600; $t++) {  # until the new process has its three windows again
  $after = @(Alive | Where-Object { $_.Id -ne $before[0].Id })
  if ($after.Count -eq 1 -and [WarpTest.Win]::Count([uint32]$after[0].Id) -eq 3) { break }
  Start-Sleep -Milliseconds 50
}
$ms = [int]((Get-Date) - $t0).TotalMilliseconds - 250  # the crash step waits for its file (250 ms polls)
$back = WaitFor '^windows: ' 1 60
Check ($after.Count -eq 1 -and @(Alive).Count -eq 1) "relaunched: process $($before.Id) -> $($after.Id), its three windows back about $ms ms after the crash"
Check ((Entry $back 'principal') -match '^\[principal:1:ssh -t devbox "tmux new -A -s main"\]$') "the main window, its ssh running: $back"
Check ((Entry $back 'sacada') -match '^\[sacada:1\]$') "the tab moved out, in its window: $(Entry $back 'sacada')"
Check ((Entry $back 'segunda') -match '^\[segunda:2:claude --model m1 --resume 000000m1-[^\]]*\] \[oculta:1\]$') "the second window, its split and its hidden tab: $(Entry $back 'segunda')"
$w = @(Bench | Where-Object { $_ -like 'window: *' } | Sort-Object)
Check ("$w" -eq 'window: other reopen=0 [sacada] window: other reopen=0 [segunda] [oculta]') "the other windows came back as they were: $($w -join ' / ')"
$n1 = WaitLaunched 'ssh *' 2 30
$n2 = WaitLaunched 'claude *' 2 30
Check ($n1 -eq 2 -and @(Launched) -contains "claude --model m1 --resume $m1") "ssh launched again, Claude Code resumed: $(@(Launched) -join ' / ')"
$c = CrashLog
$dumps = @(Get-ChildItem "$wins\crash-*.dmp" -ErrorAction SilentlyContinue)
Check ((($c -match 'crash c0000005 at warp-lite\.exe\+[0-9a-f]+ ').Count -eq 1) -and (($c -match '^\S+ \S+   relaunched$').Count -eq 1)) "crash.log: $($c -join ' / ')"
Check ($dumps.Count -eq 1 -and $dumps[0].Length -gt 10000 -and ($c -match "  minidump $($dumps[0].Name)$").Count -eq 1) "minidump: $($dumps.Name) ($($dumps[0].Length) bytes)"

# 2. A crash a moment later (a window of a second start crashes): not relaunched, a line says why.
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$null = Warp @('wait', 'crash')
WaitNoneAlive 30
Start-Sleep 5
$c = CrashLog
Check ((Alive).Count -eq 0 -and ($c -match 'crash c0000005').Count -eq 2 -and ($c -match '  not relaunched: \d+ s after the last relaunch$').Count -eq 1) "a second crash within a minute: not relaunched ($($c[-2]) / $($c[-1]))"

WaitNoneAlive
Check ((Alive).Count -eq 0) "no test processes alive ($((Alive).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$bin*" } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $bin, $log, $env:CLAUDE_CONFIG_DIR, "$dir\proj-recuperar", "$dir\go-recuperar-*.txt"
"fallos: $fail"
