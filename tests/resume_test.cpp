// Native unit test for the shared resume-position file (src/resume_xml.h).
//   g++ -std=c++17 -I src tests/resume_test.cpp -o resume_test && ./resume_test
#include "resume_xml.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_fail; } \
    } while (0)

using Map = std::map<std::string, ResumeEntry>;
static ResumeEntry E(double pos, double dur, int64_t upd) { ResumeEntry e; e.pos = pos; e.duration = dur; e.updated = upd; return e; }

int main() {
    // 1. Round trip, including characters that need escaping and non-ASCII names.
    Map m;
    const std::string weird = "/srv/media/Tom & Jerry <\"best\"> it's/Film \xC3\xA9t\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.mkv";
    m[weird] = E(754.125, 5400.5, 1763630000);
    m["/srv/a.mp4"] = E(12.0, 60.0, 1763630100);
    std::string xml = BuildResumeXml(m);
    Map back;
    ParseResumeXml(xml, back);
    CHECK(back.size() == 2);
    CHECK(back.count(weird) == 1);
    CHECK(std::fabs(back[weird].pos - 754.125) < 1e-6 && std::fabs(back[weird].duration - 5400.5) < 1e-6);
    CHECK(back[weird].updated == 1763630000);
    CHECK(xml.find("&amp;") != std::string::npos && xml.find("&quot;") != std::string::npos);
    CHECK(xml.find(weird) == std::string::npos);  // raw (unescaped) path must not appear
    // newest first
    CHECK(xml.find("/srv/a.mp4") < xml.find("Tom"));

    // 2. Numeric character references written by other tools.
    Map ent;
    ParseResumeXml("<file path=\"/x/&#233;&#x65E5;\" pos=\"7\" duration=\"0\" updated=\"5\"/>", ent);
    CHECK(ent.count("/x/\xC3\xA9\xE6\x97\xA5") == 1);

    // 3. Two devices: different files are both kept; for the same file the newest save wins.
    Map srv;
    srv["/a"] = E(100, 1000, 100);
    ApplyResumeChange(srv, "/b", 50, 1000, kResumeTrackUnset, kResumeTrackUnset, false, 101);  // device 1 saves another file
    CHECK(srv.size() == 2);
    ApplyResumeChange(srv, "/a", 30, 1000, kResumeTrackUnset, kResumeTrackUnset, false, 90);   // device 2's older save: ignored
    CHECK(std::fabs(srv["/a"].pos - 100) < 1e-9);
    ApplyResumeChange(srv, "/a", 300, 1000, kResumeTrackUnset, kResumeTrackUnset, false, 110); // newer save: wins
    CHECK(std::fabs(srv["/a"].pos - 300) < 1e-9 && srv["/a"].updated == 110);

    // 4. Removal (finished): newer removal deletes, older removal is ignored.
    ApplyResumeChange(srv, "/b", 0, 0, kResumeTrackUnset, kResumeTrackUnset, true, 100);
    CHECK(srv.count("/b") == 1);
    ApplyResumeChange(srv, "/b", 0, 0, kResumeTrackUnset, kResumeTrackUnset, true, 120);
    CHECK(srv.count("/b") == 0);

    // 5. Local saves not yet written override older server data; written ones do not linger.
    Map server2;
    server2["/a"] = E(10, 100, 200);
    Map local;
    local["/a"] = E(40, 100, 210); local["/a"].pending = true;   // not yet written, newer
    local["/gone"] = E(20, 100, 150);                             // written earlier, since removed elsewhere
    Map merged = MergePendingResume(server2, local);
    CHECK(std::fabs(merged["/a"].pos - 40) < 1e-9);
    CHECK(merged.count("/gone") == 0);
    Map local2;
    local2["/a"] = E(5, 100, 190); local2["/a"].pending = true;  // pending but older than server
    CHECK(std::fabs(MergePendingResume(server2, local2)["/a"].pos - 10) < 1e-9);

    // 6. Where to start.
    Map s;
    s["/early"] = E(3, 1000, 1);
    s["/mid"] = E(500, 1000, 1);
    s["/end"] = E(990, 1000, 1);
    s["/pct"] = E(960, 1000, 1);  // past 95 %
    s["/removed"] = E(500, 1000, 1); s["/removed"].removed = true;
    s["/nodur"] = E(500, 0, 1);    // unknown duration: still resumable
    CHECK(ResumeStartFor(s, "/early") == 0);
    CHECK(ResumeStartFor(s, "/mid") == 500);
    CHECK(ResumeStartFor(s, "/end") == 0);
    CHECK(ResumeStartFor(s, "/pct") == 0);
    CHECK(ResumeStartFor(s, "/removed") == 0);
    CHECK(ResumeStartFor(s, "/nodur") == 500);
    CHECK(ResumeStartFor(s, "/unknown") == 0);

    // 7. Size cap keeps the most recently updated entries.
    Map big;
    for (int i = 0; i < 1200; ++i) big["/f" + std::to_string(i)] = E(100, 1000, i);
    Map capped;
    ParseResumeXml(BuildResumeXml(big), capped);
    CHECK(capped.size() == kResumeMaxEntries);
    CHECK(capped.count("/f1199") == 1 && capped.count("/f0") == 0);

    // 8. Garbage / empty input does not crash and yields nothing.
    Map g;
    ParseResumeXml("", g);
    ParseResumeXml("<positions><file path=\"/a\" pos=", g);
    ParseResumeXml("not xml at all", g);
    CHECK(g.empty());

    // 9. Track selection round-trips: an id, an explicit "no", and absent.
    Map tm;
    ResumeEntry te = E(500, 1000, 300);
    te.sid = 3;
    te.aid = kResumeTrackOff;
    tm["/movie.mkv"] = te;
    tm["/plain.mp4"] = E(500, 1000, 200);  // no tracks recorded
    std::string txml = BuildResumeXml(tm);
    CHECK(txml.find("sid=\"3\"") != std::string::npos);
    CHECK(txml.find("aid=\"no\"") != std::string::npos);
    Map tback;
    ParseResumeXml(txml, tback);
    CHECK(tback["/movie.mkv"].sid == 3 && tback["/movie.mkv"].aid == kResumeTrackOff);
    CHECK(tback["/plain.mp4"].sid == kResumeTrackUnset && tback["/plain.mp4"].aid == kResumeTrackUnset);
    // An entry written before tracks were recorded parses as unset.
    Map oldf;
    ParseResumeXml("<file path=\"/old.mkv\" pos=\"7\" duration=\"9\" updated=\"5\"/>", oldf);
    CHECK(oldf["/old.mkv"].sid == kResumeTrackUnset && oldf["/old.mkv"].aid == kResumeTrackUnset);
    // A save carries its tracks through ApplyResumeChange.
    Map am;
    ApplyResumeChange(am, "/x.mkv", 10, 100, 2, kResumeTrackOff, false, 50);
    CHECK(am["/x.mkv"].sid == 2 && am["/x.mkv"].aid == kResumeTrackOff);

    std::printf(g_fail ? "%d check(s) FAILED\n" : "all resume tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
