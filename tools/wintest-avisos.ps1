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
# A second Warp (session-atencion.txt.bench): the attention pulse and the Windows notifications. A counted event leaves
# its pane pulsing after its badge is gone (going to its tab, bringing the window back), until a click or a key in it
# (not a bare Shift); the pulse timer runs only while a pane in view pulses and the window isn't minimized, and not at
# all with animations off (a steady outline); in a split tab only that pane. With «Windows notifications» on, each
# counted event asks for a toast (logged, never shown from a test): the notification's own title and body, one per
# finish (OSC 9 + BEL is one), the same tag for the same pane (replaced), none with the setting off; a click on it
# (Warp.exe --toast <its URI>, another process) goes to its tab and pane, still pulsing, and crafted links (a quote
# breaking out with --script, spaces) do nothing. The real WinRT calls up to Show, with their HRESULTs. Screenshots of the outline bright and dim, in a split tab and steady, and 12 frames of
# one breath (atencion-f*.bmp, for a video).
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
function Check([string[]]$got, [string[]]$want) {
  $n = 0
  for ($i = 0; $i -lt $want.Count; $i++) {
    $ok = $i -lt $got.Count -and $got[$i] -like $want[$i]
    if (-not $ok) { $n++ }
    "{0} {1}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $(if ($i -lt $got.Count) { $got[$i] } else { "(falta) $($want[$i])" })
  }
  for ($i = $want.Count; $i -lt $got.Count; $i++) { "     $($got[$i])" }
  $script:fail += $n
}
$fail = 0
Check $got $want
foreach ($f in 'panel', 'ocultas', 'pestanas', 'vertical', 'icono-3', 'icono-12', 'icono-20') {
  if (-not (Test-Path "$dir\avisos-$f.bmp")) { $fail++; "FALLO falta avisos-$f.bmp" }
}

# Second Warp: attention and Windows notifications (tab 0 is pane 1, tab 1 is pane 2, its split pane 3).
$env:WARPLITE_SESSION = "$dir\session-atencion.txt"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$env:WARPLITE_SESSION.notify",
  "$env:WARPLITE_SESSION.appdata", "$dir\atencion-*.bmp", "$dir\pwned.bmp"
function Feed([string]$to, [string]$t) { 'feed:' + $to + ':' + (Hex $t) }
$frames = foreach ($ms in 0..11) { "pulse-at:$($ms * 100)", ('shot:{0}\atencion-f{1:d2}.bmp' -f $dir, $ms) }
$steps2 = @('wait', 'wait', 'wait', 'focus:on', 'tab', 'wait', 'wait', 'motion:windows', 'attention', 'motion:on', 'toasts:on') +
  # An event in tab 0 while tab 1 is in view: it pulses (timer off while out of view), and stays pulsing in view after
  # its badge is cleared; Shift doesn't end it, a click does.
  @((Feed 0 "$E]777;notify;Build;done$B"), 'wait', 'notes', 'attention', 'toasts', 'goto:0', 'attention', 'badges',
    'pulse-at:0', "shot:$dir\atencion-claro.bmp", 'pulse-at:600', "shot:$dir\atencion-oscuro.bmp") + $frames +
  @('pulse-at:off', 'key:shift', 'attention', 'click-pane:0', 'attention') +
  # One toast per finish (OSC 9 and its bell), the same tag for the next one.
  @('goto:1', (Feed 0 "$E]9;one$B$B"), 'wait', 'notes', 'toasts', (Feed 0 "$E]9;two$B"), 'wait', 'notes', 'toasts') +
  # The pane in view with the window not in front: it pulses, also unfocused; minimized, the timer stops.
  @('focus:off', (Feed 1 "$E]9;bg$B"), 'wait', 'notes', 'toasts', 'attention', 'minimize', 'attention', 'restore', 'attention',
    'focus:on', 'badges', 'attention', 'key:esc', 'attention') +
  # A split tab: only the pane that said it pulses; a click in it ends it.
  @('splitR', 'wait', 'wait', (Feed '1/0' "$E]9;left$B"), 'wait', 'notes', 'toasts', 'attention', "shot:$dir\atencion-panel.bmp",
    'click-pane:0', 'attention') +
  # A click on the toast (another Warp.exe with its URI): its tab and pane, still pulsing. Crafted links (a quote and
  # --script, spaces) from a clean environment: they end at once and do nothing. The toast's own URI with a quote and
  # more after it, or a character after it, for this Warp: nothing either (tab 1 stays). Then animations off: steady.
  @((Feed 0 "$E]9;go$B"), 'wait', 'notes', 'toasts', 'where', 'toast-click', 'toast-click-done', 'where', 'attention',
    "toast-click:warp-lite:focus/1/2/3`" --script `"shot:$dir\pwned.bmp", 'toast-click-done',
    'toast-click:warp-lite:focus/1/2/3 --window x', 'toast-click-done', 'where',
    'goto:1', "toast-click+:`" --cd `"C:\", 'toast-click-done', 'toast-click+:x', 'toast-click-done', 'where', 'goto:0',
    'motion:off', 'attention', "shot:$dir\atencion-fija.bmp", 'motion:on') +
  # The setting off: counted, no toast. On again, Claude Code's title (its spinner, then ✳): the pane's name and folder.
  # The real WinRT calls (never Show).
  @('toasts:off', 'goto:1', (Feed 0 "$E]9;quiet$B"), 'wait', 'notes', 'toasts', 'toasts:on',
    (Feed 0 ("$E]0;" + [char]0x25D0 + " Fix tests$B")), 'wait', (Feed 0 ("$E]0;" + [char]0x2733 + " Fix tests$B")), 'wait', 'notes', 'toasts',
    'toast-probe', 'quit')
