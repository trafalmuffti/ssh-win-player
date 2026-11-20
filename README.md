# SSH Win Player

A small Windows video/audio player that browses another server over **SSH** and plays
its media files with **libmpv**, without copying the files first. It also plays local files.

![features](https://img.shields.io/badge/Windows-10%2F11-blue)

## Download

Download the two files in `release/` and put them in the same folder:

- `release/SshWinPlayer.exe` – the program
- `release/libmpv-2.dll` – the player library (64-bit libmpv)

Then run `SshWinPlayer.exe`. `libmpv-2.dll` is larger than GitHub's 100 MB file limit, so it is
stored with Git LFS: on GitHub use the file's **Download** (raw) button; when cloning, have
[Git LFS](https://git-lfs.com) installed (`git lfs install`, then clone as usual).

## Requirements

- Windows 10 (1809+) or Windows 11, 64-bit.
- The **OpenSSH Client** that comes with Windows (`C:\Windows\System32\OpenSSH\ssh.exe`).
  It is installed by default on current Windows; if not: *Settings → System → Optional features →
  Add a feature → OpenSSH Client*.
- On the server, nothing to install:
  - Linux / BSD / macOS / NAS: the usual `sh`, `ls`, `stat` (and `dd`, or `tail`/`head`).
  - Windows with the built-in OpenSSH Server: PowerShell, which every Windows has.

## Use

1. Type the host (`user@server`, or a `Host` alias from `%USERPROFILE%\.ssh\config`), optionally a port.
2. Authentication:
   - **Keys / ssh-agent** (recommended): leave the password empty. Keys in `%USERPROFILE%\.ssh`
     and the Windows `ssh-agent` service work, as does a key file chosen with **Key**.
   - **Password**: type it in the Password box (also used as key passphrase). It is never saved.
3. Click **Connect** (the server's system is detected with `uname` and shown in the status bar), browse folders (double-click / Enter, Backspace = up) and double-click a file.
   The other media files of that folder become the playlist (Next/Previous, auto-advance).
4. **Local files** switches to this PC's drives; you can also drop files on the window,
   use **Open...** (Ctrl+O), or pass files on the command line.

Matching subtitles (`movie.srt` next to `movie.mkv`) are loaded automatically.

### Resume on any device (Unix servers)

On Linux / BSD / macOS / NAS servers, the playback position of every remote video is saved in
one XML file in your home folder **on the server**: `~/.ssh-win-player-positions.xml`. Open the
same video from any PC running SSH Win Player and it continues where you stopped. The position
is saved when you stop, pause, switch files, disconnect or close the app, and every minute while
playing. Videos played to the end (or stopped in the first 5 seconds) are removed from the list.
Saves are merged with the file on the server, so several devices can be used at the same time.

- **Shift**+double-click (or Shift+Enter) starts a video from the beginning instead.
- Windows servers do not get this file (positions are not shared there).
- To turn it off, add `resume=0` under `[main]` in `settings.ini`.
- The status bar shows `• resume synced` when it is active for the connected server.
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
| ⌂ (above the file list) | home folder: your Windows user folder; on a server, `~` for Unix (detected by `uname`) or `%USERPROFILE%` for Windows | |
| | mpv keyboard reference in your web browser | `F1` |

- **Hover over the seek bar** to see the time under the cursor and how far that is from the
  current position, e.g. `12:34  (+2:10)`.
- **Mouse wheel** over the video, seek bar or controls: up = forward 5 s, down = back 5 s
  (hold **Shift** for 30 s).
- All the usual **mpv hotkeys** work (they are forwarded to mpv), and your own
  `%APPDATA%\mpv\mpv.conf` / `input.conf` are read.
- **Volume is fixed at 100%** – the volume keys and wheel bindings are disabled and any change is
  reset. Use the Windows/system volume instead (keyboard volume keys still control Windows).

### Windows servers

A stock Windows OpenSSH Server works: when the server has no POSIX `sh` (its SSH shell is cmd or
PowerShell), the app automatically uses a PowerShell version of its server-side script. Paths are
shown the Windows way (`C:\Users\me\Videos`; `/` lists the drives). You can type `C:\...` or
`\\server\share\...` paths in the path box. Home goes to your `%USERPROFILE%` on the server.
The PowerShell support is new and has not yet been tried against a real Windows server; please
report problems (the log has the details).

**Microsoft Coreutils for Windows** (`winget install Microsoft.Coreutils`, announced at Build
2026) is *not* needed, and on its own it is not enough for the `sh` script either: it brings
`ls`, `cat`, `tail`, `head`, `find`, `grep` and more, but no shell and (currently) no `dd`, and
reportedly no `uname`. So on a server that only has Coreutils, the app uses the PowerShell script.
If a Windows server *does* have a POSIX shell (Git Bash, MSYS2, Cygwin, busybox-w32), the `sh`
script is used; if `dd` is missing there (e.g. when Coreutils' tools come first on the PATH), file
data is read with `tail -c | head -c` instead.

## Troubleshooting

- A log is written to `%APPDATA%\SshWinPlayer\log.txt` (ssh errors, mpv warnings).
- Settings are in `%APPDATA%\SshWinPlayer\settings.ini`. To use a different ssh client, add
  `sshExe=C:\path\to\ssh.exe` under `[main]`. The remote shell that worked for each server
  (`sh` or `powershell`) is remembered under `[backend]`; delete the line to detect it again.
- Password login relies on `SSH_ASKPASS`. If it fails with the old OpenSSH 8.1 shipped with some
  Windows 10 builds, use a key, or install a newer client from
  <https://github.com/PowerShell/Win32-OpenSSH/releases> and point `sshExe` at it.

## How it works

One `ssh.exe` process per connection runs a tiny command loop on the server: a POSIX `sh`
script, or on Windows servers without `sh` a PowerShell script (started with a short
`-EncodedCommand` that reads the rest of the script from stdin). Both answer the same requests.
Directory listings come with sizes from one command per folder (`find -printf`, `stat`, or
.NET on Windows). File data is read in 64 KiB-aligned blocks with `dd` (or `tail | head`, or a
.NET `FileStream` on Windows). The data is handed to
libmpv through a custom stream protocol (`sshwp://`). Browsing and streaming use separate
connections.

**Buffering for distant servers:** every remote file gets a background prefetcher that keeps
**5 seconds before and 5 seconds after** the current position in memory (estimated from file
size and duration). It fetches with many requests in flight at once (pipelining), so a server
with high latency is not limited to one block per round trip. Playback starts once 5 s are
buffered, resumes after a stall with 5 s buffered, and seeking back inside the buffer is instant.

**Back buffer = keyframe interval:** an exact seek has to start decoding at the keyframe before
the target, which can be several seconds earlier (often up to 10 s). So the player keeps
**5 s + one keyframe interval** of already-played data in memory (at least 64 MiB). The
interval is measured from the video's I-frames while playing; until a longer one is measured,
10 s is assumed, so the buffer is big enough from the first second. Stepping back a few seconds
then needs nothing from the server.
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
