# Copying a Claude Code block quote («▎ text» on every line) leaves the bars out and keeps the rest:
# the indent before the bar, empty quote lines and other box characters (│ of a tree) stay as they are.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-citas.txt"
$env:WARPLITE_NO_UPDATE = '1'
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench"
$print = 'cls; $q = [string][char]0x258E; Write-Host ($q + '' INICIO uno dos''); Write-Host $q; Write-Host ($q + '' tres''); ' +
  'Write-Host (''   '' + $q + '' cuatro''); Write-Host ([string][char]0x2502 + ''   FIN arbol'')'
$steps = @('wait', 'wait', 'wait', "edit:$print", 'submit', 'wait', "select:$([char]0x258E) INICIO", 'select-to:FIN', 'copy', 'clipboard', 'quit')
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); 'TIMEOUT' }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue) | ? { $_ -like 'clipboard:*' } | Select -Last 1
$want = "clipboard: INICIO uno dos\n\ntres\n   cuatro\n$([char]0x2502)   FIN arbol"
$got
if ($got -ceq $want) { 'OK   copia sin barras'; 'fallos: 0' } else { "FALLO se esperaba: $want"; 'fallos: 1' }
