# Consumo de CPU de warp-lite y sus hijos: en reposo y con 4 paneles con salida continua (20 cambios/s cada uno).
param([string]$Exe = "$PSScriptRoot\warp-lite.exe")
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-cpu.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench"
$spin = 'while (1) { Write-Host -NoNewline ([char]13 + (Get-Date -Format HH:mm:ss.fff)); Start-Sleep -Milliseconds 50 }'
$waits = (1..12 | ForEach-Object { 'wait' }) -join '|'
# Como en un monitor de 2560 px con 4 paneles animándose a la vez (Claude en tmux).
$four = "edit:$spin|submit|splitR|wait|wait|edit:$spin|submit|splitR|wait|wait|edit:$spin|submit|splitD|wait|wait|edit:$spin|submit"
$p = Start-Process -FilePath $Exe -ArgumentList "--script `"move:0,0,2560,1380|wait|wait|$waits|$four|wait|wait|prime|$waits|$waits|verify|splitR|wait|splitD|wait|splitR|wait|bench|quit`"" -PassThru
function Snap {
  $ids = @($p.Id) + @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" | ForEach-Object ProcessId)
  $r = @{}
  foreach ($id in $ids) { $q = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($q) { $r[$q.Name] = $q.TotalProcessorTime.TotalMilliseconds } }
  $r
}
Start-Sleep 7
$a = Snap; Start-Sleep 10; $b = Snap
"REPOSO (10 s, % de un núcleo):"; foreach ($k in $b.Keys) { "  {0,-14} {1,6:N1} %" -f $k, (($b[$k] - $a[$k]) / 100) }
Start-Sleep 14
$a = Snap; Start-Sleep 10; $b = Snap
"4 PANELES CON SALIDA CONTINUA (10 s, % de un núcleo):"; foreach ($k in $b.Keys) { "  {0,-14} {1,6:N1} %" -f $k, (($b[$k] - $a[$k]) / 100) }
if (-not $p.WaitForExit(60000)) { $p.Kill() }
Get-Content "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue
