# Automatic ssh reconnection, with a fake ssh (bin\ssh.cmd, always run by its full path: the real ssh
# never runs and nothing touches the network). A dropped session is launched again with growing waits
# (2, 4, 8 s) until it connects; a normal exit, a tmux detach, «~.», another exit code, an authentication
# or host key error, a first connection that fails and other remote commands are not; a permission
# denied inside the loop stops it; Esc and typing cancel it; a network change brings the next attempt
# forward; closing the tab stops the retries and ends its ssh; a pane waiting to reconnect is saved as
# running and, reopened, keeps trying. Results in session-reconexion.txt.bench; screenshot of the
# status card in reconexion.bmp.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-reconexion.txt"
$env:WARPLITE_NO_UPDATE = '1'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$env:WARPLITE_SESSION*", "$dir\bin", "$dir\rc-logs", "$dir\reconexion.bmp"
New-Item -ItemType Directory "$dir\bin", "$dir\rc-logs" | Out-Null
$fake = @'
# Fake ssh: what it does depends on the host (rc-<case>.invalid) and on how many times it has run.
$h = @($args | Where-Object { $_ -like 'rc-*' })[0]
$case = $h.Split('.')[0]
$plans = @{
  'rc-a' = @('drop', 'timeout', 'timeout', 'up'); 'rc-b1' = @('exit0'); 'rc-b2' = @('detach'); 'rc-b3' = @('tilde')
  'rc-b4' = @('code1'); 'rc-b5' = @('auth'); 'rc-b6' = @('hostkey'); 'rc-b7' = @('timeout'); 'rc-b8' = @('drop')
  'rc-c' = @('drop', 'auth'); 'rc-d' = @('closed', 'up'); 'rc-e' = @('drop', 'timeout'); 'rc-f' = @('drop', 'timeout')
  'rc-g' = @('drop', 'timeout', 'timeout', 'up'); 'rc-h' = @('drop', 'timeout'); 'rc-i' = @('drop', 'timeout'); 'rc-j' = @('up')
}
$plan = $plans[$case]
$log = "$PSScriptRoot\..\rc-logs\$case.txt"
$n = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -like 'start *' }).Count
$what = $plan[[math]::Min($n, $plan.Count - 1)]
function Ms { [DateTimeOffset]::Now.ToUnixTimeMilliseconds() }
function Quit([int]$code) { Add-Content $log "end $n $code $(Ms)"; exit $code }
function Up([int]$s) { for ($i = 0; $i -lt $s; $i++) { [Console]::Out.WriteLine('connected'); Start-Sleep 1 } }
function Err([string]$t) { [Console]::Error.WriteLine($t) }
Add-Content $log "start $n $what $(Ms)"
switch ($what) {
  'up' { for ($i = 0; $i -lt 400; $i++) { [Console]::Out.WriteLine('connected'); Add-Content $log "alive $(Ms)"; Start-Sleep 1 }; Quit 0 }
  'drop' { Up 2; Err 'client_loop: send disconnect: Connection reset'; Quit 255 }
  'closed' { Up 2; Err "Connection to $h closed by remote host."; Err "Connection to $h closed."; Quit 255 }
  'timeout' { Err "ssh: connect to host $h port 22: Connection timed out"; Quit 255 }
  'auth' { Err "diego@${h}: Permission denied (publickey)."; Quit 255 }
  'hostkey' { Err '@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @'; Err 'Host key verification failed.'; Quit 255 }
  'exit0' { Up 1; Err "Connection to $h closed."; Quit 0 }
  'detach' { Up 1; [Console]::Out.WriteLine('[detached (from session t)]'); Quit 0 }
  'tilde' { Up 1; Err "Connection to $h closed."; Quit 255 }
  'code1' { Up 1; Err 'client_loop: send disconnect: Connection reset'; Quit 1 }
}
'@
Set-Content -Encoding UTF8 "$dir\bin\fake-ssh.ps1" $fake
Set-Content -Encoding ASCII "$dir\bin\ssh.cmd" "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake-ssh.ps1`" %*"
$S = "& '$dir\bin\ssh.cmd'"

