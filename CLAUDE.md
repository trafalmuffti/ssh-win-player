# CLAUDE.md

## Rules
- Write fixed bugs to fixed-bugs.txt (append one entry per bug: date, symptom, cause, fix).

## Project
SSH Win Player: a native Win32 (C++17) media player that browses a remote server
over SSH (Windows' bundled OpenSSH `ssh.exe`) and plays video/audio with libmpv.
Local files are supported too.

- `src/main.cpp`   – UI, mpv integration, key/wheel handling, playlist, file browser
- `src/sshconn.*`  – persistent ssh.exe connection running a small command loop on the
                     server: POSIX `sh` script (kRemoteScript) or, for Windows servers
                     without sh, a PowerShell 5.1 script (kPsScript) with the same protocol
- `src/mpv_dyn.h`  – runtime loader for libmpv-2.dll (headers in third_party/mpv-include)
- `res/`           – manifest, icon, version resource
- `build.sh`       – cross-compile with mingw-w64 on Linux → build/SshWinPlayer.exe
- `release/`       – prebuilt exe + zip with libmpv-2.dll

Key design points:
- Remote files are fed to mpv through a custom stream protocol `sshwp://<percent-encoded path>`
  (mpv_stream_cb_add_ro). Two ssh connections: one for browsing (UI thread), one for streaming.
- Remote shell selection (SshConn::startLocked): try sh, then PowerShell (`powershell
  -EncodedCommand <bootstrap>` reading kPsScript from stdin up to `#SWP-END#`; cmd.exe limits the
  command line to 8191 chars). Fatal ssh errors (auth, host key, network) are not retried. The
  working shell is remembered per server in settings.ini `[backend]`. PowerShell paths are
  POSIX-style internally ("/" = drives, "/C:/Users/me", "//srv/share") and shown as C:\... in the UI.
- Microsoft Coreutils for Windows (Build 2026) has no sh, no dd (and reportedly no uname), so it
  cannot run kRemoteScript by itself; kRemoteScript falls back to `tail -c | head -c` when dd is
  missing. The PowerShell script does not depend on Coreutils.
- Remote buffering: each RemoteStream has a prefetch thread + 256 KiB block cache keeping 5 s
  before/after the read position, fetched with SshConn::readPipelined (many requests in flight).
  mpv: cache-pause-initial / cache-pause-wait=5 / demuxer-seekable-cache.
- Subs / Audio buttons (TrackBtnProc): click cycles; hold 400 ms or right-click opens a popup of the
  first 20 tracks from the observed `track-list` (ShowTrackMenu); selecting sets sid / aid.
- Home button (GoHome): local = %USERPROFILE%. Remote (DetectRemoteSystem at connect): uname
  MINGW*/MSYS*/CYGWIN*/Windows_NT or no uname -> server %USERPROFILE% via `cygpath -u` (W command);
  otherwise uname exists -> Unix -> ~ (P command); nothing found -> "/".
- F1 opens https://mpv.io/manual/stable/#keyboard-control (app key path + forced mpv binding
  `F1 script-message swp-help`, so it also works when mpv's window has focus).
- Volume is locked at 100% (bindings + property observer). Use the system volume.
- Mouse wheel seeks (±5 s, Shift ±30 s). mpv hotkeys are forwarded via the `keypress` command.
- Password auth uses the exe itself as SSH_ASKPASS (env SSHWINPLAYER_ASKPASS=1).
