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
//   L \n path         -> OK \n <real path> \n names... \n //END//   | ERR
//   S \n path         -> OK <size>                                  | ERR
//   R \n off len \n p -> OK <n> \n <n raw bytes>                    | ERR
// "//END//" can never be a file name because names cannot contain '/'.
static const char* kRemoteScript =
    "sz(){ stat -c %s -- \"$1\" 2>/dev/null || stat -f %z -- \"$1\" 2>/dev/null || wc -c <\"$1\" 2>/dev/null; }\n"
    "LC_ALL=C; export LC_ALL\n"
    "echo __SWP_READY__\n"
    "while IFS= read -r c; do\n"
    "case \"$c\" in\n"
    "P) cd 2>/dev/null; printf 'OK\\n%s\\n' \"$(pwd -P)\";;\n"
    "L) IFS= read -r p; if cd \"$p\" 2>/dev/null; then printf 'OK\\n%s\\n' \"$(pwd -P)\";"
    " ls -1ApL </dev/null 2>/dev/null; echo '//END//'; else echo ERR; fi;;\n"
    "S) IFS= read -r p; s=''; [ -f \"$p\" ] && [ -r \"$p\" ] && s=$(sz \"$p\" </dev/null);"
    " if [ -n \"$s\" ]; then echo \"OK $((s+0))\"; else echo ERR; fi;;\n"
    "R) read -r o l; IFS= read -r p; s=''; [ -f \"$p\" ] && [ -r \"$p\" ] && s=$(sz \"$p\" </dev/null);"
    " if [ -n \"$s\" ]; then n=$((s-o)); [ $n -gt $l ] && n=$l; [ $n -lt 0 ] && n=0; echo \"OK $n\";"
    " if [ $n -gt 0 ]; then dd if=\"$p\" bs=65536 skip=$((o/65536)) count=$(((n+65535)/65536)) 2>/dev/null </dev/null; fi;"
    " else echo ERR; fi;;\n"
    "Q) exit 0;;\n"
    "*) echo ERR;;\n"
    "esac\n"
    "done\n";

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

bool SshConn::startLocked(std::string& err) {
    if (!haveParams_) { err = "No connection parameters"; return false; }
    if (params_.sshExe.empty()) {
        err = "ssh.exe was not found. Enable the Windows optional feature \"OpenSSH Client\" "
              "(Settings > System > Optional features).";
        return false;
    }
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
    cmd += L" -- " + QuoteArg(params_.dest) + L" sh";

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

    AppLog("starting: " + WideToUtf8(cmd));
    if (!writeAll(kRemoteScript)) {
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

static bool ValidPath(const std::string& p) { return p.find('\n') == std::string::npos && !p.empty(); }

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
            if (l.back() == '/') { e.isDir = true; l.pop_back(); }
            e.name = l;
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