# Which commands may be launched again, and what ssh's last lines mean.
$cmds = @(
  @('ssh fr', 'yes'), @('ssh -t fr "tmux new -A -s main"', 'yes'), @('ssh -t fr tmux attach -t main', 'yes'),
  @('ssh fr -t tmux a', 'yes'), @('ssh.exe -p 2222 -i ~/.ssh/id fr "screen -dR"', 'yes'),
  @('C:\Windows\System32\OpenSSH\ssh.exe -L 8080:localhost:80 -N fr', 'yes'),
  @("& 'C:\Program Files\Git\usr\bin\ssh.exe' -oServerAliveInterval=30 fr 'tmux -u new-session -dA -s x'", 'yes'),
  @('ssh fr "tmux new -s main"', 'no'), @('ssh fr make deploy', 'no'), @('ssh -t fr "bash -lc deploy"', 'no'),
  @('ssh -t fr tmux', 'no'), @('scp a fr:b', 'no'), @('git pull', 'no'), @('mosh fr', 'no'), @('ssh', 'no')
)
$texts = @(
  @('client_loop: send disconnect: Connection reset', 'lost'),
  @('Connection to fr closed by remote host.          Connection to fr closed.', 'lost'),
  @('packet_write_wait: Connection to 1.2.3.4 port 22: Broken pipe', 'lost'),
  @('Read from remote host fr: Connection timed out', 'lost'), @('Timeout, server fr not responding.', 'lost'),
  @('client_loop: send disconnect: Network is unreachable', 'lost'),
  @('ssh: connect to host fr port 22: Connection timed out', 'unreachable'),
  @('ssh: Could not resolve hostname fr: No such host is known.', 'unreachable'),
  @('kex_exchange_identification: read: Connection reset by peer', 'unreachable'),
  @('Connection closed by 100.64.0.1 port 22', 'unreachable'),
  @('diego@fr: Permission denied (publickey).', 'fatal'), @('Host key verification failed.', 'fatal'),
  @('Connection to fr closed.', 'other'), @('[detached (from session main)]', 'other')
)
$steps = @('wait', 'wait', 'wait') + ($cmds | ForEach-Object { "reconnect-cmd:$($_[0])" }) + ($texts | ForEach-Object { "reconnect-text:$($_[0])" })
# A: dropped, two attempts that can't connect, the third one connects.
$steps += @('reconnect-wait:idle', "edit:$S -t rc-a.invalid `"tmux new -A -s t`"", 'submit', 'reconnect-wait:trying 3', 'reconnect-wait:running', 'wait', 'reconnect')
# B: never launched again (in a new tab: A's tmux is still connected in the first one).
$steps += @('tab', 'reconnect-wait:idle')
foreach ($c in 'rc-b1', 'rc-b2', 'rc-b3', 'rc-b4', 'rc-b5', 'rc-b6', 'rc-b7') { $steps += @("edit:$S -t $c.invalid `"tmux a`"", 'submit', 'reconnect-wait:idle') }
$steps += @("edit:$S rc-b8.invalid make deploy", 'submit', 'reconnect-wait:idle', 'reconnect')
# C: permission denied inside the loop stops it. D: plain ssh to a shell, closed by the remote host.
$steps += @("edit:$S -t rc-c.invalid `"tmux a`"", 'submit', 'reconnect-wait:trying 1', 'reconnect-wait:idle', 'reconnect')
$steps += @("edit:$S rc-d.invalid", 'submit', 'reconnect-wait:trying 1', 'reconnect-wait:running', 'wait', 'reconnect')
# E: Esc cancels while waiting (status card in Spanish on the screenshot). F: typing cancels.
# G: a network change brings the attempt forward.
$steps += @('tab', 'reconnect-wait:idle', 'lang:es', "edit:$S -t rc-e.invalid `"tmux a`"", 'submit', 'reconnect-wait:waiting 2', "shot:$dir\reconexion.bmp",
  'key:esc', 'reconnect') + @('wait') * 6 + @('reconnect', 'lang:en')
$steps += @("edit:$S -t rc-f.invalid `"tmux a`"", 'submit', 'reconnect-wait:waiting 1', 'edit:x', 'edit-text', 'clear') + @('wait') * 4 + @('reconnect')
$steps += @("edit:$S -t rc-g.invalid `"tmux a`"", 'submit', 'reconnect-wait:waiting 3', 'net-change', 'reconnect-wait:trying 3', 'reconnect-wait:running', 'wait', 'reconnect')
# H: closing the tab while waiting stops the retries, and while connected ends its ssh.
$steps += @('tab', 'reconnect-wait:idle', "edit:$S -t rc-h.invalid `"tmux a`"", 'submit', 'reconnect-wait:waiting 1', 'close-pane')
$steps += @('tab', 'reconnect-wait:idle', "edit:$S -t rc-j.invalid `"tmux a`"", 'submit', 'wait', 'wait', 'wait', 'close-pane') + @('wait') * 8
# I: quitting while waiting saves it as running.
$steps += @('tab', 'reconnect-wait:idle', "edit:$S -t rc-i.invalid `"tmux a`"", 'submit', 'reconnect-wait:waiting 1', 'quit')

function Fakes { @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$dir\bin*" }) }
function Logs { $l = @{}; Get-ChildItem "$dir\rc-logs" -Filter '*.txt' | ForEach-Object { $l[$_.BaseName] = @(Get-Content $_.FullName) }; $l }
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(420000)) { $p.Kill(); 'TIMEOUT 1' }
Start-Sleep 3
$left1 = (Fakes).Count
$logs1 = Logs
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$saved = Get-Content -Raw $env:WARPLITE_SESSION -ErrorAction SilentlyContinue
Remove-Item "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue
# Reopened: the pane that was waiting is relaunched by the restore and, unreachable, keeps trying.
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$((@('reconnect-wait:trying 1', 'reconnect-wait:waiting 2', 'reconnect', 'quit')) -join '|')`"" -PassThru
if (-not $p.WaitForExit(180000)) { $p.Kill(); 'TIMEOUT 2' }
Start-Sleep 3
$left2 = (Fakes).Count
$got += @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue | ForEach-Object { "reabrir: $_" })

$want = @($cmds | ForEach-Object { "reconnect-cmd: $($_[1])" }) + @($texts | ForEach-Object { "reconnect-text: $($_[1])" }) + @(
  'reconnect: exit 255 lost -> attempt 1 in 2s', 'reconnect: attempt 1 launched', 'reconnect: exit 255 unreachable -> attempt 2 in 4s',
  'reconnect: attempt 2 launched', 'reconnect: exit 255 unreachable -> attempt 3 in 8s', 'reconnect: attempt 3 launched', 'reconnect: connected',
  'reconnect: running',
  'reconnect: exit 0 other -> no', 'reconnect: exit 0 other -> no', 'reconnect: exit 255 other -> no', 'reconnect: exit 1 other -> no',
  'reconnect: exit 255 fatal -> no', 'reconnect: exit 255 fatal -> no', 'reconnect: exit 255 unreachable -> no', 'reconnect: idle',
  'reconnect: exit 255 lost -> attempt 1 in 2s', 'reconnect: attempt 1 launched', 'reconnect: exit 255 fatal -> stop', 'reconnect: idle',
  'reconnect: exit 255 lost -> attempt 1 in 2s', 'reconnect: attempt 1 launched', 'reconnect: connected', 'reconnect: running',
  'reconnect: exit 255 lost -> attempt 1 in 2s', 'reconnect: attempt 1 launched', 'reconnect: exit 255 unreachable -> attempt 2 in 4s',
  'reconnect: cancelled', 'reconnect: idle', 'reconnect: idle',
  'reconnect: exit 255 lost -> attempt 1 in 2s', 'reconnect: cancelled', 'edit: [x]', 'reconnect: idle',
  'reconnect: exit 255 lost -> attempt 1 in 2s', 'reconnect: attempt 1 launched', 'reconnect: exit 255 unreachable -> attempt 2 in 4s',
  'reconnect: attempt 2 launched', 'reconnect: exit 255 unreachable -> attempt 3 in 8s', 'reconnect: network changed -> attempt 3 sooner',
  'reconnect: attempt 3 launched', 'reconnect: connected', 'reconnect: running',
  'reconnect: exit 255 lost -> attempt 1 in 2s',
  'reconnect: exit 255 lost -> attempt 1 in 2s',
  'reabrir: reconnect: exit 255 unreachable -> attempt 1 in 2s', 'reabrir: reconnect: attempt 1 launched',
  'reabrir: reconnect: exit 255 unreachable -> attempt 2 in 4s', 'reabrir: reconnect: waiting 2'
)
$fail = 0
for ($i = 0; $i -lt [math]::Max($want.Count, $got.Count); $i++) {
  $ok = $i -lt $got.Count -and $i -lt $want.Count -and $got[$i] -ceq $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] + $(if (-not $ok -and $i -lt $want.Count) { " (esperado $($want[$i]))" }) } else { "(falta) $($want[$i])" })
}
# What the fake ssh saw: how many times each case ran, and the waits between attempts.
function Check([bool]$ok, [string]$what) { if (-not $ok) { $script:fail++ }; "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }
function Times($l, [string]$kind) { @($l | Where-Object { $_ -like "$kind *" } | ForEach-Object { [long]($_ -split ' ')[-1] }) }
$runs = [ordered]@{ 'rc-a' = 4; 'rc-b1' = 1; 'rc-b2' = 1; 'rc-b3' = 1; 'rc-b4' = 1; 'rc-b5' = 1; 'rc-b6' = 1; 'rc-b7' = 1; 'rc-b8' = 1
  'rc-c' = 2; 'rc-d' = 2; 'rc-e' = 2; 'rc-f' = 1; 'rc-g' = 4; 'rc-h' = 1; 'rc-i' = 1; 'rc-j' = 1 }
foreach ($k in $runs.Keys) { $c = (Times $logs1[$k] 'start').Count; Check ($c -eq $runs[$k]) "$k ssh launched $c times (expected $($runs[$k]))" }
$st = Times $logs1['rc-a'] 'start'; $en = Times $logs1['rc-a'] 'end'
for ($k = 0; $k -lt 3 -and $k + 1 -lt $st.Count -and $k -lt $en.Count; $k++) {
  $gap = ($st[$k + 1] - $en[$k]) / 1000.0; $base = 2 * [math]::Pow(2, $k)
  Check ($gap -ge $base -and $gap -le $base * 1.1 + 3) ("rc-a wait before attempt {0}: {1:N1} s (backoff {2} s)" -f ($k + 1), $gap, $base)
}
$st = Times $logs1['rc-g'] 'start'; $en = Times $logs1['rc-g'] 'end'
if ($st.Count -ge 4 -and $en.Count -ge 3) { $gap = ($st[3] - $en[2]) / 1000.0; Check ($gap -lt 6.5) ("rc-g wait before attempt 3 after a network change: {0:N1} s (backoff 8 s)" -f $gap) }
$alive = Times $logs1['rc-j'] 'alive'; $next = Times $logs1['rc-i'] 'start'
if ($alive.Count -and $next.Count) { Check (($next[0] - $alive[-1]) -gt 5000) ("rc-j ssh ended with its tab: last sign of life {0:N1} s before the next case started" -f (($next[0] - $alive[-1]) / 1000.0)) }
else { Check $false 'rc-j: no log' }
$c = (Times (Get-Content "$dir\rc-logs\rc-i.txt") 'start').Count
Check ($c -eq 3) "rc-i ssh launched $c times after reopening (expected 3: the drop, the restore, one attempt)"
Check ($saved -match ("L " + (-join ([Text.Encoding]::UTF8.GetBytes("$S -t rc-i.invalid `"tmux a`"") | ForEach-Object { $_.ToString('x2') })) + ' [0-9a-f-]+ run')) 'pane waiting to reconnect saved as running'
Check ($left1 -eq 0) "fake ssh still running after quitting: $left1"
Check ($left2 -eq 0) "fake ssh still running after quitting the reopened Warp: $left2"
# Nothing of the test is left running.
Fakes | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\bin", "$dir\rc-logs"
"fallos: $fail"
