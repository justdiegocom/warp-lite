# Phone access through warp-remote (the sidecar next to warp-lite.exe), against a test Warp with its own
# WARPLITE_DIR (never the real one), using warp-remote-cli.exe as the phone. (1) Nothing paired: no warp-remote
# runs. (2) Pair phone shows a code; a newer code replaces it; the client pairs with it and lists the panes of
# both windows of this Warp; the codes are one-time and an unknown device is turned away; while it is connected
# the title bar shows it. (3) Quit-all ends warp-remote; opening Warp again with a phone paired starts it and the
# client gets in with no code; an update restart replaces it with a new one. (4) Removing the device in Warp turns
# it away, and with nothing paired warp-remote ends. -External: the first code is for a client elsewhere (in
# remote-ticket.txt); the test waits for external-done.txt before going on.
param([switch]$External, [int]$ExternalWait = 300)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$wins = "$dir\wins-remote"
$phone = "$dir\cli-phone"; $stranger = "$dir\cli-stranger"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $phone, $stranger, "$dir\remote-*.bmp", "$dir\remote-ticket.txt",
  "$dir\external-done.txt", "$dir\go-*.txt"
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $wins | Out-Null
function Warp($steps) {
  $env:WARPLITE_DIR = $wins
  Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps -join '|')`"" -PassThru
}
function Bench { @(Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName }) }
function WaitFor($pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2; $t++) { if (@(@(Bench) -match $pat).Count -ge $n) { break }; Start-Sleep -Milliseconds 500 }
  @(@(Bench) -match $pat)[$n - 1]
}
function Go($name) { New-Item -ItemType File -Force "$dir\go-$name.txt" | Out-Null }
function Sidecars { @(Get-Process warp-remote -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$dir\warp-remote.exe" }) }
function WaitSidecars($n, $sec = 15) { for ($t = 0; $t -lt $sec * 2 -and (Sidecars).Count -ne $n; $t++) { Start-Sleep -Milliseconds 500 }; (Sidecars).Count }
function Alive { @(Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) }
function WaitNoneAlive($sec = 30) { for ($t = 0; $t -lt $sec * 2 -and (Alive).Count; $t++) { Start-Sleep -Milliseconds 500 } }
function RemoteCli($state) {
  $ErrorActionPreference = 'Continue'  # its errors go to stderr: they are results here
  $out = & "$dir\warp-remote-cli.exe" --state $state --name 'wintest phone' @args 2>&1 | ForEach-Object { "$_" }
  [pscustomobject]@{ Ok = $LASTEXITCODE -eq 0; Out = ($out -join ' / ') }
}
$devs = 1 + [int][bool]$External  # paired devices once this test's phone is (the external one comes first)
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }

# 1. Two windows of one Warp; nothing paired.
$a = Warp @('wait', 'rename:first', 'splitR', 'wait', 'wait', 'remote-state', "wait-file:$dir\go-pair.txt",
  'remote-pair', 'remote-wait', 'remote-ticket', "shot:$dir\remote-pair.bmp", "wait-file:$dir\go-pair2.txt",
  'remote-pair', 'remote-wait', 'remote-ticket', "wait-file:$dir\go-shot.txt", 'remote-state', "shot:$dir\remote-connected.bmp",
  "wait-file:$dir\go-quit.txt", 'quit-all')
Start-Sleep 4
$b = Warp @('wait', 'rename:second', "wait-file:$dir\go-quit.txt")
$st = WaitFor '^remote: '
Check ($st -eq 'remote: phones=0 devices=0 sidecar=0 card=0' -and (Sidecars).Count -eq 0) "nothing paired, no warp-remote: $st ($((Sidecars).Count) running)"

# 2. Pairing.
Go 'pair'
$t1 = (WaitFor '^remote-ticket: ') -replace '^remote-ticket: '
Check ($t1 -match '^WARP1[A-Z2-7]{100,}$') "pairing code shown ($($t1.Length) characters)"
Check ((WaitSidecars 1) -eq 1) 'warp-remote started for pairing'
if ($External) {
  Set-Content -Encoding ASCII "$dir\remote-ticket.txt" $t1
  "waiting for the external client (external-done.txt)..."
  for ($t = 0; $t -lt $ExternalWait -and -not (Test-Path "$dir\external-done.txt"); $t++) { Start-Sleep 1 }
  Check (Test-Path "$dir\external-done.txt") 'external client done'
}
Go 'pair2'
$t2 = (WaitFor '^remote-ticket: ' 2) -replace '^remote-ticket: '
Check ($t2 -match '^WARP1' -and $t2 -ne $t1) 'a new code'
$r = RemoteCli $stranger pair $t1
Check (-not $r.Ok) "the replaced code is refused: $($r.Out)"
$r = RemoteCli $phone pair $t2
Check ($r.Ok -and $r.Out -match 'paired with') "paired: $($r.Out)"
$r = RemoteCli $stranger pair $t2
Check (-not $r.Ok -and $r.Out -match 'not_paired') "a used code is refused, unknown device turned away: $($r.Out)"
$r = RemoteCli $phone --json sessions
$s = $null; try { $s = ($r.Out | ConvertFrom-Json).sessions } catch {}
$tabs = @($s | ForEach-Object tab_name | Sort-Object -Unique)
$winsSeen = @($s | ForEach-Object window | Sort-Object -Unique).Count
Check ($r.Ok -and $winsSeen -eq 2 -and @($s).Count -ge 3 -and ($tabs -contains 'first') -and ($tabs -contains 'second')) "sessions: $(@($s).Count) panes in $winsSeen windows, tabs [$($tabs -join ', ')]"
$r = RemoteCli $phone --json ping 5
$med = $null; try { $med = ($r.Out | ConvertFrom-Json).median_ms } catch {}
Check ($r.Ok -and $med -ne $null) "ping: median $med ms"
# Connected for a while: the title bar shows it.
$hold = Start-Process -FilePath "$dir\warp-remote-cli.exe" -ArgumentList "--state `"$phone`" --name `"wintest phone`" path 8" -PassThru
Start-Sleep 3
Go 'shot'
$st = WaitFor '^remote: ' 2
$phones = if ($External) { '[12]' } else { '1' }  # a killed app stays connected until QUIC's idle timeout (~30 s)
Check ($st -match "^remote: phones=$phones devices=$devs sidecar=1 card=0$") "connected, card closed by the pairing: $st"
if (-not $hold.WaitForExit(30000)) { $hold.Kill() }

