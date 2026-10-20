# Claude Code y Codex abiertos al cerrar: al reabrir se reanuda la conversación de cada panel
# (claude --resume <id> / codex resume <id>), no se empieza una nueva. Con claude/codex falsos
# (apuntan lo que reciben) y conversaciones falsas en CLAUDE_CONFIG_DIR y CODEX_HOME.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-reanudar.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude"
$env:CODEX_HOME = "$dir\fake-codex"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$dir\fake-*", "$dir\proj ñ", "$dir\bin", "$dir\args.txt"
$proj = New-Item -ItemType Directory "$dir\proj ñ"
New-Item -ItemType Directory "$dir\bin" | Out-Null
foreach ($n in 'claude', 'codex') { Set-Content -Encoding ASCII "$dir\bin\$n.cmd" "@echo $n %*>>`"%~dp0..\args.txt`"`r`n@ping -n 60 127.0.0.1 >nul" }
$env:PATH = "$dir\bin;$env:PATH"
# Dos conversaciones de Claude en esa carpeta (la b, más reciente) y una de Codex (y otra de codex exec).
$cdir = New-Item -ItemType Directory -Force ("$env:CLAUDE_CONFIG_DIR\projects\" + ($proj.FullName -replace '[^A-Za-z0-9]', '-'))
$a = '11111111-aaaa-4aaa-8aaa-aaaaaaaaaaaa'; $b = '22222222-bbbb-4bbb-8bbb-bbbbbbbbbbbb'
Set-Content "$cdir\$a.jsonl" '{}'; (Get-Item "$cdir\$a.jsonl").LastWriteTime = (Get-Date).AddHours(-2)
Set-Content "$cdir\$b.jsonl" '{}'
$xdir = New-Item -ItemType Directory -Force ("$env:CODEX_HOME\sessions\" + (Get-Date -Format 'yyyy\\MM\\dd'))
$cwdJson = $proj.FullName.Replace('\', '\\')
$x = '019f0000-cccc-7ccc-8ccc-cccccccccccc'; $y = '019f0000-dddd-7ddd-8ddd-dddddddddddd'
Set-Content -Encoding UTF8 "$xdir\rollout-1-$x.jsonl" "{`"type`":`"session_meta`",`"payload`":{`"id`":`"$x`",`"cwd`":`"$cwdJson`",`"source`":`"cli`"}}"
Set-Content -Encoding UTF8 "$xdir\rollout-2-$y.jsonl" "{`"type`":`"session_meta`",`"payload`":{`"id`":`"$y`",`"cwd`":`"$cwdJson`",`"source`":`"exec`"}}"

$unit = @('claude', 'claude --model opus "haz algo"', 'claude --dangerously-skip-permissions --continue', 'claude -r abcdef12-3456 --verbose',
  'claude -p "hola"', 'claude mcp list', 'codex --yolo', 'codex -m gpt-5 "arregla esto"', 'codex resume --last', 'codex exec "x"', 'C:\bin\claude.exe', 'git status')
$cd = "cd '$($proj.FullName)'"
$steps = @('wait', 'wait', 'wait') + ($unit | ForEach-Object { "resume-cmd:$_" }) + @(
  "edit:$cd", 'submit', 'wait', 'edit:claude --model opus "haz algo"', 'submit', 'wait', 'wait',
  'tab', 'wait', 'wait', "edit:$cd", 'submit', 'wait', 'edit:claude --dangerously-skip-permissions', 'submit', 'wait', 'wait',
  'tab', 'wait', 'wait', "edit:$cd", 'submit', 'wait', 'edit:codex --yolo', 'submit', 'wait', 'wait',
  'tab', 'wait', 'wait', 'edit:ping -n 30 127.0.0.1', 'submit', 'wait', 'wait', 'resume', 'quit')
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT 1" }
$first = @(Get-Content "$dir\args.txt" -ErrorAction SilentlyContinue)
Remove-Item "$dir\args.txt" -ErrorAction SilentlyContinue
# Reabrir: debe reanudar cada conversación.
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|wait|wait|wait|wait|quit`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); "TIMEOUT 2" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue) + @("primera: " + ($first -join ' / ')) +
  @(Get-Content "$dir\args.txt" -ErrorAction SilentlyContinue | Sort-Object | ForEach-Object { "reabrir: $_" })
$want = @(
  'resume-cmd: claude --resume ID', 'resume-cmd: claude --model opus --resume ID', 'resume-cmd: claude --dangerously-skip-permissions --resume ID',
  'resume-cmd: claude --verbose --resume ID', 'resume-cmd: (no)', 'resume-cmd: (no)', 'resume-cmd: codex --yolo resume ID',
  'resume-cmd: codex -m gpt-5 resume ID', 'resume-cmd: codex resume ID', 'resume-cmd: (no)', 'resume-cmd: C:\bin\claude.exe --resume ID', 'resume-cmd: (no)',
  "resume: claude --dangerously-skip-permissions --resume $b", "resume: claude --model opus --resume $a", "resume: codex --yolo resume $x",
  'primera: claude --model opus "haz algo" / claude --dangerously-skip-permissions / codex --yolo',
  "reabrir: claude --dangerously-skip-permissions --resume $b", "reabrir: claude --model opus --resume $a", "reabrir: codex --yolo resume $x"
)
$fail = 0
for ($i = 0; $i -lt [math]::Max($want.Count, $got.Count); $i++) {
  $ok = $i -lt $got.Count -and $i -lt $want.Count -and $got[$i] -ceq $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
}
# Lo que lanzó la prueba (cmd de los falsos y sus ping) no se queda vivo.
Get-CimInstance Win32_Process -Filter "Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$dir\bin*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\fake-*", "$dir\bin"
"fallos: $fail"
