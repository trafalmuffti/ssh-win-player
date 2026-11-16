# CLAUDE.md

## Rules
- Write fixed bugs to fixed-bugs.txt (append one entry per bug: date, symptom, cause, fix).

## Project
SSH Win Player: a native Win32 (C++17) media player that browses a remote server
over SSH (Windows' bundled OpenSSH `ssh.exe`) and plays video/audio with libmpv.
Local files are supported too.

- `src/main.cpp`   – UI, mpv integration, key/wheel handling, playlist, file browser
- `src/sshconn.*`  – persistent ssh.exe connection running a small POSIX `sh` command
                     loop on the server (list / size / aligned `dd` block reads)
- `src/mpv_dyn.h`  – runtime loader for libmpv-2.dll (headers in third_party/mpv-include)
- `res/`           – manifest, icon, version resource
- `build.sh`       – cross-compile with mingw-w64 on Linux → build/SshWinPlayer.exe
- `release/`       – prebuilt exe + zip with libmpv-2.dll

Key design points:
- Remote files are fed to mpv through a custom stream protocol `sshwp://<percent-encoded path>`
  (mpv_stream_cb_add_ro). Two ssh connections: one for browsing (UI thread), one for streaming.
- Remote buffering: each RemoteStream has a prefetch thread + 256 KiB block cache keeping 5 s
  before/after the read position, fetched with SshConn::readPipelined (many requests in flight).
  mpv: cache-pause-initial / cache-pause-wait=5 / demuxer-seekable-cache.
- Subs / Audio buttons (TrackBtnProc): click cycles; hold 400 ms or right-click opens a popup of the
  first 20 tracks from the observed `track-list` (ShowTrackMenu); selecting sets sid / aid.
- Home button (GoHome): local = %USERPROFILE%; remote = ~ if the server has uname (U command,
  detected at connect, g_remoteIsUnix / g_remoteHome), else "/".
- F1 opens https://mpv.io/manual/stable/#keyboard-control (app key path + forced mpv binding
  `F1 script-message swp-help`, so it also works when mpv's window has focus).
- Volume is locked at 100% (bindings + property observer). Use the system volume.
- Mouse wheel seeks (±5 s, Shift ±30 s). mpv hotkeys are forwarded via the `keypress` command.
- Password auth uses the exe itself as SSH_ASKPASS (env SSHWINPLAYER_ASKPASS=1).
