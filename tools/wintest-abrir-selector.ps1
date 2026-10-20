# «Abrir Warp aquí» (--cd), título de pestaña con el nombre de la conversación de Claude y el
# selector de terminales (Ctrl+Shift+K), también entre dos ventanas. Claude falso: apunta en
# sessions\<pid>.json su conversación y su nombre, como el de verdad.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$env:CLAUDE_CONFIG_DIR = "$dir\fake-claude"
$s1 = "$dir\session-sel.txt"; $s2 = "$dir\session-sel2.txt"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$s1*", "$s2*", "$dir\fake-*", "$dir\proj", "$dir\otra carpeta", "$dir\bin"
New-Item -ItemType Directory "$dir\proj", "$dir\otra carpeta", "$dir\bin", "$env:CLAUDE_CONFIG_DIR\sessions" | Out-Null
$fake = @'
param([string]$tool)
$a = @($args); $model = 'x'
for ($i = 0; $i -lt $a.Count; $i++) { if ($a[$i] -eq '--model') { $model = $a[$i + 1] } }
$name = @{ m1 = @('Arreglar login', 'auto'); m2 = @('proj', 'derived') }[$model]
Set-Content -Encoding UTF8 "$env:CLAUDE_CONFIG_DIR\sessions\$PID.json" "{`"pid`":$PID,`"sessionId`":`"$model-id`",`"name`":`"$($name[0])`",`"nameSource`":`"$($name[1])`",`"status`":`"idle`"}"
Start-Sleep 300
'@
Set-Content -Encoding UTF8 "$dir\bin\fake.ps1" $fake
Set-Content -Encoding ASCII "$dir\bin\claude.cmd" "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake.ps1`" claude %*"
$env:PATH = "$dir\bin;$env:PATH"
function Bench($f) { @(Get-Content -Encoding UTF8 "$f.bench" -ErrorAction SilentlyContinue) }
function WaitFor($f, $pat) { for ($t = 0; $t -lt 240; $t++) { if ((Bench $f) -match $pat) { return }; Start-Sleep -Milliseconds 500 } }

# Ventana 1: dos Claude y una terminal normal; luego espera (mientras, llegan --cd y la ventana 2).
$cd = "cd '$dir\proj'"
$w = @('wait') * 14
$steps1 = @('wait', 'wait', 'wait', "edit:$cd", 'submit', 'wait', 'edit:claude --model m1', 'submit', 'wait', 'wait',
  'tab', 'wait', 'wait', "edit:$cd", 'submit', 'wait', 'edit:claude --model m2', 'submit', 'wait', 'wait',
  'tab', 'wait', 'wait', 'wait', 'titles', 'switcher', 'sw-type:login', 'key:enter', 'active') + $w + @('active', 'titles') + $w + @('active', 'quit')
$env:WARPLITE_SESSION = $s1
$p1 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps1 -join '|')`"" -PassThru
WaitFor $s1 '^active'
# «Abrir Warp aquí» con Warp abierto: como lo pasa el Explorador (barra final + comilla).
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--cd `"$dir\otra carpeta\`"" -PassThru
$fwd = $p.WaitForExit(10000)
"--cd con Warp abierto: el proceso nuevo termina enseguida: $fwd"
WaitFor $s1 '^titles: (\[[^\]]*\] ?){4}'
# Ventana 2 (otra sesión, mismo Warp): su selector ve las terminales de la ventana 1 y salta a una.
$env:WARPLITE_SESSION = $s2
$p2 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"wait|wait|wait|switcher|sw-type:login|key:enter|wait|quit`"" -PassThru
if (-not $p2.WaitForExit(60000)) { $p2.Kill(); 'TIMEOUT 2' }
if (-not $p1.WaitForExit(90000)) { $p1.Kill(); 'TIMEOUT 1' }
# Sin Warp abierto: arranca y la pestaña nueva ya está en esa carpeta.
$env:WARPLITE_SESSION = $s1
$p3 = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--cd `"$dir\proj`" --script `"wait|wait|active|quit`"" -PassThru
if (-not $p3.WaitForExit(60000)) { $p3.Kill(); 'TIMEOUT 3' }

$got = (Bench $s1) + @((Bench $s2) | ForEach-Object { "v2 $_" })
$want = @(
  'titles: `[Arreglar login`] `[claude --model m2`] `[*`]',
  'switcher: `[Arreglar login | proj`] `[claude --model m2 | proj`] `[* | *`] sel=2',
  "active: tab 0 ``[Arreglar login``] $dir\proj*",
  "active: tab 1 ``[*``] $dir\otra carpeta*",
  'titles: `[Arreglar login`] `[*`] `[claude --model m2`] `[*`]',
  "active: tab 0 ``[Arreglar login``] $dir\proj*",
  "active: tab 1 ``[*``] $dir\proj*",
  'v2 switcher: `[* | *`] `[Arreglar login | proj | otra`] `[* | otra carpeta | otra`] `[claude --model m2 | proj | otra`] `[* | * | otra`] sel=0'
)
$fail = 0
for ($i = 0; $i -lt [math]::Max($want.Count, $got.Count); $i++) {
  $ok = $i -lt $got.Count -and $i -lt $want.Count -and $got[$i] -like $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
}
if (-not $fwd) { $fail++ }
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='cmd.exe'" | Where-Object { $_.CommandLine -like "*$dir\bin*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$dir\fake-*", "$dir\bin"
"fallos: $fail"
