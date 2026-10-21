# Una descarga que se corta a mitad (la conexión cae): el actualizador lo reintenta 3 veces y falla con
# «cut off», nunca con «checksum», y no instala nada. Servidor http local que manda 300 KB de 1 MB y corta.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-cut.txt"
$env:WARPLITE_NO_UPDATE = '1'
$env:WARPLITE_FAKE_VERSION = '0.0.1'
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$env:WARPLITE_SESSION.updlog"
$port = 18000 + (Get-Random -Maximum 1000)
$base = "http://127.0.0.1:$port"
$env:WARPLITE_UPDATE_URL = "$base/latest.json"
$srv = Start-Job -ArgumentList $port, $base {
  param($port, $base)
  $l = [Net.HttpListener]::new(); $l.Prefixes.Add("http://127.0.0.1:$port/"); $l.Start()
  $json = '{"tag_name": "v9.9.9", "assets": [{"name": "warp-lite-setup.exe", "browser_download_url": "' + $base + '/setup.exe", "digest": "sha256:' + ('0' * 64) + '"}]}'
  $hits = 0
  while ($true) {
    $c = $l.GetContext()
    if ($c.Request.Url.AbsolutePath -eq '/latest.json') {
      $b = [Text.Encoding]::UTF8.GetBytes($json); $c.Response.ContentLength64 = $b.Length
      $c.Response.OutputStream.Write($b, 0, $b.Length); $c.Response.Close()
    } else {
      $hits++; "hit $hits" | Out-File -Append "$env:TEMP\wl-cut-hits.txt"
      $c.Response.ContentLength64 = 1000000
      $b = [byte[]]::new(300000); $c.Response.OutputStream.Write($b, 0, $b.Length); $c.Response.OutputStream.Flush()
      $c.Response.Abort()
    }
  }
}
Remove-Item -Force -ErrorAction SilentlyContinue "$env:TEMP\wl-cut-hits.txt"
Start-Sleep 2
$steps = @('wait', 'auto-update:off', 'update-check', 'wait', 'wait', 'update-state', 'update-click') + @('wait') * 20 + @('update-state', 'quit')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$($steps -join '|')`"" -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); "TIMEOUT" }
Stop-Job $srv; Remove-Job $srv -Force
$hits = @(Get-Content "$env:TEMP\wl-cut-hits.txt" -ErrorAction SilentlyContinue).Count
Remove-Item -Force -ErrorAction SilentlyContinue "$env:TEMP\wl-cut-hits.txt"
Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue
$log = Get-Content -Raw -Encoding UTF8 "$env:WARPLITE_SESSION.updlog" -ErrorAction SilentlyContinue
$log
$fail = 0
if ($hits -ne 3) { $fail++; "FALLO intentos de descarga: $hits (se esperaban 3)" } else { 'OK   3 intentos de descarga' }
if ($log -notmatch 'cut off|read') { $fail++; 'FALLO el error no dice que la descarga se cortó' } else { 'OK   error de descarga cortada' }
if ($log -match 'checksum') { $fail++; 'FALLO salió «checksum»' }
"fallos: $fail"
