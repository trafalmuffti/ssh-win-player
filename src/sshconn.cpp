#include "sshconn.h"

#include <cstdio>
#include <cstring>
#include <deque>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) {  // not valid UTF-8: fall back to the ANSI code page
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring FindSshExe() {
    wchar_t buf[MAX_PATH];
    // Windows' bundled OpenSSH client.
    if (GetEnvironmentVariableW(L"SystemRoot", buf, MAX_PATH)) {
        std::wstring p = std::wstring(buf) + L"\\System32\\OpenSSH\\ssh.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        // 32-bit process on 64-bit Windows.
        p = std::wstring(buf) + L"\\Sysnative\\OpenSSH\\ssh.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    wchar_t* part = nullptr;
    if (SearchPathW(nullptr, L"ssh.exe", nullptr, MAX_PATH, buf, &part)) return buf;
    return L"";
}

static std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

// The remote command loop.  POSIX sh, works with dash/bash/busybox/BSD sh.
// Requests (one field per line):
//   P                 -> OK \n <home dir>
//   U                 -> OK <uname -s>  | ERR (no uname: probably not Unix)
//   W                 -> OK \n <%USERPROFILE% as a POSIX path (cygpath)>  | ERR
//   L \n path         -> OK \n <real path> \n entries... \n //END//   | ERR
//                        entry = <size>:<type>:<name>  (size -1 = unknown;
//                        type d / directory / Directory = folder)
//   S \n path         -> OK <size>                                  | ERR
//   R \n off len \n p -> OK <n> \n <n raw bytes>                    | ERR
//   X \n path \n lines... \n //END//  -> OK | ERR   (write text file, atomic)
// "//END//" can never be a file name because names cannot contain '/'.
static const char* kRemoteScript =
    "sz(){ stat -c %s -- \"$1\" 2>/dev/null || stat -f %z -- \"$1\" 2>/dev/null || wc -c <\"$1\" 2>/dev/null; }\n"
    "LC_ALL=C; export LC_ALL\n"
    // No dd (e.g. Microsoft's Coreutils for Windows): read with tail | head.
    "if command -v dd >/dev/null 2>&1; then RD=dd; else RD=th; fi\n"
    "lsm(){ case $LM in\n"
    "g) find -L . -mindepth 1 -maxdepth 1 -printf '%s:%y:%f\\n' </dev/null 2>/dev/null;;\n"
    "s) stat -L -c '%s:%F:%n' -- * .[!.]* ..?* </dev/null 2>/dev/null;;\n"
    "b) stat -L -f '%z:%HT:%N' -- * .[!.]* ..?* </dev/null 2>/dev/null;;\n"
    "*) ls -1ApL </dev/null 2>/dev/null | while IFS= read -r n; do case \"$n\" in */) printf '%s:d:%s\\n' -1 \"${n%/}\";; *) printf '%s:f:%s\\n' -1 \"$n\";; esac; done;;\n"
    "esac; }\n"
    "LM=n; if [ \"$(find . -maxdepth 0 -printf x 2>/dev/null)\" = x ]; then LM=g; elif stat -L -c %s . >/dev/null 2>&1; then LM=s; elif stat -L -f %z . >/dev/null 2>&1; then LM=b; fi\n"
    "echo __SWP_READY__\n"
    "while IFS= read -r c; do\n"
    "case \"$c\" in\n"
    "P) cd 2>/dev/null; printf 'OK\\n%s\\n' \"$(pwd -P)\";;\n"
    "W) p=''; if [ -n \"$USERPROFILE\" ]; then if command -v cygpath >/dev/null 2>&1;"
    " then p=$(cygpath -u \"$USERPROFILE\" 2>/dev/null); else p=$USERPROFILE; fi; fi;"
    " if [ -n \"$p\" ] && [ -d \"$p\" ]; then printf 'OK\\n%s\\n' \"$p\"; else echo ERR; fi;;\n"
    "U) if command -v uname >/dev/null 2>&1; then printf 'OK %s\\n' \"$(uname -s 2>/dev/null)\"; else echo ERR; fi;;\n"
    "L) IFS= read -r p; if cd \"$p\" 2>/dev/null; then printf 'OK\\n%s\\n' \"$(pwd -P)\";"
    " lsm; echo '//END//'; else echo ERR; fi;;\n"
    "S) IFS= read -r p; s=''; [ -f \"$p\" ] && [ -r \"$p\" ] && s=$(sz \"$p\" </dev/null);"
    " if [ -n \"$s\" ]; then echo \"OK $((s+0))\"; else echo ERR; fi;;\n"
    "R) read -r o l; IFS= read -r p; s=''; [ -f \"$p\" ] && [ -r \"$p\" ] && s=$(sz \"$p\" </dev/null);"
    " if [ -n \"$s\" ]; then n=$((s-o)); [ $n -gt $l ] && n=$l; [ $n -lt 0 ] && n=0; echo \"OK $n\";"
    " if [ $n -gt 0 ]; then if [ \"$RD\" = dd ]; then dd if=\"$p\" bs=65536 skip=$((o/65536)) count=$(((n+65535)/65536)) 2>/dev/null </dev/null;"
    " else tail -c +$((o+1)) \"$p\" 2>/dev/null </dev/null | head -c \"$n\"; fi; fi;"
    " else echo ERR; fi;;\n"
    // X: write a small text file atomically (temp file + mv).  The content
    // arrives as lines up to a "//END//" line, so no dd / byte counts needed.
    "X) IFS= read -r p; t=\"$p.swp-tmp\"; if ( : > \"$t\" ) 2>/dev/null; then w=1; else w=0; t=/dev/null; fi;"
    " while IFS= read -r l; do [ \"$l\" = '//END//' ] && break; printf '%s\\n' \"$l\"; done > \"$t\";"
    " if [ $w = 1 ] && mv -f \"$t\" \"$p\" 2>/dev/null; then echo OK; else [ $w = 1 ] && rm -f \"$t\"; echo ERR; fi;;\n"
    "Q) exit 0;;\n"
    "*) echo ERR;;\n"
    "esac\n"
    "done\n";


// PowerShell version of the same protocol, for Windows servers that have no
// POSIX sh (Windows' OpenSSH server runs cmd or PowerShell; Microsoft's
// Coreutils for Windows adds ls/cat/tail/... but no shell and no dd).
// Written for Windows PowerShell 5.1 (always installed); also runs on pwsh.
// Windows paths are exposed as POSIX-style paths so the client logic stays
// the same:  "/" = list of drives,  "/C:/Users/me" = C:\Users\me,
// "//server/share/x" = \\server\share\x.
static const char* kPsScript = R"PS(
$ErrorActionPreference = 'Stop'
$in = $r
$win = ($PSVersionTable.PSEdition -eq 'Desktop') -or $IsWindows
$utf8 = New-Object System.Text.UTF8Encoding($false)
$out = [Console]::OpenStandardOutput()
$buf = New-Object byte[] 4194304
function W1([string]$s) { $b = $utf8.GetBytes($s + "`n"); $out.Write($b, 0, $b.Length) }
function ToNative([string]$p) {
  if (-not $win) { return $p }
  if ($p.StartsWith('//')) { return $p.Replace('/', '\') }
  $s = $p.TrimStart('/')
  if ($s -match '^[A-Za-z]:$') { $s = $s + '/' }
  return $s.Replace('/', '\')
}
function ToPosix([string]$w) {
  if (-not $win) { return $w }
  if ($w.StartsWith('\\')) { return $w.Replace('\', '/').TrimEnd('/') }
  return '/' + $w.Replace('\', '/').TrimEnd('/')
}
function HomeDir {
  if ($win -and $env:USERPROFILE) { return (ToPosix $env:USERPROFILE) }
  if ($HOME) { return (ToPosix $HOME) }
  return '/'
}
$skip = 6   # FILE_ATTRIBUTE_HIDDEN (2) | FILE_ATTRIBUTE_SYSTEM (4); plain ints, not enum objects
W1 '__SWP_READY__'
$out.Flush()
while ($true) {
  $c = $in.ReadLine()
  if ($c -eq $null) { break }
  try {
    if ($c -eq 'P') { W1 'OK'; W1 (HomeDir) }
    elseif ($c -eq 'U') {
      if ($win) { W1 'OK Windows_NT' } else { $u = ''; try { $u = [string](& uname -s) } catch { }; W1 ('OK ' + $u) }
    }
    elseif ($c -eq 'W') {
      if ($env:USERPROFILE -and [IO.Directory]::Exists($env:USERPROFILE)) { W1 'OK'; W1 (ToPosix $env:USERPROFILE) } else { W1 'ERR' }
    }
    elseif ($c -eq 'L') {
      $p = $in.ReadLine()
      if ($win -and $p -eq '/') {
        $lines = New-Object System.Collections.Generic.List[string]
        foreach ($d in [IO.DriveInfo]::GetDrives()) { if ($d.IsReady) { $lines.Add('-1:d:' + $d.Name.TrimEnd('\')) } }
        W1 'OK'; W1 '/'
        foreach ($l in $lines) { W1 $l }
        W1 '//END//'
      } else {
        $di = New-Object IO.DirectoryInfo((ToNative $p))
        if (-not $di.Exists) { W1 'ERR' } else {
          $lines = New-Object System.Collections.Generic.List[string]
          try {
            foreach ($e in $di.EnumerateFileSystemInfos()) {
              $a = [int]$e.Attributes
              if (-not ($win -and (($a -band $skip) -ne 0))) {
                if (($a -band 16) -ne 0) { $lines.Add('-1:d:' + $e.Name) }
                else { $lines.Add([string]$e.Length + ':f:' + $e.Name) }
              }
            }
          } catch { }
          W1 'OK'; W1 (ToPosix $di.FullName)
          foreach ($l in $lines) { W1 $l }
          W1 '//END//'
        }
      }
    }
    elseif ($c -eq 'S') {
      $p = $in.ReadLine()
      $fi = New-Object IO.FileInfo((ToNative $p))
      if ($fi.Exists) { W1 ('OK ' + $fi.Length) } else { W1 'ERR' }
    }
    elseif ($c -eq 'R') {
      $ol = $in.ReadLine().Split(' ')
      $p = $in.ReadLine()
      $o = [int64]$ol[0]; $l = [int64]$ol[1]
      $fs = $null
      try { $fs = New-Object IO.FileStream((ToNative $p), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete') } catch { }
      if ($fs -eq $null) { W1 'ERR' } else {
        $n = $fs.Length - $o
        if ($n -gt $l) { $n = $l }
        if ($n -lt 0) { $n = 0 }
        if ($buf.Length -lt $n) { $buf = New-Object byte[] $n }
        $got = 0
        if ($n -gt 0) {
          $fs.Position = $o
          while ($got -lt $n) { $k = $fs.Read($buf, [int]$got, [int]($n - $got)); if ($k -le 0) { break }; $got += $k }
        }
        $fs.Close()
        W1 ('OK ' + $got)
        if ($got -gt 0) { $out.Write($buf, 0, [int]$got) }
      }
    }
    elseif ($c -eq 'X') {
      # Not used for Windows servers (no shared resume file there): consume the lines, refuse.
      $p = $in.ReadLine()
      while ($true) { $l = $in.ReadLine(); if ($l -eq $null -or $l -eq '//END//') { break } }
      W1 'ERR'
    }
    elseif ($c -eq 'Q') { exit 0 }
    else { W1 'ERR' }
  } catch { W1 'ERR' }
  $out.Flush()
}
exit 0
)PS";

// Small start-up command passed with -EncodedCommand: it reads the script
// above from stdin (up to the #SWP-END# line) and runs it.  The script itself
// is too long for cmd.exe's 8191 character command line.
static const char* kPsBootstrap =
    "$r=New-Object IO.StreamReader([Console]::OpenStandardInput(),(New-Object Text.UTF8Encoding($false)));"
    "$b=New-Object Text.StringBuilder;"
    "while($true){$l=$r.ReadLine();if($l -eq $null -or $l -eq '#SWP-END#'){break};[void]$b.AppendLine($l)};"
    "Invoke-Expression $b.ToString()";

// Base64 of UTF-16LE text, as PowerShell's -EncodedCommand expects.
static std::wstring PsEncode(const char* script) {
    std::string bytes;
    for (const char* p = script; *p; ++p) { bytes += *p; bytes += '\0'; }  // ASCII -> UTF-16LE
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned v = ((unsigned char)bytes[i] << 16) | ((unsigned char)bytes[i + 1] << 8) | (unsigned char)bytes[i + 2];
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == bytes.size()) {
        unsigned v = (unsigned char)bytes[i] << 16;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += L"==";
    } else if (i + 2 == bytes.size()) {
        unsigned v = ((unsigned char)bytes[i] << 16) | ((unsigned char)bytes[i + 1] << 8);
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += L'=';
    }
    return out;
}

// ssh errors where trying another remote shell cannot help.
static bool IsFatalSshError(const std::string& e) {
    for (const char* s : {"Permission denied", "Host key verification failed", "Could not resolve hostname",
                          "Connection refused", "Connection timed out", "No route to host",
                          "REMOTE HOST IDENTIFICATION HAS CHANGED", "Too many authentication failures",
                          "Connection closed by"})
        if (e.find(s) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// SshConn
// ---------------------------------------------------------------------------
SshConn::~SshConn() { disconnect(); }

void SshConn::setParams(const SshParams& p) {
    std::lock_guard<std::mutex> lk(mtx_);
    params_ = p;
    haveParams_ = true;
}

bool SshConn::hasParams() {
    std::lock_guard<std::mutex> lk(mtx_);
    return haveParams_;
}

bool SshConn::connect(std::string& err) {
    enabled_ = true;
    HANDLE h = procForKill_.load();
    if (h) TerminateProcess(h, 1);
    std::lock_guard<std::mutex> lk(mtx_);
    stopLocked();
    return startLocked(err);
}

void SshConn::disconnect() {
    // Kill first (without the lock) so a reader blocked in ReadFile wakes up
    // and releases the lock.
    enabled_ = false;
    HANDLE h = procForKill_.load();
    if (h) TerminateProcess(h, 1);
    std::lock_guard<std::mutex> lk(mtx_);
    stopLocked();
}

bool SshConn::isConnected() {
    std::lock_guard<std::mutex> lk(mtx_);
    return aliveLocked();
}

bool SshConn::aliveLocked() {
    return proc_ && !broken_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
}

std::string SshConn::lastStderr() {
    std::lock_guard<std::mutex> lk(errMtx_);
    return errText_;
}

void SshConn::stopLocked() {
    if (inW_) {
        // Polite shutdown if still alive.
        if (proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT && !broken_) {
            DWORD w;
            WriteFile(inW_, "Q\n", 2, &w, nullptr);
        }
        CloseHandle(inW_);
        inW_ = nullptr;
    }
    if (proc_) {
        if (WaitForSingleObject(proc_, 300) == WAIT_TIMEOUT) TerminateProcess(proc_, 1);
        WaitForSingleObject(proc_, 2000);
    }
    if (outR_) { CloseHandle(outR_); outR_ = nullptr; }
    if (errThread_.joinable()) errThread_.join();
    if (errR_) { CloseHandle(errR_); errR_ = nullptr; }
    procForKill_.store(nullptr);
    if (proc_) { CloseHandle(proc_); proc_ = nullptr; }
    broken_ = true;
    rpos_ = rlen_ = 0;
}

static std::wstring BuildEnvBlock(const std::vector<std::wstring>& extra) {
    // Copy current environment, dropping variables we override.
    std::wstring block;
    wchar_t* env = GetEnvironmentStringsW();
    for (wchar_t* p = env; p && *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        bool skip = false;
        for (auto& e : extra) {
            size_t eq = e.find(L'=');
            std::wstring key = e.substr(0, eq + 1);
            if (kv.size() >= key.size() && _wcsnicmp(kv.c_str(), key.c_str(), key.size()) == 0) skip = true;
        }
        if (!skip) { block += kv; block.push_back(L'\0'); }
    }
    if (env) FreeEnvironmentStringsW(env);
    for (auto& e : extra) { block += e; block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

// Tries the remote shells in order: POSIX sh first, then PowerShell (Windows
// servers).  params_.backend = kBackendPowerShell tries PowerShell first
// (remembered per server by the UI).
bool SshConn::startLocked(std::string& err) {
    if (!haveParams_) { err = "No connection parameters"; return false; }
    if (params_.sshExe.empty()) {
        err = "ssh.exe was not found. Enable the Windows optional feature \"OpenSSH Client\" "
              "(Settings > System > Optional features).";
        return false;
    }
    int order[2] = {kBackendSh, kBackendPowerShell};
    if (params_.backend == kBackendPowerShell) std::swap(order[0], order[1]);
    std::string errs[2];
    for (int i = 0; i < 2; ++i) {
        if (launchLocked(order[i], errs[i])) {
            activeBackend_ = order[i];
            return true;
        }
        if (IsFatalSshError(errs[i])) { err = errs[i]; return false; }
        if (i == 0)
            AppLog(std::string(order[i] == kBackendSh ? "sh" : "PowerShell") + " not usable on the server, trying " +
                   (order[i] == kBackendSh ? "PowerShell" : "sh"));
    }
    err = "The server offers neither a POSIX shell (sh) nor PowerShell for this connection.\n\n"
          "sh: " + errs[order[0] == kBackendSh ? 0 : 1] + "\n\nPowerShell: " + errs[order[0] == kBackendSh ? 1 : 0];
    return false;
}

bool SshConn::launchLocked(int backend, std::string& err) {
    {
        std::lock_guard<std::mutex> lk(errMtx_);
        errText_.clear();
    }

    std::wstring cmd = QuoteArg(params_.sshExe);
    cmd += L" -T -x -a -e none";
    cmd += L" -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=3";
    cmd += L" -o StrictHostKeyChecking=accept-new -o NumberOfPasswordPrompts=1 -o LogLevel=ERROR";
    if (params_.password.empty()) cmd += L" -o BatchMode=yes";
    if (!params_.port.empty()) cmd += L" -p " + QuoteArg(params_.port);
    if (!params_.keyFile.empty()) cmd += L" -i " + QuoteArg(params_.keyFile);
    cmd += L" -- " + QuoteArg(params_.dest);
    std::string initial;
    if (backend == kBackendPowerShell) {
        cmd += L" powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + PsEncode(kPsBootstrap);
        initial = std::string(kPsScript) + "\n#SWP-END#\n";
    } else {
        cmd += L" sh";
        initial = kRemoteScript;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 1 << 16) || !CreatePipe(&outR, &outW, &sa, 1 << 20) ||
        !CreatePipe(&errR, &errW, &sa, 1 << 14)) {
        err = "CreatePipe failed";
        for (HANDLE h : {inR, inW, outR, outW, errR, errW}) if (h) CloseHandle(h);
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    std::vector<std::wstring> extra;
    if (!params_.password.empty()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        extra.push_back(std::wstring(L"SSH_ASKPASS=") + self);
        extra.push_back(L"SSH_ASKPASS_REQUIRE=force");
        extra.push_back(L"DISPLAY=localhost:0");
        extra.push_back(L"SSHWINPLAYER_ASKPASS=1");
        extra.push_back(L"SSHWINPLAYER_PW=" + params_.password);
    }
    std::wstring envBlock = BuildEnvBlock(extra);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    std::wstring cmdMutable = cmd;
    BOOL ok = CreateProcessW(nullptr, &cmdMutable[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             (void*)envBlock.c_str(), nullptr, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        err = "Could not start ssh.exe (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(inW); CloseHandle(outR); CloseHandle(errR);
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    procForKill_.store(proc_);
    inW_ = inW;
    outR_ = outR;
    errR_ = errR;
    broken_ = false;
    rpos_ = rlen_ = 0;

    HANDLE errHandle = errR_;
    errThread_ = std::thread([this, errHandle]() {
        char b[1024];
        DWORD n;
        std::string pending;
        while (ReadFile(errHandle, b, sizeof(b), &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lk(errMtx_);
            errText_.append(b, n);
            if (errText_.size() > 16384) errText_.erase(0, errText_.size() - 16384);
            pending.append(b, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                AppLog("ssh: " + pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
        }
    });

    std::string logCmd = WideToUtf8(cmd);
    if (logCmd.size() > 400) logCmd = logCmd.substr(0, 400) + "...";
    AppLog("starting: " + logCmd);
    if (!writeAll(initial)) {
        err = "Could not talk to ssh.exe";
    } else {
        // Skip anything a noisy shell rc file may print before our marker.
        std::string line;
        for (int i = 0; i < 1000; ++i) {
            if (!readLine(line)) break;
            if (line == "__SWP_READY__") return true;
        }
        err = "Connection failed.";
    }
    // Failure: collect ssh's error text.
    if (proc_) WaitForSingleObject(proc_, 3000);
    stopLocked();
    std::string e = lastStderr();
    while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
    if (!e.empty()) err += "\n\n" + e;
    else if (params_.password.empty())
        err += "\n\nNo password was given, so only key / ssh-agent authentication was tried.";
    return false;
}

bool SshConn::writeAll(const std::string& s) {
    const char* p = s.data();
    size_t left = s.size();
    while (left) {
        DWORD w = 0;
        if (!WriteFile(inW_, p, (DWORD)left, &w, nullptr) || w == 0) { broken_ = true; return false; }
        p += w;
        left -= w;
    }
    return true;
}

bool SshConn::readLine(std::string& line) {
    line.clear();
    for (;;) {
        if (rpos_ == rlen_) {
            DWORD n = 0;
            if (!ReadFile(outR_, rbuf_, sizeof(rbuf_), &n, nullptr) || n == 0) { broken_ = true; return false; }
            rpos_ = 0;
            rlen_ = n;
        }
        char* start = rbuf_ + rpos_;
        char* nl = (char*)memchr(start, '\n', rlen_ - rpos_);
        if (nl) {
            line.append(start, nl - start);
            rpos_ += (nl - start) + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.append(start, rlen_ - rpos_);
        rpos_ = rlen_;
        if (line.size() > (1 << 20)) { broken_ = true; return false; }
    }
}

bool SshConn::readExact(char* dst, size_t n) {
    while (n) {
        if (rpos_ < rlen_) {
            size_t k = std::min(n, rlen_ - rpos_);
            memcpy(dst, rbuf_ + rpos_, k);
            rpos_ += k; dst += k; n -= k;
            continue;
        }
        DWORD got = 0;
        // Large reads go straight into the destination.
        if (n >= sizeof(rbuf_)) {
            if (!ReadFile(outR_, dst, (DWORD)std::min<size_t>(n, 1 << 24), &got, nullptr) || got == 0) {
                broken_ = true;
                return false;
            }
            dst += got; n -= got;
        } else {
            if (!ReadFile(outR_, rbuf_, sizeof(rbuf_), &got, nullptr) || got == 0) { broken_ = true; return false; }
            rpos_ = 0; rlen_ = got;
        }
    }
    return true;
}

// f() returns: 1 = success, 0 = remote reported an error, -1 = I/O failure.
template <class F>
bool SshConn::withRetry(F&& f, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!aliveLocked()) {
            stopLocked();
            if (!enabled_) { err = "Not connected"; return false; }
            if (!startLocked(err)) return false;
        }
        int r = f();
        if (r == 1) return true;
        if (r == 0) return false;
        AppLog("connection lost, reconnecting");
        stopLocked();
    }
    if (err.empty()) err = "Connection lost";
    return false;
}

// Parses "<size>:<type>:<name>" from the L command.
static bool ParseEntry(const std::string& l, RemoteEntry& e) {
    size_t a = l.find(':');
    size_t b = a == std::string::npos ? a : l.find(':', a + 1);
    if (b == std::string::npos || b + 1 >= l.size()) return false;
    std::string size = l.substr(0, a), type = l.substr(a + 1, b - a - 1);
    e.name = l.substr(b + 1);
    if (e.name.size() > 2 && e.name.compare(0, 2, "./") == 0) e.name.erase(0, 2);
    e.isDir = !type.empty() && (type[0] == 'd' || type[0] == 'D');
    e.size = (!size.empty() && size[0] != '-') ? _strtoi64(size.c_str(), nullptr, 10) : -1;
    return !e.name.empty();
}

static bool ValidPath(const std::string& p) { return p.find('\n') == std::string::npos && !p.empty(); }

bool SshConn::readTextFile(const std::string& path, std::string& out, bool& exists, std::string& err) {
    if (!ValidPath(path)) { err = "Invalid path"; return false; }
    exists = false;
    out.clear();
    return withRetry([&]() -> int {
        out.clear();
        std::string l;
        if (!writeAll("S\n" + path + "\n") || !readLine(l)) return -1;
        if (l == "ERR") { exists = false; return 1; }  // no such file: not an error
        if (l.compare(0, 3, "OK ") != 0) return -1;
        int64_t size = _strtoi64(l.c_str() + 3, nullptr, 10);
        if (size < 0 || size > 64 * 1024 * 1024) { err = "File too large"; return 0; }
        const int64_t chunk = 1024 * 1024;
        std::vector<char> buf((size_t)chunk);
        for (int64_t off = 0; off < size; off += chunk) {
            char req[64];
            snprintf(req, sizeof(req), "R\n%lld %lld\n", (long long)off, (long long)chunk);
            if (!writeAll(req + path + "\n") || !readLine(l)) return -1;
            if (l == "ERR") { err = "Cannot read " + path; return 0; }
            if (l.compare(0, 3, "OK ") != 0) return -1;
            int64_t n = _strtoi64(l.c_str() + 3, nullptr, 10);
            if (n < 0 || n > chunk) return -1;
            if (n && !readExact(buf.data(), (size_t)n)) return -1;
            out.append(buf.data(), (size_t)n);
            if (n < chunk) break;
        }
        exists = true;
        return 1;
    }, err);
}

bool SshConn::writeTextFile(const std::string& path, const std::string& content, std::string& err) {
    if (!ValidPath(path)) { err = "Invalid path"; return false; }
    // Content travels as lines ending with a "//END//" line.
    std::string body;
    size_t start = 0;
    while (start < content.size()) {
        size_t nl = content.find('\n', start);
        std::string line = content.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "//END//") { err = "Content contains the end marker"; return false; }
        body += line + "\n";
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return withRetry([&]() -> int {
        std::string l;
        if (!writeAll("X\n" + path + "\n" + body + "//END//\n") || !readLine(l)) return -1;
        if (l == "OK") return 1;
        if (l == "ERR") { err = "Cannot write " + path; return 0; }
        return -1;
    }, err);
}

bool SshConn::systemName(std::string& os, bool& hasUname, std::string& err) {
    hasUname = false;
    os.clear();
    return withRetry([&]() -> int {
        std::string l;
        if (!writeAll("U\n") || !readLine(l)) return -1;
        if (l == "ERR") return 1;
        if (l.compare(0, 2, "OK") != 0) return -1;
        hasUname = true;
        os = l.size() > 3 ? l.substr(3) : "";
        return 1;
    }, err);
}

bool SshConn::windowsProfile(std::string& path, bool& found, std::string& err) {
    found = false;
    return withRetry([&]() -> int {
        std::string l;
        if (!writeAll("W\n") || !readLine(l)) return -1;
        if (l == "ERR") return 1;
        if (l != "OK" || !readLine(path)) return -1;
        found = !path.empty();
        return 1;
    }, err);
}

bool SshConn::home(std::string& path, std::string& err) {
    return withRetry([&]() -> int {
        std::string l;
        if (!writeAll("P\n") || !readLine(l)) return -1;
        if (l != "OK") return -1;
        if (!readLine(path)) return -1;
        return 1;
    }, err);
}

bool SshConn::list(const std::string& path, std::string& realPath, std::vector<RemoteEntry>& out,
                   std::string& err) {
    if (!ValidPath(path)) { err = "Invalid path"; return false; }
    return withRetry([&]() -> int {
        out.clear();
        std::string l;
        if (!writeAll("L\n" + path + "\n") || !readLine(l)) return -1;
        if (l == "ERR") { err = "Cannot open folder " + path; return 0; }
        if (l != "OK") return -1;
        if (!readLine(realPath)) return -1;
        for (;;) {
            if (!readLine(l)) return -1;
            if (l == "//END//") break;
            if (l.empty()) continue;
            RemoteEntry e;
            if (!ParseEntry(l, e)) continue;
            if (e.name == "." || e.name == "..") continue;
            out.push_back(std::move(e));
        }
        return 1;
    }, err);
}

bool SshConn::size(const std::string& path, int64_t& sz, std::string& err) {
    if (!ValidPath(path)) { err = "Invalid path"; return false; }
    return withRetry([&]() -> int {
        std::string l;
        if (!writeAll("S\n" + path + "\n") || !readLine(l)) return -1;
        if (l == "ERR") { err = "Cannot read " + path; return 0; }
        if (l.compare(0, 3, "OK ") != 0) return -1;
        sz = _strtoi64(l.c_str() + 3, nullptr, 10);
        return 1;
    }, err);
}

int64_t SshConn::read(const std::string& path, int64_t off, int64_t len, char* buf) {
    if (!ValidPath(path) || off % kBlock) return -1;
    int64_t result = -1;
    std::string err;
    char req[64];
    snprintf(req, sizeof(req), "R\n%lld %lld\n", (long long)off, (long long)len);
    bool ok = withRetry([&]() -> int {
        std::string l;
        if (!writeAll(req + path + "\n") || !readLine(l)) return -1;
        if (l == "ERR") return 0;
        if (l.compare(0, 3, "OK ") != 0) return -1;
        int64_t n = _strtoi64(l.c_str() + 3, nullptr, 10);
        if (n < 0 || n > len) return -1;
        if (n && !readExact(buf, (size_t)n)) return -1;
        result = n;
        return 1;
    }, err);
    return ok ? result : -1;
}

int SshConn::readPipelined(const std::string& path, int64_t len, int depth, int maxRequests,
                           const std::function<int64_t()>& next,
                           const std::function<void(int64_t, const char*, int64_t)>& done) {
    if (!ValidPath(path) || len <= 0 || len % kBlock) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!aliveLocked()) {
        stopLocked();
        if (!enabled_) return -1;
        std::string err;
        if (!startLocked(err)) return -1;
    }
    std::vector<char> buf((size_t)len);
    std::deque<int64_t> queue;
    int sent = 0, result = 1;
    auto send = [&](int64_t off) {
        char req[64];
        snprintf(req, sizeof(req), "R\n%lld %lld\n", (long long)off, (long long)len);
        return writeAll(req + path + "\n");
    };
    auto fill = [&]() -> bool {
        while ((int)queue.size() < depth && sent < maxRequests) {
            int64_t off = next();
            if (off < 0 || off % kBlock) break;
            if (!send(off)) return false;
            queue.push_back(off);
            ++sent;
        }
        return true;
    };
    if (!fill()) { stopLocked(); return -1; }
    while (!queue.empty()) {
        int64_t off = queue.front();
        queue.pop_front();
        std::string l;
        if (!readLine(l)) { stopLocked(); return -1; }
        if (l == "ERR") {
            result = 0;
            done(off, nullptr, -1);
        } else {
            if (l.compare(0, 3, "OK ") != 0) { stopLocked(); return -1; }
            int64_t n = _strtoi64(l.c_str() + 3, nullptr, 10);
            if (n < 0 || n > len) { stopLocked(); return -1; }
            if (n && !readExact(buf.data(), (size_t)n)) { stopLocked(); return -1; }
            done(off, buf.data(), n);
        }
        if (result == 1 && !fill()) { stopLocked(); return -1; }
    }
    return result;
}
