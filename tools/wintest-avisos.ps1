# Unread badges: a program in a pane that isn't in view (another tab, an inactive pane, the window not in front) says
# it has finished, and that pane, its tab, the hidden-tabs button and the window total count it. Through the real path
# (a PowerShell command in the pane writes the sequence, through ConPTY): OSC 9 text, two events, OSC 777 notify, BEL,
# OSC 9 + BEL together (one), OSC 9;9 folder and 9;4 progress (none), kitty's OSC 99 chunks (one), OSC 7501
# working -> done (one), Claude Code's title going from its spinner to ✳ (one; its bell 3 s later is the same news) and
# an OSC 9 wrapped for tmux passthrough with no tmux to unwrap it (one, through its bell: ConPTY passes the DCS on and
# libvterm ends it at the inner ESC, printing the rest as before; find-state only reports it). An event in the pane in
# view (none), with the window not in front (one, gone when it comes back), going to the tab (clears it), an inactive
# pane of the tab in view, a hidden tab. The same sequences fed straight to the terminal, without ConPTY (feed:).
# Screenshots of the badges across the top, at the side, on a pane header and on the hidden-tabs button, and the
# taskbar overlay icon. Results in session-avisos.txt.bench.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$env:WARPLITE_SESSION = "$dir\session-avisos.txt"
$env:WARPLITE_NO_UPDATE = '1'
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$dir\avisos-*.bmp"
$pre = '$e = [char]27; $b = [char]7; $s = $e + ''\''; '
# Runs <emit> in tab 0 five seconds later, with tab 1 in view meanwhile; then what the events did and the badges.
function Bg([string]$emit, [int]$waits = 5) {
  @('goto:0', "edit:${pre}Start-Sleep 5; $emit", 'submit', 'goto:1') + @('wait') * $waits + @('notes', 'badges')
}
function Hex([string]$t) { -join ([Text.Encoding]::UTF8.GetBytes($t) | ForEach-Object { '{0:x2}' -f $_ }) }
$E = [string][char]27; $B = [string][char]7
$osc9 = '[Console]::Write($e + '']9;Build done'' + $b)'
$two = '[Console]::Write($e + '']9;one'' + $b); Start-Sleep 2; [Console]::Write($e + '']777;notify;Codex;two'' + $b)'
$bel = '[Console]::Write([string]$b)'
$withBell = '[Console]::Write($e + '']9;Done'' + $b + $b)'
$conEmu = '[Console]::Write($e + '']9;9;C:\Windows'' + $b + $e + '']9;4;3;0'' + $b)'
$kitty = '[Console]::Write($e + '']99;i=7:d=0:p=title;Claude Code'' + $s + $e + '']99;i=7:p=body;Done'' + $s + $e + '']99;i=7:d=1:a=focus;'' + $s)'
$status = '[Console]::Write($e + '']7501;state=working:app=codex'' + $s); Start-Sleep 2; [Console]::Write($e + '']7501;state=done:app=codex'' + $s)'
$title = '[Console]::Write($e + '']0;'' + [char]0x25D0 + '' Fix tests'' + $b); Start-Sleep 2; ' +
  '[Console]::Write($e + '']0;'' + [char]0x2733 + '' Fix tests'' + $b); Start-Sleep 3; [Console]::Write([string]$b)'
