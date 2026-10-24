# The update restart from a Warp of one process per window (the versions before this one) to this one, as on a computer
# that updates (WARPLITE_DIR, never the real Warp): three windows of the old Warp, each its own process (the main one
# running a fake ssh ... tmux, a second start, a tab moved out); the installer puts the new exe where the old one is
# (the running one renamed, as Windows allows) and «Restart» in the old Warp launches it with --relaunch. Then: one
# process of the new Warp, the three windows back, ssh launched again, no old process left. -Old: the old warp-lite.exe.
param([Parameter(Mandatory)][string]$Old)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$wins = "$dir\wins-subir"; $app = "$dir\subir-app"; $bin = "$dir\bin-subir"; $log = "$dir\args-subir.txt"
$env:WARPLITE_NO_UPDATE = '1'
Remove-Item Env:\WARPLITE_SESSION, Env:\WARPLITE_CHILD_SCRIPT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $app, $bin, $log, "$dir\go-subir-*.txt"
New-Item -ItemType Directory $wins, $app, $bin | Out-Null
Copy-Item $Old "$app\warp-lite.exe"
$fake = @'
$tool = $args[0]; $a = @($args | Select-Object -Skip 1)
for ($k = 0; $k -lt 100; $k++) { try { Add-Content -Encoding UTF8 "$PSScriptRoot\..\args-subir.txt" (("$tool " + ($a -join ' ')).Trim()) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 30 } }
Start-Sleep 300
'@
Set-Content -Encoding UTF8 "$bin\fake.ps1" $fake
Set-Content -Encoding ASCII "$bin\ssh.cmd" "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake.ps1`" ssh %*"
$env:PATH = "$bin;$env:PATH"
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
function Warp($steps) {
  $env:WARPLITE_DIR = $wins
  Start-Process -FilePath "$app\warp-lite.exe" -ArgumentList "--script `"$(($steps -join '|').Replace('"', '\"'))`"" -PassThru
}
function Bench { @(Get-ChildItem "$wins\*.bench" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content -Encoding UTF8 $_.FullName }) }
function WaitFor($pat, $n = 1, $sec = 60) {
  for ($t = 0; $t -lt $sec * 2; $t++) { if (@(@(Bench) -match $pat).Count -ge $n) { break }; Start-Sleep -Milliseconds 500 }
  @(@(Bench) -match $pat)[$n - 1]
}
function Launched { @(Get-Content -Encoding UTF8 $log -ErrorAction SilentlyContinue) }
function Alive { @(Get-Process warp-lite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$app\*" }) }
$fail = 0
function Check($ok, $what) { if (-not $ok) { $script:fail++ }; '{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $what }

# The old Warp: the main window (ssh running, a tab moved out to its own process) and a second start.
$env:WARPLITE_CHILD_SCRIPT = 'wait|wait|window-info'  # every window that opens later says what it is
$m = Warp @('wait', 'rename:principal', 'edit:ssh -t devbox "tmux new -A -s main"', 'submit', 'tab', 'wait', 'rename:sacada', 'detach-tab',
  'wait', "wait-file:$dir\go-subir-update.txt", 'update-restart')
Start-Sleep 4
$s = Warp @('wait', 'rename:segunda', 'wait')
for ($t = 0; $t -lt 60 -and @(Launched).Count -lt 1; $t++) { Start-Sleep -Milliseconds 500 }
for ($t = 0; $t -lt 60 -and @(Alive).Count -lt 3; $t++) { Start-Sleep -Milliseconds 500 }  # the tab moved out, in its own process
Start-Sleep 3
$prev = @(Alive)
Check ($prev.Count -eq 3 -and @(Launched).Count -eq 1) "the old Warp: $($prev.Count) processes, ssh running ($(@(Launched) -join ' / '))"
# The installer: the running exe renamed, the new one in its place.
Rename-Item "$app\warp-lite.exe" 'warp-lite-old.exe'
Copy-Item "$dir\warp-lite.exe" "$app\warp-lite.exe"
Remove-Item "$wins\*.bench" -ErrorAction SilentlyContinue
New-Item -ItemType File -Force "$dir\go-subir-update.txt" | Out-Null
$new = @()
for ($t = 0; $t -lt 120; $t++) {  # until the old processes are gone and the new one has its windows
  Start-Sleep -Milliseconds 500
  $new = @(Alive | Where-Object { $prev.Id -notcontains $_.Id })
  if (@(Alive | Where-Object { $prev.Id -contains $_.Id }).Count -eq 0 -and $new.Count -eq 1 -and [WarpTest.Win]::Count([uint32]$new[0].Id) -eq 3) { break }
}
$left = @(Get-Process -Id $prev.Id -ErrorAction SilentlyContinue)
Check ($left.Count -eq 0 -and $new.Count -eq 1) "after «Restart»: the old processes gone ($($left.Count) left), one new process ($($new.Count))"
Check ($new.Count -eq 1 -and [WarpTest.Win]::Count([uint32]$new[0].Id) -eq 3) "its three windows back ($(if ($new.Count) { [WarpTest.Win]::Count([uint32]$new[0].Id) }))"
$null = WaitFor '^window: other' 2  # their script starts a few seconds after they open
$w = @(Bench | Where-Object { $_ -like 'window: *' } | Sort-Object)
Check ("$w" -eq 'window: other reopen=0 [sacada] window: other reopen=0 [segunda]') "the others as they were: $($w -join ' / ')"
for ($t = 0; $t -lt 60 -and @(Launched).Count -lt 2; $t++) { Start-Sleep -Milliseconds 500 }
Check (@(Launched).Count -eq 2) "ssh launched again in the main window: $(@(Launched) -join ' / ')"

# Done: quit-all through a start of this Warp (it hands it to the running one).
Remove-Item Env:\WARPLITE_CHILD_SCRIPT
$q = Warp @('wait', 'quit-all')
for ($t = 0; $t -lt 60 -and (Alive).Count; $t++) { Start-Sleep -Milliseconds 500 }
Check ((Alive).Count -eq 0) "no test processes alive ($((Alive).Count))"
Alive | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$bin*" } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $wins, $app, $bin, $log, "$dir\go-subir-*.txt"
"fallos: $fail"
