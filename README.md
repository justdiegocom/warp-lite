# warp-lite

A small, fast terminal for Windows with the look and shortcuts of [Warp](https://github.com/warpdotdev/warp) — and nothing else. No AI, no account, no telemetry. One ~5 MB `.exe` (Win32 + ConPTY + libvterm + GDI).

![Split panes](docs/screenshot-panes.png)

## Features

- **Tabs and split panes** with Warp's shortcuts: `Ctrl+Shift+D` split right, `Ctrl+Shift+E` split down, `Ctrl+Alt+Arrows` move between panes, `Ctrl+Shift+T` new tab, `Ctrl+Shift+W` close.
- **Input box** at the bottom with the current folder (click it to switch folders), history, and the command coloured green if it exists or underlined in red if not.
- **Picks up where you left off**: tabs, splits, folders and windows come back after a restart. Your last command is waiting in the input box, and anything that was still running (e.g. `ssh … tmux`) is relaunched.
- **Vertical tabs**, renamable and hideable tabs, several windows (drag a tab out to open it in a new one).
- `Ctrl+F` search, `Ctrl+click` links, drop files to paste their path, `Ctrl +/-` zoom.
- Warp's dark theme and font (Hack 13 px), with Nerd Font icons and symbol fallback built in.
- Light on CPU: only the rows that change are redrawn.

![Vertical tabs](docs/screenshot-vertical.png)

## Install

Run `warp-lite-setup.exe` (per-user, no admin). It shows up as **Warp** in the Start menu and in *Installed apps*. Right-click the title bar for the language and *Start with Windows*.

## Build

```sh
tools/build.sh          # cross-compiles with zig (x86_64-windows-gnu) → dist/
```

On Windows: `tools/build.ps1`.

## License

Third-party: libvterm (MIT), Hack (MIT / Bitstream Vera), Symbols Nerd Font (MIT). No Warp code is used; only its shortcuts, colours and default font are replicated.
