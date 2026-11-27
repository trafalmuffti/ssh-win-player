// Shared resume positions: the XML file format and merge rules.
// Plain C++ (no Windows headers) so it can be unit-tested on any platform.
//
// File (one <file> element per line, newest first):
//   <?xml version="1.0" encoding="UTF-8"?>
//   <positions version="1">
//     <file path="/srv/movies/a.mkv" pos="754.120" duration="5400.000" updated="1763630000" sid="3" aid="no"/>
//   </positions>
// pos / duration in seconds, updated = unix time of the save.
// sid / aid are optional: the mpv subtitle / audio track ids selected when the
// position was saved, or "no" when the track was explicitly off.  Absent means
// nothing was recorded (older file) and the player's own choice stands.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

static const double kResumeMinPos = 5.0;       // stopped earlier than this: nothing to resume
static const size_t kResumeMaxEntries = 1000;  // most recently updated files kept

// Track selection stored in a ResumeEntry (mpv track ids).
static const int kResumeTrackUnset = -2;  // nothing recorded: leave the player's choice alone
static const int kResumeTrackOff = -1;    // explicitly off ("no")

static void AppendUtf8(std::string& r, unsigned long cp) {
    if (cp < 0x80) r += (char)cp;
    else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        r += (char)(0xF0 | (cp >> 18)); r += (char)(0x80 | ((cp >> 12) & 0x3F));
        r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F));
    }
}

struct ResumeEntry {
    double pos = 0, duration = 0;
    int sid = kResumeTrackUnset, aid = kResumeTrackUnset;  // selected subtitle / audio track
    int64_t updated = 0;   // unix time of the save
    bool removed = false;  // local tombstone (finished / restarted), not written
    bool pending = false;  // local change not yet written to the server
};
static std::string XmlEscape(const std::string& s) {
    std::string r;
    for (char ch : s) {
        switch (ch) {
        case '&': r += "&amp;"; break;
        case '<': r += "&lt;"; break;
        case '>': r += "&gt;"; break;
        case '"': r += "&quot;"; break;
        case '\'': r += "&apos;"; break;
        default: r += ch;
        }
    }
    return r;
}

static std::string XmlUnescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { r += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) { r += s[i]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") r += '&';
        else if (ent == "lt") r += '<';
        else if (ent == "gt") r += '>';
        else if (ent == "quot") r += '"';
        else if (ent == "apos") r += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                                  : strtoul(ent.c_str() + 1, nullptr, 10);
            AppendUtf8(r, cp);
        } else { r += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return r;
}

static void ParseResumeXml(const std::string& text, std::map<std::string, ResumeEntry>& out) {
    size_t at = 0;
    while ((at = text.find("<file ", at)) != std::string::npos) {
        size_t end = text.find("/>", at);
        if (end == std::string::npos) break;
        std::string tag = text.substr(at, end - at);
        auto attr = [&](const char* name) -> std::string {
            std::string key = std::string(" ") + name + "=\"";
            size_t a = tag.find(key);
            if (a == std::string::npos) return "";
            a += key.size();
            size_t b = tag.find('"', a);
            return b == std::string::npos ? "" : XmlUnescape(tag.substr(a, b - a));
        };
        std::string path = attr("path");
        if (!path.empty()) {
            auto track = [&](const char* name) -> int {
                std::string v = attr(name);
                if (v.empty()) return kResumeTrackUnset;
                if (v == "no") return kResumeTrackOff;
                return (int)strtol(v.c_str(), nullptr, 10);
            };
            ResumeEntry e;
            e.pos = strtod(attr("pos").c_str(), nullptr);
            e.duration = strtod(attr("duration").c_str(), nullptr);
            e.sid = track("sid");
            e.aid = track("aid");
            e.updated = strtoll(attr("updated").c_str(), nullptr, 10);
            out[path] = e;
        }
        at = end + 2;
    }
}

static std::string BuildResumeXml(const std::map<std::string, ResumeEntry>& m) {
    std::vector<std::pair<std::string, ResumeEntry>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.updated > b.second.updated; });
    if (v.size() > kResumeMaxEntries) v.resize(kResumeMaxEntries);
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!-- SSH Win Player: saved playback positions (seconds), shared by all devices. -->\n"
                    "<positions version=\"1\">\n";
    char num[128];
    auto trackAttr = [](const char* name, int v) -> std::string {
        if (v == kResumeTrackUnset) return "";
        return std::string(" ") + name + "=\"" + (v == kResumeTrackOff ? "no" : std::to_string(v)) + "\"";
    };
    for (auto& kv : v) {
        snprintf(num, sizeof(num), "\" pos=\"%.3f\" duration=\"%.3f\" updated=\"%lld\"", kv.second.pos,
                 kv.second.duration, (long long)kv.second.updated);
        x += "  <file path=\"" + XmlEscape(kv.first) + num + trackAttr("sid", kv.second.sid) +
             trackAttr("aid", kv.second.aid) + "/>\n";
    }
    x += "</positions>\n";
    return x;
}

static bool ResumeNearEnd(double pos, double dur) { return dur > 0 && (dur - pos < 15.0 || pos > dur * 0.95); }

// Forget a position when there is nothing worth resuming: barely started or
// (nearly) finished.
static bool ResumeShouldForget(double pos, double dur) { return pos < kResumeMinPos || ResumeNearEnd(pos, dur); }

// Applies one device's save to the map read from the server.  A newer save of
// the same file by another device wins.
static void ApplyResumeChange(std::map<std::string, ResumeEntry>& m, const std::string& path, double pos,
                              double dur, int sid, int aid, bool remove, int64_t updated) {
    auto it = m.find(path);
    if (it != m.end() && it->second.updated > updated) return;
    if (remove) { m.erase(path); return; }
    ResumeEntry e;
    e.pos = pos;
    e.duration = dur;
    e.sid = sid;
    e.aid = aid;
    e.updated = updated;
    m[path] = e;
}

// Server state plus this device's saves that are not written yet (newest wins).
static std::map<std::string, ResumeEntry> MergePendingResume(std::map<std::string, ResumeEntry> server,
                                                             const std::map<std::string, ResumeEntry>& local) {
    for (auto& kv : local) {
        if (!kv.second.pending) continue;
        auto s = server.find(kv.first);
        if (s == server.end() || s->second.updated <= kv.second.updated) server[kv.first] = kv.second;
    }
    return server;
}

// Where to start a file (0 = from the beginning).
static double ResumeStartFor(const ResumeEntry& e) {
    if (e.removed) return 0;
    if (ResumeShouldForget(e.pos, e.duration)) return 0;
    return e.pos;
}
static double ResumeStartFor(const std::map<std::string, ResumeEntry>& m, const std::string& path) {
    auto it = m.find(path);
    if (it == m.end()) return 0;
    return ResumeStartFor(it->second);
}
