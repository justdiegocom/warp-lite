# Claude Code y Codex abiertos al cerrar: al reabrir, cada panel reanuda SU conversación
# (claude --resume <id> / codex resume <id>), también con varios en la misma carpeta. Con claude
# y codex falsos que hacen lo mismo que los de verdad: claude apunta su conversación en
# sessions\<pid>.json y codex tiene abierto su rollout-…-<id>.jsonl mientras corre.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-reanudar.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude"
$env:CODEX_HOME = "$dir\fake-codex"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$dir\fake-*", "$dir\proj ñ", "$dir\bin", "$dir\args.txt"
$proj = New-Item -ItemType Directory "$dir\proj ñ"
New-Item -ItemType Directory "$dir\bin", "$env:CLAUDE_CONFIG_DIR\sessions" | Out-Null
$xdir = New-Item -ItemType Directory -Force ("$env:CODEX_HOME\sessions\" + (Get-Date -Format 'yyyy\\MM\\dd'))
$fake = @'
param([string]$tool)
$a = @($args)
# Several start at once (a restore): the log is shared, so a write that finds it busy tries again.
for ($k = 0; $k -lt 100; $k++) { try { Add-Content -Encoding UTF8 "$PSScriptRoot\..\args.txt" (("$tool " + ($a -join ' ')).Trim()) -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 30 } }
$model = 'x'; $res = ''
for ($i = 0; $i -lt $a.Count; $i++) {
  if ($a[$i] -in '--model', '-m') { $model = $a[$i + 1] }
  if ($a[$i] -in '--resume', 'resume' -and $i + 1 -lt $a.Count -and $a[$i + 1] -notlike '-*') { $res = $a[$i + 1] }
}
$id = if ($res) { $res } else { $model.PadLeft(8, '0') + '-0000-4000-8000-000000000000' }
if ($tool -eq 'claude') {
  Set-Content "$env:CLAUDE_CONFIG_DIR\sessions\$PID.json" "{`"pid`":$PID,`"sessionId`":`"$id`",`"cwd`":`"x`"}"
  Start-Sleep 300
} else {
  $f = Get-ChildItem "$env:CODEX_HOME\sessions" -Recurse -Filter "*$id.jsonl" | Select-Object -First 1
  $path = if ($f) { $f.FullName } else { "$env:CODEX_HOME\sessions\" + (Get-Date -Format 'yyyy\\MM\\dd') + "\rollout-$(Get-Date -Format 'HHmmssfff')-$id.jsonl" }
  $s = [IO.File]::Open($path, 'OpenOrCreate', 'ReadWrite', 'ReadWrite, Delete')
  if ($s.Length -eq 0) {
    $b = [Text.Encoding]::UTF8.GetBytes("{`"type`":`"session_meta`",`"payload`":{`"id`":`"$id`",`"cwd`":`"$((Get-Location).Path.Replace('\', '\\'))`",`"source`":`"cli`"}}`n")
    $s.Write($b, 0, $b.Length); $s.Flush()
  }
  Start-Sleep 300
  $s.Close()
}
'@
Set-Content -Encoding UTF8 "$dir\bin\fake.ps1" $fake
foreach ($n in 'claude', 'codex') { Set-Content -Encoding ASCII "$dir\bin\$n.cmd" "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake.ps1`" $n %*" }
# Un atajo que lanza claude con sus opciones (el clauded.cmd de muchos).
Set-Content -Encoding ASCII "$dir\bin\clauded.cmd" "@claude --dangerously-skip-permissions %*"
$env:PATH = "$dir\bin;$env:PATH"
function SortO([string[]]$l) { if (-not $l) { return @() }; [Array]::Sort($l, [StringComparer]::Ordinal); $l }
function Id([string]$m) { $m.PadLeft(8, '0') + '-0000-4000-8000-000000000000' }
# Conversaciones en disco con las fechas al revés de lo que diría «el último que escribió»:
# así solo sale bien si se usa la conversación de cada proceso.
$cdir = New-Item -ItemType Directory -Force ("$env:CLAUDE_CONFIG_DIR\projects\" + ($proj.FullName -replace '[^A-Za-z0-9]', '-'))
foreach ($k in 1..5) { Set-Content "$cdir\$(Id "m$k").jsonl" '{}'; (Get-Item "$cdir\$(Id "m$k").jsonl").LastWriteTime = (Get-Date).AddMinutes(-$k) }

$unit = @('claude', 'claude --model opus "haz algo"', 'claude --dangerously-skip-permissions --continue', 'claude -r abcdef12-3456 --verbose',
  'claude -p "hola"', 'claude mcp list', 'codex --yolo', 'codex -m gpt-5 "arregla esto"', 'codex resume --last', 'codex exec "x"', 'C:\bin\claude.exe', 'git status')
$cd = "cd '$($proj.FullName)'"
$steps = @('wait', 'wait', 'wait') + ($unit | ForEach-Object { "resume-cmd:$_" })
$launch = @('claude --model m1', 'claude --model m2 haz-algo', 'claude --dangerously-skip-permissions --model m3', 'claude --model m4', 'codex -m c1', 'codex --yolo -m c2',
  'clauded --model m5', 'clauded --model m6 hola')
for ($i = 0; $i -lt $launch.Count; $i++) {
  if ($i) { $steps += @('tab', 'wait', 'wait') }
  $steps += @("edit:$cd", 'submit', 'wait', "edit:$($launch[$i])", 'submit', 'wait', 'wait')
}
# Claude abierto a mano dentro de otra shell: cmd no es un atajo suyo y se relanza solo cmd.
$steps += @('tab', 'wait', 'wait', "edit:$cd", 'submit', 'wait', 'edit:cmd', 'submit', 'wait', 'type:claude --model m7', 'enter', 'wait', 'wait')
$steps += @('wait', 'wait', 'wait', 'wait', 'wait', 'resume', 'quit')
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
# En cuanto están los dos codex, sus rollouts también al revés (c1 el más reciente).
for ($t = 0; $t -lt 100 -and @(Get-Content "$dir\args.txt" -ErrorAction SilentlyContinue).Count -lt 6; $t++) { Start-Sleep -Milliseconds 500 }
Start-Sleep 2
Get-ChildItem $xdir -Filter "*$(Id c1).jsonl" | ForEach-Object { $_.LastWriteTime = (Get-Date).AddMinutes(1) }
if (-not $p.WaitForExit(150000)) { $p.Kill(); "TIMEOUT 1" }
$first = @(Get-Content -Encoding UTF8 "$dir\args.txt" -ErrorAction SilentlyContinue)
Remove-Item "$dir\args.txt" -ErrorAction SilentlyContinue
# Reabrir: cada panel, su conversación (9 PowerShell y sus claude/codex falsos arrancan a la vez).
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$((@('wait') * 16) -join '|')|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); "TIMEOUT 2" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue) + @("primera: " + ($first -join ' / ')) +
  @(SortO @(Get-Content -Encoding UTF8 "$dir\args.txt" -ErrorAction SilentlyContinue) | ForEach-Object { "reabrir: $_" })
# Sesión guardada por una versión anterior (sin conversación): `claude` / `codex` a secas también reanudan.
function Hex([string]$t) { -join ([Text.Encoding]::UTF8.GetBytes($t) | ForEach-Object { $_.ToString('x2') }) }
Remove-Item "$dir\args.txt" -ErrorAction SilentlyContinue
$old = "BW1`nactive 0`ntab 0 S 0 0.5`nL $(Hex 'claude --dangerously-skip-permissions') $(Hex $proj.FullName) run`nL $(Hex 'codex -c model=x') $(Hex $proj.FullName) run`n"
[IO.File]::WriteAllText($env:WARPLITE_SESSION, $old)
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|wait|wait|wait|wait|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); "TIMEOUT 3" }
$got += @(SortO @(Get-Content -Encoding UTF8 "$dir\args.txt" -ErrorAction SilentlyContinue) | ForEach-Object { "antigua: $_" })
$want = @(
  'resume-cmd: claude --resume ID', 'resume-cmd: claude --model opus --resume ID', 'resume-cmd: claude --dangerously-skip-permissions --resume ID',
  'resume-cmd: claude --verbose --resume ID', 'resume-cmd: (no)', 'resume-cmd: (no)', 'resume-cmd: codex --yolo resume ID',
  'resume-cmd: codex -m gpt-5 resume ID', 'resume-cmd: codex resume ID', 'resume-cmd: (no)', 'resume-cmd: C:\bin\claude.exe --resume ID', 'resume-cmd: (no)',
  "resume: claude --dangerously-skip-permissions --model m3 --resume $(Id m3)", "resume: claude --model m1 --resume $(Id m1)",
  "resume: claude --model m2 --resume $(Id m2)", "resume: claude --model m4 --resume $(Id m4)",
  "resume: clauded --model m5 --resume $(Id m5)",
  "resume: codex --yolo -m c2 resume $(Id c2)", "resume: codex -m c1 resume $(Id c1)",
  ('primera: ' + ((@($launch | ForEach-Object { $_ -replace '^clauded', 'claude --dangerously-skip-permissions' }) + 'claude --model m7') -join ' / ')),
  "reabrir: claude --dangerously-skip-permissions --model m3 --resume $(Id m3)",
  "reabrir: claude --dangerously-skip-permissions --model m5 --resume $(Id m5)", 'reabrir: claude --dangerously-skip-permissions --model m6 hola',
  "reabrir: claude --model m1 --resume $(Id m1)",
  "reabrir: claude --model m2 --resume $(Id m2)", "reabrir: claude --model m4 --resume $(Id m4)",
  "reabrir: codex --yolo -m c2 resume $(Id c2)", "reabrir: codex -m c1 resume $(Id c1)",
  'antigua: claude --dangerously-skip-permissions --continue', 'antigua: codex -c model=x resume --last'
)
$fail = 0
for ($i = 0; $i -lt [math]::Max($want.Count, $got.Count); $i++) {
  $ok = $i -lt $got.Count -and $i -lt $want.Count -and $got[$i] -ceq $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
}
# Los claude/codex falsos que lanzó la prueba no se quedan vivos.
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$dir\bin*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\fake-*", "$dir\bin"
"fallos: $fail"
