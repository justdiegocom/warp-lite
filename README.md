# warp-lite

A small, fast terminal for Windows with the look and shortcuts of [Warp](https://github.com/warpdotdev/warp) — and nothing else. No AI, no account, no telemetry. One ~5 MB `.exe` (Win32 + ConPTY + libvterm + GDI + DirectWrite).

![Split panes](docs/screenshot-panes.png)

## Features

- **Tabs and split panes** with Warp's shortcuts: `Ctrl+Shift+D` split right, `Ctrl+Shift+E` split down, `Ctrl+Alt+Arrows` move between panes, `Ctrl+Shift+T` new tab, `Ctrl+Shift+W` close.
- **Jump to any terminal** with `Ctrl+Shift+K`: every pane of every window, with what it's running and its folder; type to filter, `Enter` to go there. Tabs running Claude Code are named after their conversation.
- **Open Warp here** in the Explorer right-click menu (on a folder, a drive or inside a folder): a new tab opens already in that folder. On Windows 11 it's under *Show more options*.
- **Input box** at the bottom with the current folder (click it to switch folders), history, and the command coloured green if it exists or underlined in red if not.
- **Tab completion** like Warp's menu: commands, `-parameters` and paths (`↑/↓` or `Tab` to move, `Enter` to pick).
- **Picks up where you left off**: the main window (the first one Warp opens) always comes back after a restart with its tabs, splits and folders, however it was closed. Other windows (a tab dragged out, a second Warp) come back too only if *Reopen on restart* is ticked in their title-bar menu; an update restart brings back every window. Your last command is waiting in the input box, and anything that was still running (e.g. `ssh … tmux`) is relaunched. Claude Code and Codex resume the exact conversation each pane had (`claude --resume <id>`, `codex resume <id>`), even with several of them open in the same folder, instead of starting a new one.
- **Unread badges**: when Claude Code, Codex or any other program finishes or waits for you in a pane you aren't looking at, that pane, its tab (or the hidden-tabs button) and the Windows taskbar button show a count, like a chat app, and the taskbar button flashes if Warp isn't in front (not over a full-screen game). Looking at the pane clears the count; the pane itself keeps pulsing in the accent color (its outline, its header and its tab) until you click or type in it, so you see which one it was (steady if Windows animations are off). No sound. With *Windows notifications* on (title-bar menu, off by default) each one is also a silent Windows notification, one per pane; clicking it brings Warp to front on that tab and pane. See [Unread badges](#unread-badges) for what to set up.
- **Reconnects dropped ssh sessions**: when the connection drops (`client_loop: send disconnect: Connection reset`, `closed by remote host`…), `ssh host` or `ssh -t host "tmux new -A -s x"` (also `tmux attach`, `screen -R`) is launched again by itself after 2, 4, 8, 16 and then every 30 s, sooner when the network comes back, until it connects. A small card shows the attempt; `Esc` or typing cancels. A normal exit, a tmux detach or an authentication error never retries, and neither do other remote commands.
- **Vertical tabs**, renamable and hideable tabs, drag tabs to reorder them, several windows (drag a tab or a split pane out to open it in a new one, or onto another Warp window to join them: on its tabs it becomes a tab there, on one of its panes it splits that pane on the side you drop it, and that window shows where it will go while you drag; what was running in it, like `ssh … tmux` or Claude Code, is launched again there).
- `Ctrl+F` search, file and folder paths that exist shown in their own color, `Ctrl+click` links and file paths (also when wrapped over several lines, quoted or not, with `:line` behind): files open with their app, folders in Explorer, and programs and scripts are only shown selected in Explorer, never run. Drop files to paste their path, `Ctrl +/-` zoom.
- `Ctrl+C` copies the selection; text copied inside tmux/vim over SSH reaches the Windows clipboard (OSC 52).
- Warp's dark theme and font (Hack 13 px), with Nerd Font icons and symbol fallback built in. Emoji in color, and box drawing, blocks and powerline separators drawn over the whole cell like Windows Terminal does, so boxes and tables join up whatever the font.
- **Choose the font** by right-clicking the title bar: Hack, Cascadia Mono, Consolas, JetBrains Mono, Fira Code and other Google Fonts (downloaded once, the first time you pick them), or any other monospaced font you have installed. It changes in every window at once. The same menu has the **font size** (of that window) and the **text brightness** (from #AAAAAA to white; #DDDDDD by default, #CCCCCC is Windows Terminal's), which also changes in every window.
- Light on CPU: only the rows that change are redrawn.

![Vertical tabs](docs/screenshot-vertical.png)

## Unread badges

A pane counts one when its program sends a desktop notification (OSC 9, OSC 777 `notify`, OSC 99), rings the bell, or stops working: Claude Code's title spinner (◐ ◑) turning into ✳, or Codex's status (OSC 7501) leaving `working`. A notification and its bell arriving together count once, and `OSC 9;9` (folder) and `OSC 9;4` (progress) never count. A Windows notification takes the program's own title and text when it sent one, otherwise the pane's name and folder; it leaves the action center once the pane is looked at, and none is shown over a full-screen game or a presentation. They need the installed Warp (Windows shows them under its Start menu entry), not the portable exe.

- **Claude Code**, run in Warp or over plain `ssh`: nothing to set up (its title shows when it stops, to finish or to ask for a permission). Inside tmux its title stays still, so it has to ring the bell. Either set `"preferredNotifChannel": "terminal_bell"` in `~/.claude/settings.json` where it runs (it rings when it waits for a permission, and about a minute after finishing if you haven't typed), or, for a badge as soon as each reply ends, add these hooks instead:

  ```json
  "hooks": {
    "Stop": [{ "hooks": [{ "type": "command", "command": "[ -n \"$TMUX_PANE\" ] && printf '\\a' > \"$(tmux display -p -t \"$TMUX_PANE\" '#{pane_tty}')\"" }] }],
    "Notification": [{ "matcher": "permission_prompt", "hooks": [{ "type": "command", "command": "[ -n \"$TMUX_PANE\" ] && printf '\\a' > \"$(tmux display -p -t \"$TMUX_PANE\" '#{pane_tty}')\"" }] }]
  }
  ```

- **Codex**: outside tmux, nothing (its status is enough). Inside tmux, let it notify even though it can't tell which pane you look at (Warp can), in `~/.codex/config.toml`:

  ```toml
  [tui]
  notification_condition = "always"
  ```

- **tmux**: the bell gets through with the defaults (`bell-action any`, `visual-bell off`). For OSC 9 notifications from inside it (Claude Code's `iterm2`, Codex's `osc9`), tmux 3.3 or later needs `set -g allow-passthrough on` in `~/.tmux.conf`; older versions pass them already.

## Phone (preview)

See your terminals from your phone, at home or away, with nothing to set up and no account. Warp is always the host; the phone app (`mobile/ios`, SwiftUI) connects to it.

- **Pair phone…** in the title-bar menu shows a QR code (and the same code as text) that the app scans or pastes. The code works once and expires after 2 minutes; devices that weren't paired are turned away.
- **Paired devices** in the same menu lists them; each one can be removed (it is disconnected at once) or allowed to type (**Allow typing**, off until chosen).
- The app lists every tab and pane of every Warp window, with its folder, whether something is running, whether it's Claude Code or Codex (also behind ssh) and whether it works, waits for you or finished, with Warp's unread count; it shows whether the connection is direct or through a relay and its round-trip time. A phone icon in Warp's title bar shows when a phone is connected.
- **Tap a terminal**: one running Claude Code or Codex reads as a chat, from its own transcript: your messages, its answers in bubbles (markdown: headings, lists, tables, code), its tool calls as compact cards («Edited main.cpp +12 −3», «Ran build.sh · exit 1», consecutive ones grouped as «Ran 3 commands · Read 4 files») with the details on tap, its questions and plans as cards, under a status (working for how long and on what, waiting for you, finished). Also behind `ssh host`, through a script or function that runs ssh (`fr fnviewer`), with tmux too: Warp reads the conversation there through your own ssh setup (non-interactive, read-only, and only while the phone shows that pane). Any other pane shows its text, wrapped to the phone.
- **Subagents**: each one Claude Code starts is a card with its task, type, status, time, tokens and its report, and the ones working are chips at the top; a tap opens that subagent's own conversation (read-only).
- **Questions as buttons**: Claude Code's permission prompts, trust dialog, menus and AskUserQuestion (and Codex's approvals) show their options as big buttons; a tap picks that option in the terminal, only if the same question is still there. When an answer ends with numbered options or a yes/no question, they come as quick replies; nothing is sent until one is tapped.
- **Reply** from the phone, plus Esc, Enter, ↑, ↓, Tab and Ctrl+C keys (folded away in a chat), for a device allowed to type; the title bar says *Phone typing* while it does. Text goes in as one line, and only an explicit Enter submits.
- Works across NATs and mobile data: [iroh](https://iroh.computer) (QUIC, end-to-end encrypted) connects directly when hole punching works and through n0's public relays otherwise.
- The networking lives in `warp-remote.exe` (Rust, `warp-remote/`), next to Warp.exe. It only runs while a phone is paired or pairing is in progress, and ends with the last Warp window. Only then does anything talk to n0's servers (the relay, and the address lookup that lets the phone find this computer again); with no phone paired, Warp makes no network connections beyond the update check. Protocol: `warp-remote/src/proto.rs`.

## Install

Download `warp-lite-setup.exe` from [Releases](https://github.com/justdiegocom/warp-lite/releases/latest) and run it (per-user, no admin); `warp-lite.zip` is the portable exe, and each file has a `.sha256` next to it. It shows up as **Warp** in the Start menu and in *Installed apps*. Right-click the title bar for the language, *Start with Windows*, *Windows notifications*, *Check for updates* and *Update automatically*.

**Updates:** Warp checks this repo's latest release at startup and every 6 hours. With *Update automatically* (on by default) the new installer is downloaded, verified against the release's SHA-256 and installed in the background; a small notice offers *Restart* when it's ready. Otherwise the notice offers *Install*. It never restarts on its own.

## Build

```sh
tools/build.sh          # cross-compiles with zig (x86_64-windows-gnu) → dist/
```

On Windows: `tools/build.ps1` (it takes `dist/warp-remote.exe` from `tools/build.sh`, which cross-compiles it with `cargo zigbuild`).

Phone app, with Xcode and xcodegen: `mobile/ios/build.sh sim` (Simulator), `mobile/ios/build.sh device <TEAM_ID>` or `mobile/ios/build.sh ipa` (unsigned); `mobile/ios/build.sh demo` drives the app in the booted Simulator against a real Warp with real taps (see `WarpRemoteUITests/DemoTests.swift`), and `mobile/ios/build.sh chat-demo <simulator id>` the chat, a subagent, buttons and a quick reply (`ChatDemoTests.swift`). Tests of the protocol: `cargo nextest run` in `warp-remote/`; `warp-remote-cli` is a client like the app, for scripts (`sessions`, `watch`, `type`, `key`, `pick`).

## License

Third-party: libvterm (MIT), Hack (MIT / Bitstream Vera), Symbols Nerd Font (MIT), QR Code generator by Project Nayuki (MIT), iroh (MIT / Apache-2.0). No Warp code is used; only its shortcuts, colours and default font are replicated.
