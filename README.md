# SSH Win Player

A small Windows video/audio player that browses another server over **SSH** and plays
its media files with **libmpv**, without copying the files first. It also plays local files.

![features](https://img.shields.io/badge/Windows-10%2F11-blue)

## Download

`release/SshWinPlayer-win64.zip` – unzip it anywhere and run `SshWinPlayer.exe`.
(The zip has `SshWinPlayer.exe` and `libmpv-2.dll`, which must stay in the same folder.
`release/SshWinPlayer.exe` is the program on its own, if you already have `libmpv-2.dll`.)

## Requirements

- Windows 10 (1809+) or Windows 11, 64-bit.
- The **OpenSSH Client** that comes with Windows (`C:\Windows\System32\OpenSSH\ssh.exe`).
  It is installed by default on current Windows; if not: *Settings → System → Optional features →
  Add a feature → OpenSSH Client*.
- On the server: any normal Linux/BSD/macOS/NAS with `sh`, `ls`, `dd`, `stat` (nothing to install).

## Use

1. Type the host (`user@server`, or a `Host` alias from `%USERPROFILE%\.ssh\config`), optionally a port.
2. Authentication:
   - **Keys / ssh-agent** (recommended): leave the password empty. Keys in `%USERPROFILE%\.ssh`
     and the Windows `ssh-agent` service work, as does a key file chosen with **Key**.
   - **Password**: type it in the Password box (also used as key passphrase). It is never saved.
3. Click **Connect**, browse folders (double-click / Enter, Backspace = up) and double-click a file.
   The other media files of that folder become the playlist (Next/Previous, auto-advance).
4. **Local files** switches to this PC's drives; you can also drop files on the window,
   use **Open...** (Ctrl+O), or pass files on the command line.

Matching subtitles (`movie.srt` next to `movie.mkv`) are loaded automatically.
New host keys are accepted on first connect (`StrictHostKeyChecking=accept-new`); a *changed*
host key is refused.

## Controls

| Button | Action | Key |
|---|---|---|
| ⏮ / ⏭ | previous / next file in folder | `<` / `>` (Enter) |
| ⏪ / ⏩ | -1 min / +1 min | Down / Up |
| -10s / +10s | -10 s / +10 s | Left / Right = ±5 s |
| ◂❙ / ❙▸ | frame back / frame step | `,` / `.` |
| ▶ / ⏸ | play / pause | Space, `p`, right click |
| ⏹ | stop | `q` |
| − / 1.00× / + | slower / reset speed / faster | `[` / Backspace / `]` |
| Subs / Audio | click: next subtitle / audio track. **Hold** (or right-click): list of the first 20 tracks to choose from (Subs also has *Off*) | `j` / `#` |
| Loop | loop current file | `L` |
| Fullscreen | toggle | `f`, double click, Esc leaves |
| ☰ | show / hide the file browser | |

- **Mouse wheel** over the video, seek bar or controls: up = forward 5 s, down = back 5 s
  (hold **Shift** for 30 s).
- All the usual **mpv hotkeys** work (they are forwarded to mpv), and your own
  `%APPDATA%\mpv\mpv.conf` / `input.conf` are read.
- **Volume is fixed at 100%** – the volume keys and wheel bindings are disabled and any change is
  reset. Use the Windows/system volume instead (keyboard volume keys still control Windows).

## Troubleshooting

- A log is written to `%APPDATA%\SshWinPlayer\log.txt` (ssh errors, mpv warnings).
- Settings are in `%APPDATA%\SshWinPlayer\settings.ini`. To use a different ssh client, add
  `sshExe=C:\path\to\ssh.exe` under `[main]`.
- Password login relies on `SSH_ASKPASS`. If it fails with the old OpenSSH 8.1 shipped with some
  Windows 10 builds, use a key, or install a newer client from
  <https://github.com/PowerShell/Win32-OpenSSH/releases> and point `sshExe` at it.

## How it works

One `ssh.exe` process per connection runs a tiny POSIX `sh` loop on the server. Directory
listings use `ls`; file data is read in 64 KiB-aligned blocks with `dd`. The data is handed to
libmpv through a custom stream protocol (`sshwp://`). Browsing and streaming use separate
connections.

**Buffering for distant servers:** every remote file gets a background prefetcher that keeps
**5 seconds before and 5 seconds after** the current position in memory (estimated from file
size and duration). It fetches with many requests in flight at once (pipelining), so a server
with high latency is not limited to one block per round trip. Playback starts once 5 s are
buffered, resumes after a stall with 5 s buffered, and seeking back inside the buffer is instant.
While buffering, the status bar shows `Buffering... NN%`.

## Build

Cross-compile on Linux (Debian/Ubuntu):

```sh
sudo apt-get install g++-mingw-w64-x86-64-posix
./build.sh            # -> build/SshWinPlayer.exe
```

At runtime put `libmpv-2.dll` (64-bit, e.g. from the `mpv-dev-x86_64-*.7z` builds at
<https://sourceforge.net/projects/mpv-player-windows/files/libmpv/>) next to the exe.
The mpv headers are in `third_party/mpv-include` (mpv client API, ISC license).

`libmpv-2.dll` in the release zip is a third-party build of mpv/FFmpeg distributed under the GPL
(source: <https://github.com/mpv-player/mpv>, build scripts: <https://github.com/shinchiro/mpv-winbuild-cmake>).
