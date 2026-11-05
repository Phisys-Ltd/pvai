// Peers against a real daemon: the offline/seenAt fields through peerList and
// `pvai peers`, and the warning on `proposed` for a target the app's list
// says is offline or lacks the session.
//
// usage: pvai-peers-test /path/to/pvai
#include "support.h"

using namespace pvai;
using namespace pvai::test;

namespace {

std::string g_pvai;

RunResult run(const std::string &args, const std::string &env = "", const std::string &input = "")
{
    return runPvai(g_pvai, args, env, input);
}

// Asks for the peer list over the socket, as `pvai peers` does.
json peerList()
{
    LineClient c;
    c.send({{"type", "hello"}, {"client", "pvai"}, {"version", "0"}});
    c.next("welcome");
    c.send({{"type", "listPeers"}});
    return c.next("peerList");
}

std::string warningOf(const std::string &out)
{
    if (out.find("warning: ") == std::string::npos)
        return "";
    return out.find("is offline") != std::string::npos ? "host-offline"
           : out.find("lists no session") != std::string::npos ? "unknown-session" : "?";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    g_pvai = std::filesystem::absolute(argv[1]).string();
    const TestHome home;
    ::setenv("PVAI_CODEX", "", 1);
    ::unsetenv(kEnvId);
    const int appPort = 20000 + static_cast<int>(::getpid() % 20000);

    DaemonProcess daemon;
    daemon.start(g_pvai, appPort);

    // No list yet: no warning, whatever the target.
    RunResult r = run("propose M4/wgone text");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning before any list: " + r.out);

    LineClient app;
    app.send({{"type", "hello"}, {"client", "test-app"}, {"version", "0"}});
    check(!app.next("welcome").empty(), "the app is welcome");
    const long long seen = static_cast<long long>(std::time(nullptr)) - 2 * 3600;
    app.send({{"type", "peers"}, {"self", "tf"},
              {"peers", {{{"host", "M4"}, {"session", "w97dde1bd"}, {"label", "Projekte"}, {"state", "idle"}},
                         {{"host", "M4"}, {"session", "w2"}, {"label", "Other"}},
                         {{"host", "mac"}, {"session", "wa"}, {"label", "A"}, {"offline", true}, {"seenAt", seen}},
                         {{"host", "mac"}, {"session", "wb"}, {"offline", true}},
                         {{"host", "bad"}, {"session", "x"}, {"offline", "yes"}, {"seenAt", "yesterday"}},
                         {{"host", "pi5"}, {"session", "t1"}, {"offline", false}},
                         {{"host", "gone"}, {"session", ""}, {"offline", true}},
                         {{"host", "whole"}, {"session", ""}}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const json list = peerList();
    json byId;
    for (const auto &p : list.value("peers", json::array()))
        byId[p.value("host", "") + "/" + p.value("session", "")] = p;
    check(byId["mac/wa"].value("offline", false) && byId["mac/wa"].value("seenAt", 0LL) == seen,
          "offline and seenAt pass through peerList: " + byId["mac/wa"].dump());
    check(byId["mac/wb"].value("offline", false) && !byId["mac/wb"].contains("seenAt"), "offline without seenAt");
    check(!byId["bad/x"].contains("offline") && !byId["bad/x"].contains("seenAt"),
          "bad types are dropped: " + byId["bad/x"].dump());
    check(byId["pi5/t1"].value("offline", true) == false, "offline false stays false");
    check(!byId["M4/w2"].contains("offline"), "entries without the fields stay as they were");

    r = run("peers");
    check(r.out.find("  mac/wa  \"A\"  (offline, last seen 2 h ago)\n") != std::string::npos
              && r.out.find("  mac/wb  (offline)\n") != std::string::npos
              && r.out.find("  M4/w97dde1bd  idle  \"Projekte\"\n") != std::string::npos
              && r.out.find("  pi5/t1\n") != std::string::npos,
          "pvai peers marks offline entries: " + r.out);

    // Warnings.
    r = run("propose mac/wa text");
    check(r.status == 0 && warningOf(r.out) == "host-offline"
              && r.out.find("warning: mac is offline (last seen 2 h ago); the message waits until the owner releases it and mac is back")
                     != std::string::npos,
          "host-offline: " + r.out);
    r = run("propose mac text");
    check(r.status == 0 && warningOf(r.out) == "host-offline", "host-offline for a bare host: " + r.out);
    r = run("propose gone/x text");
    check(r.status == 0 && warningOf(r.out) == "host-offline" && r.out.find("last seen") == std::string::npos,
          "host-offline without seenAt: " + r.out);
    r = run("propose M4/wmissing text");
    check(r.status == 0 && warningOf(r.out) == "unknown-session"
              && r.out.find("warning: M4 lists no session wmissing (it may have been restarted; see pvai peers)") != std::string::npos,
          "unknown-session: " + r.out);
    r = run("propose M4/w97dde1bd text");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning for a known id: " + r.out);
    r = run("propose M4/Projekte text");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning for a known label: " + r.out);
    r = run("propose M4 text");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning for a bare online host: " + r.out);
    // Ten proposals a minute per sender: the rest from a second one.
    r = run("propose whole/w9 text", "PVAI_ID=wsecond");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning for a session of a host listed as a whole: " + r.out);
    r = run("propose nowhere/w1 text", "PVAI_ID=wsecond");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning for an unknown host: " + r.out);
    r = run("propose tf/wnothing text", "PVAI_ID=wsecond");
    check(r.status == 0 && warningOf(r.out).empty(), "no warning for this host (peers self): " + r.out);

    daemon.stop();
    return finish();
}