$tmux = '[Console]::Write($e + ''Ptmux;'' + $e + $e + '']9;wr'' + ''apped'' + $b + $s)'
$here = "edit:${pre}Start-Sleep 1; [Console]::Write(`$e + ']9;Here' + `$b)"
$steps = @('wait', 'wait', 'wait', 'focus:on', 'tab', 'wait', 'wait') +
  (Bg $osc9) + @('goto:0', 'badges') +
  (Bg $two) + (Bg $bel) + (Bg $withBell) + (Bg $conEmu) + (Bg $kitty) + (Bg $status) + (Bg $title 8) +
  (Bg $tmux) + @('goto:0', 'find:9;wrapped', 'find-state', 'find-close') +
  # The pane in view, then with the window not in front, then back in front.
  @('goto:1', $here, 'submit', 'wait', 'wait', 'notes', 'badges') +
  @('focus:off', $here, 'submit', 'wait', 'wait', 'notes', 'badges', 'taskbar', 'focus:on', 'badges') +
  # An inactive pane of the tab in view: the right one says it, the left one is active.
  @('splitR', 'wait', 'wait', "edit:${pre}Start-Sleep 5; $osc9", 'submit', 'left') + @('wait') * 5 + @('notes', 'badges', "shot:$dir\avisos-panel.bmp", 'right', 'badges') +
  # A hidden tab.
  @('goto:0', "edit:${pre}Start-Sleep 5; $two", 'submit', 'hide-tab') + @('wait') * 6 + @('notes', 'badges', "shot:$dir\avisos-ocultas.bmp", 'goto:0', 'badges') +
  # Badges across the top and at the side: 2 in tab 0, 1 in a third tab (fed).
  (Bg $two 6) + @('tab', 'wait', 'wait', 'goto:1', ('feed:2:' + (Hex "$E]9;x$B")), 'wait', 'notes', 'badges', "shot:$dir\avisos-pestanas.bmp", 'vtabs', "shot:$dir\avisos-vertical.bmp", 'vtabs') +
  # Straight to the terminal, without ConPTY: OSC 9, OSC 777, BEL, 9;9 (none), a raw tmux passthrough (libvterm ends
  # the DCS at the inner ESC and rings the bell; tmux itself would have unwrapped it), OSC 7501.
  @('goto:0', 'goto:1', ('feed:0:' + (Hex "$E]9;x$B")), 'wait', ('feed:0:' + (Hex "$E]777;notify;t;b$B")), 'wait',
    ('feed:0:' + (Hex $B)), 'wait', ('feed:0:' + (Hex "$E]9;9;C:\x$B")), 'wait', ('feed:0:' + (Hex "${E}Ptmux;$E$E]9;raw$B$E\")), 'wait',
    ('feed:0:' + (Hex "$E]7501;state=working:app=codex$E\")), ('feed:0:' + (Hex "$E]7501;state=done:app=codex$E\")), 'wait', 'notes', 'badges') +
  @("badge-icon:3,64,$dir\avisos-icono-3.bmp", "badge-icon:12,64,$dir\avisos-icono-12.bmp", "badge-icon:3,20,$dir\avisos-icono-20.bmp", 'quit')
$arg = ($steps -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(400000)) { $p.Kill(); "TIMEOUT" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$want = @(
  'notes: alert:counted', 'badges: tab0=1 tab1=0 window=1 hidden=0 fg=1',
  'badges: tab0=0 tab1=0 window=0 hidden=0 fg=1',
  'notes: alert:counted alert:counted', 'badges: tab0=2 tab1=0 window=2 *',
  'notes: bell:counted', 'badges: tab0=1 tab1=0 window=1 *',
  'notes: alert:counted bell:merged', 'badges: tab0=1 tab1=0 window=1 *',
  'notes: (none)', 'badges: tab0=0 tab1=0 window=0 *',
  'notes: alert:counted', 'badges: tab0=1 tab1=0 window=1 *',
  'notes: idle:counted', 'badges: tab0=1 tab1=0 window=1 *',
  'notes: idle:counted bell:quiet', 'badges: tab0=1 tab1=0 window=1 *',
  'notes: bell:counted', 'badges: tab0=1 tab1=0 window=1 *', 'find: open *',
  'notes: alert:seen', 'badges: tab0=0 tab1=0 window=0 hidden=0 fg=1',
  'notes: alert:counted', 'badges: tab0=0 tab1=1 window=1 hidden=0 fg=0', 'taskbar: *', 'badges: tab0=0 tab1=0 window=0 hidden=0 fg=1',
  'notes: alert:counted', 'badges: tab0=0 tab1=1`[0,1`] window=1 *', 'badges: tab0=0 tab1=0`[0,0`] window=0 *',
  'notes: alert:counted alert:counted', 'badges: tab0=2 tab1=0`[0,0`] window=2 hidden=2 fg=1', 'badges: tab0=0 tab1=0`[0,0`] window=0 hidden=0 fg=1',
  'notes: alert:counted alert:counted', 'badges: tab0=2 tab1=0`[0,0`] window=2 *',
  'notes: alert:counted', 'badges: tab0=2 tab1=0`[0,0`] tab2=1 window=3 *',
  'notes: alert:counted alert:counted bell:counted bell:counted idle:counted', 'badges: tab0=5 tab1=0`[0,0`] tab2=1 window=6 *'
)
$fail = 0
for ($i = 0; $i -lt $want.Count; $i++) {
  $ok = $i -lt $got.Count -and $got[$i] -like $want[$i]
  if (-not $ok) { $fail++ }
  "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
}
for ($i = $want.Count; $i -lt $got.Count; $i++) { "     $($got[$i])" }
foreach ($f in 'panel', 'ocultas', 'pestanas', 'vertical', 'icono-3', 'icono-12', 'icono-20') {
  if (-not (Test-Path "$dir\avisos-$f.bmp")) { $fail++; "FALLO falta avisos-$f.bmp" }
}
"fallos: $fail"