# 3. Quit-all ends warp-remote; Warp again (a phone paired) starts it; an update restart replaces it.
Go 'quit'
foreach ($p in $a, $b) { if (-not $p.WaitForExit(30000)) { $p.Kill(); 'TIMEOUT' } }
Check ((WaitSidecars 0) -eq 0) 'quit-all: warp-remote ended'
Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue
$c = Warp @('wait', 'wait', 'wait', 'remote-state', "wait-file:$dir\go-update.txt", 'update-restart')
$st = WaitFor '^remote: '
Check ($st -eq "remote: phones=0 devices=$devs sidecar=1 card=0") "Warp again starts it: $st"
$r = RemoteCli $phone sessions
Check ($r.Ok -and $r.Out -match 'first') "in with no code: $($r.Out.Substring(0, [Math]::Min(80, $r.Out.Length)))"
$old = @(Sidecars | ForEach-Object Id)
Go 'update'
if (-not $c.WaitForExit(30000)) { $c.Kill(); 'TIMEOUT' }
$new = @()
for ($t = 0; $t -lt 40 -and -not $new.Count; $t++) { Start-Sleep -Milliseconds 500; $new = @(Sidecars | Where-Object { $old -notcontains $_.Id }) }
Check ($new.Count -eq 1 -and (WaitSidecars 1) -eq 1) "update restart: a new warp-remote ($($old -join ',') -> $($new.Id -join ','))"
$r = RemoteCli $phone sessions
Check ($r.Ok -and $r.Out -match 'first') "after the update restart, in with no code: $($r.Out.Substring(0, [Math]::Min(60, $r.Out.Length)))"

# 4. Removed in Warp (from another window) while connected: disconnected at once; nothing paired, warp-remote ends.
$remove = if ($External) { @('remote-remove:1', 'remote-remove:0') } else { @('remote-remove:0') }  # this test's phone first
Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue
$e = Warp (@('wait', 'wait', "wait-file:$dir\go-remove.txt") + $remove + @('wait', 'remote-state', "wait-file:$dir\go-end.txt", 'quit-all'))
$hold = Start-Process -FilePath "$dir\warp-remote-cli.exe" -ArgumentList "--state `"$phone`" --name `"wintest phone`" ping 60" -PassThru `
  -RedirectStandardOutput "$dir\cli-out.txt" -RedirectStandardError "$dir\cli-err.txt"
$null = $hold.Handle  # so ExitCode is kept
Start-Sleep 3
Go 'remove'
$st = WaitFor '^remote: '
Check ($st -match 'devices=0') "removed: $st"
if (-not $hold.WaitForExit(30000)) { $hold.Kill() }
$err = "$(Get-Content "$dir\cli-err.txt" -ErrorAction SilentlyContinue)"
Check ($hold.ExitCode -ne 0 -and $err -match 'revoked|not_paired') "removed while connected: $err"
Check ((WaitSidecars 0) -eq 0) 'nothing paired: warp-remote ended'
Go 'end'
if (-not $e.WaitForExit(30000)) { $e.Kill(); 'TIMEOUT' }

WaitNoneAlive
$null = WaitSidecars 0
Check ((Alive).Count -eq 0 -and (Sidecars).Count -eq 0) "no test processes alive ($((Alive).Count) + $((Sidecars).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Sidecars | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Content "$wins\remote\remote.log" -ErrorAction SilentlyContinue | Select-Object -Last 12
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $phone, $stranger, "$dir\go-*.txt", "$dir\external-done.txt", "$dir\cli-*.txt"
"fallos: $fail"
