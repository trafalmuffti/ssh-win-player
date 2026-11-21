// A persistent connection to a remote host through the OpenSSH client
// (ssh.exe, bundled with Windows 10 1809+ / Windows 11).
//
// One ssh.exe process is started per connection running a tiny POSIX sh
// command loop on the server.  Requests are line based; file data is read
// in aligned blocks with dd, so seeking inside a remote file costs a single
// round trip and no extra software is needed on the server.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Remote command loop implementations.
enum { kBackendSh = 1, kBackendPowerShell = 2 };

struct SshParams {
    std::wstring sshExe;    // full path to ssh.exe
    std::wstring dest;      // [user@]host or ~/.ssh/config alias
    std::wstring port;      // optional
    std::wstring keyFile;   // optional identity file
    std::wstring password;  // optional (password or key passphrase)
    int backend = 0;        // remote shell to try first (kBackendSh / kBackendPowerShell)
};

struct RemoteEntry {
    std::string name;  // UTF-8 (raw bytes from the server)
    bool isDir = false;
    int64_t size = -1;  // bytes, -1 = unknown
};

class SshConn {
public:
    static constexpr int64_t kBlock = 65536;  // read alignment

    SshConn() = default;
    ~SshConn();
    SshConn(const SshConn&) = delete;
    SshConn& operator=(const SshConn&) = delete;

    void setParams(const SshParams& p);
    // Called (from any thread) when an established connection broke and could
    // not be restored automatically.  Not called for a failed first connect()
    // or after disconnect().
    void setLostCallback(std::function<void(const std::string&)> cb) { onLost_ = std::move(cb); }
    // Starts (or restarts) the ssh process.  Thread safe.
    bool connect(std::string& err);
    // Kills the ssh process.  Safe to call while another thread is blocked
    // in a request: that request then fails.
    void disconnect();
    bool isConnected();
    bool hasParams();

    // Requests.  Each one is atomic (internally locked) and transparently
    // reconnects once if the connection was lost.
    bool home(std::string& path, std::string& err);
    // Runs `uname -s` if the server has uname (=> probably Unix).
    bool systemName(std::string& os, bool& hasUname, std::string& err);
    // Small text files (the shared resume-position file).  readTextFile sets
    // exists = false (and returns true) when the file does not exist.
    bool readTextFile(const std::string& path, std::string& out, bool& exists, std::string& err);
    bool writeTextFile(const std::string& path, const std::string& content, std::string& err);
    // Windows servers (Git Bash / MSYS2 / Cygwin shells): %USERPROFILE% as a
    // POSIX path via cygpath.  found = false when not available.
    bool windowsProfile(std::string& path, bool& found, std::string& err);
    bool list(const std::string& path, std::string& realPath,
              std::vector<RemoteEntry>& out, std::string& err);
    bool size(const std::string& path, int64_t& sz, std::string& err);
    // off must be a multiple of kBlock.  Returns bytes read (0 = EOF), or -1.
    int64_t read(const std::string& path, int64_t off, int64_t len, char* buf);

    // Pipelined block reads: keeps up to `depth` requests of `len` bytes in
    // flight so a distant server is not limited to one block per round trip.
    // next() returns the next offset to request (a multiple of kBlock) or -1;
    // done(off, data, n) is called for each answer in request order (n = -1 if
    // the server reported an error for that block).  At most maxRequests are
    // sent per call so other streams get a turn on the connection.
    // Returns 1 on success, 0 if a block failed on the server, -1 on I/O error.
    int readPipelined(const std::string& path, int64_t len, int depth, int maxRequests,
                      const std::function<int64_t()>& next,
                      const std::function<void(int64_t, const char*, int64_t)>& done);
    bool isEnabled() const { return enabled_; }
    // Which remote shell the current connection uses (kBackendSh / kBackendPowerShell).
    int activeBackend() { std::lock_guard<std::mutex> lk(mtx_); return activeBackend_; }

    std::string lastStderr();

private:
    bool startLocked(std::string& err);
    bool launchLocked(int backend, std::string& err);
    void stopLocked();
    bool aliveLocked();
    bool writeAll(const std::string& s);
    bool readLine(std::string& line);
    bool readExact(char* dst, size_t n);
    template <class F> bool withRetry(F&& f, std::string& err);

    std::mutex mtx_;
    SshParams params_;
    bool haveParams_ = false;
    std::atomic<bool> enabled_{false};
    int activeBackend_ = 0;
    std::function<void(const std::string&)> onLost_;
    bool everConnected_ = false;      // a connection worked since the last connect()/disconnect()
    std::atomic<bool> armed_{false};  // ssh exiting now would be unexpected
    void notifyLostLocked(const std::string& err);
    HANDLE proc_ = nullptr;
    std::atomic<HANDLE> procForKill_{nullptr};
    HANDLE inW_ = nullptr;   // our end of ssh stdin
    HANDLE outR_ = nullptr;  // our end of ssh stdout
    HANDLE errR_ = nullptr;  // our end of ssh stderr
    std::thread errThread_;
    std::mutex errMtx_;
    std::string errText_;
    bool broken_ = true;
    char rbuf_[65536];
    size_t rpos_ = 0, rlen_ = 0;
};

// Helpers shared with the UI.
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::wstring FindSshExe();
void AppLog(const std::string& line);