$arg = ($steps2 -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(400000)) { $p.Kill(); "TIMEOUT" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$toast = 'toast: tag=p1 group=w* accepts=* <toast launch="warp-lite:focus/*/*/1" activationType="protocol"><visual><binding template="ToastGeneric">'
$want = @(
  'attention: tab0/pane0=0 tab1/pane0=0 timer=0 motion=* back=*',
  'notes: alert:counted', 'attention: tab0/pane0=1 tab1/pane0=0 timer=0 motion=1 back=*',
  "$toast<text>Build</text><text>done</text><text placement=`"attribution`">*</text></binding></visual><audio silent=`"true`"/></toast>",
  'attention: tab0/pane0=1 tab1/pane0=0 timer=1 motion=1 back=*', 'badges: tab0=0 tab1=0 window=0 hidden=0 fg=1',
  'attention: tab0/pane0=1 tab1/pane0=0 timer=1 motion=1 back=*', 'attention: tab0/pane0=0 tab1/pane0=0 timer=0 motion=1 back=*',
  'notes: alert:counted bell:merged', "$toast<text>one</text><text>*:\*</text><text placement=`"attribution`">*",
  'notes: alert:counted', "$toast<text>two</text>*",
  'notes: alert:counted', 'toast: tag=p2 group=w* <toast launch="warp-lite:focus/*/*/2"*<text>bg</text>*',
  'attention: tab0/pane0=1 tab1/pane0=1 timer=1 motion=1 back=*', 'attention: tab0/pane0=1 tab1/pane0=1 timer=0 motion=1 back=*',
  'attention: tab0/pane0=1 tab1/pane0=1 timer=1 motion=1 back=*', 'badges: tab0=2 tab1=0 window=2 hidden=0 fg=1',
  'attention: tab0/pane0=1 tab1/pane0=1 timer=1 motion=1 back=*', 'attention: tab0/pane0=1 tab1/pane0=0 timer=0 motion=1 back=*',
  'notes: alert:counted', 'toast: tag=p2 group=w*<text>left</text>*',
  'attention: tab0/pane0=1 tab1/pane0=1 tab1/pane1=0 timer=1 motion=1 back=*', 'attention: tab0/pane0=1 tab1/pane0=0 tab1/pane1=0 timer=0 motion=1 back=*',
  'notes: alert:counted', "$toast<text>go</text>*", 'where: tab1/pane0', 'toast-click: ended 0', 'where: tab0/pane0',
  'attention: tab0/pane0=1 tab1/pane0=0 tab1/pane1=0 timer=1 motion=1 back=*', 'toast-click: ended 0', 'toast-click: ended 0', 'where: tab0/pane0',
  'toast-click: ended 0', 'toast-click: ended 0', 'where: tab1/pane0',
  'attention: tab0/pane0=1 tab1/pane0=0 tab1/pane1=0 timer=0 motion=0 back=*',
  'notes: alert:counted', 'toast: (none)',
  'notes: idle:counted', "$toast<text>* Fix tests</text><text>*:\*</text></binding>*",
  'toast-probe: combase=1 statics=0x00000000 notifier=0x00000000 history=0x00000000 factory=0x00000000 xml=0x00000000 create=0x00000000 tag=0x00000000 setting=*'
)
Check $got $want
foreach ($f in 'claro', 'oscuro', 'panel', 'fija', 'f00', 'f11') {
  if (-not (Test-Path "$dir\atencion-$f.bmp")) { $fail++; "FALLO falta atencion-$f.bmp" }
}
# The crafted links ran nothing and opened no Warp of their own.
foreach ($f in "$dir\pwned.bmp", "$env:WARPLITE_SESSION.appdata\warp-lite") { if (Test-Path $f) { $fail++; "FALLO existe $f" } }

# Third Warp: the case as it happens, through ConPTY (a bare BEL, which is what tmux passes on from Claude Code's hook)
# while Warp isn't in front, then Warp in front again. (a) A tab with one pane: the badge goes, the pulse stays (its
# outline and its tab, drawn by the timer into what the window shows) until a click in it. (b) The right pane of a
# split, the left one active: badge and pulse stay (outline, header, tab); keys in the left pane don't end it, a click
# in the right one does. Frames of (b) as the window shows them, for a video (caso-f*.bmp).
$env:WARPLITE_SESSION = "$dir\session-caso.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $env:WARPLITE_SESSION, "$env:WARPLITE_SESSION.bench", "$dir\caso-*.bmp"
$ring = "edit:Write-Host 'Claude Code finished, waiting for you'; Start-Sleep 5; [Console]::Write([char]7)"
$samples = @('back-sample') * 8
$video = foreach ($k in 0..29) { 'backshot:{0}\caso-f{1:d2}.bmp' -f $dir, $k }
$steps3 = @('wait', 'wait', 'wait', 'focus:on', 'motion:on') +
  @($ring, 'submit', 'focus:off') + @('wait') * 5 + @('notes', 'attention', 'focus:on', 'badges', 'attention', 'prime') + $samples +
  @('click-pane:0', 'attention') +
  @('splitR', 'wait', 'wait', $ring, 'submit', 'left', 'focus:off') + @('wait') * 6 + @('notes', 'focus:on', 'badges', 'attention',
    'edit:typed', 'key:esc', 'attention', 'prime') + $samples + $video + @('click-pane:1', 'attention', 'badges', 'quit')
$arg = ($steps3 -join '|').Replace('"', '\"')
$p = Start-Process -FilePath "$dir\warp-lite.exe" -ArgumentList "--script `"$arg`"" -PassThru
if (-not $p.WaitForExit(300000)) { $p.Kill(); "TIMEOUT" }
$got = @(Get-Content -Encoding UTF8 "$env:WARPLITE_SESSION.bench" -ErrorAction SilentlyContinue)
$want = @(
  'notes: bell:counted', 'attention: tab0/pane0=1 timer=1 motion=1 back=0', 'badges: tab0=0 window=0 hidden=0 fg=1',
  'attention: tab0/pane0=1 timer=1 motion=1 back=0') + @('back: outline=* header=- tab=*') * 8 + @(
  'attention: tab0/pane0=0 timer=0 motion=1 back=1',
  'notes: bell:counted', 'badges: tab0=1`[0,1`] window=1 hidden=0 fg=1', 'attention: tab0/pane0=0 tab0/pane1=1 timer=1 motion=1 back=1',
  'attention: tab0/pane0=0 tab0/pane1=1 timer=1 motion=1 back=1') + @('back: outline=* header=* tab=*') * 8 + @(
  'attention: tab0/pane0=0 tab0/pane1=0 timer=0 motion=1 back=1', 'badges: tab0=0`[0,0`] window=0 hidden=0 fg=1')
Check $got $want
# Drawn by the timer: each of outline, header and tab takes at least 3 shades over the samples, and the outline is
# never the plain background.
foreach ($set in @(@($got | Where-Object { $_ -like 'back: * header=- *' }), @($got | Where-Object { $_ -like 'back: * header=[0-9A-F]*' }))) {
  foreach ($part in 'outline', 'header', 'tab') {
    $v = @($set | ForEach-Object { if ($_ -match "$part=(\S+)") { $Matches[1] } } | Where-Object { $_ -ne '-' } | Sort-Object -Unique)
    if ($v.Count -eq 0) { continue }
    $ok = $v.Count -ge 3 -and -not ($part -eq 'outline' -and $v -contains '050505')
    if (-not $ok) { $fail++ }
    "{0} {1}: {2}" -f $(if ($ok) { 'OK  ' } else { 'FALLO' }), $part, ($v -join ' ')
  }
}
if (-not (Test-Path "$dir\caso-f29.bmp")) { $fail++; 'FALLO falta caso-f29.bmp' }
"fallos: $fail"
