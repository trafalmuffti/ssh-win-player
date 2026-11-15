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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SshParams {
    std::wstring sshExe;    // full path to ssh.exe
    std::wstring dest;      // [user@]host or ~/.ssh/config alias
    std::wstring port;      // optional
    std::wstring keyFile;   // optional identity file
    std::wstring password;  // optional (password or key passphrase)
};

struct RemoteEntry {
    std::string name;  // UTF-8 (raw bytes from the server)
    bool isDir = false;
};

class SshConn {
public:
    static constexpr int64_t kBlock = 65536;  // read alignment

    SshConn() = default;
    ~SshConn();
    SshConn(const SshConn&) = delete;
    SshConn& operator=(const SshConn&) = delete;

    void setParams(const SshParams& p);
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
    bool list(const std::string& path, std::string& realPath,
              std::vector<RemoteEntry>& out, std::string& err);
    bool size(const std::string& path, int64_t& sz, std::string& err);
    // off must be a multiple of kBlock.  Returns bytes read (0 = EOF), or -1.
    int64_t read(const std::string& path, int64_t off, int64_t len, char* buf);

    std::string lastStderr();

private:
    bool startLocked(std::string& err);
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
