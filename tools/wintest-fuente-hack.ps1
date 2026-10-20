# Going back to Hack (embedded in the exe, not installed) from another font in the Font menu.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_SESSION = "$dir\session-hack.txt"
Remove-Item -Force -ErrorAction SilentlyContinue "$env:WARPLITE_SESSION*"
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList '--script "wait|font:Consolas|font|font:Hack|font|quit"' -PassThru
if (-not $p.WaitForExit(60000)) { $p.Kill(); 'TIMEOUT' }
$b = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench")
$fail = 0
if ($b[0] -notlike 'font: `[Consolas`]*') { $fail++; "FALLO $($b[0])" } else { "OK   $($b[0].Substring(0, 24))" }
if ($b[1] -notlike 'font: `[Hack`]*') { $fail++; "FALLO $($b[1])" } else { "OK   $($b[1].Substring(0, 20))" }
"fallos: $fail"
