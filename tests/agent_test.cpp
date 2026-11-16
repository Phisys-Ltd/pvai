// Agent messages against a real daemon: `pvai propose`, the proposals an app
// sees, dismiss, the limits, and the first line a released message gets.
// Runs in its own HOME and runtime directory, with a fake wrapper as the
// target session.
//
// usage: pvai-agent-test /path/to/pvai
#include "support.h"

#include <openssl/evp.h>

#include <sys/stat.h>

#include <fstream>

using namespace pvai;
using namespace pvai::test;

namespace {

std::string g_pvai;

RunResult run(const std::string &args, const std::string &env = "", const std::string &input = "")
{
    return runPvai(g_pvai, args, env, input);
}

std::string sha256(const std::string &data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 15];
    }
    return out;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const std::string &path, const std::string &text)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// Bytes with line ends and JSON in them: a download's body is never lines.
std::string content(std::size_t size, unsigned seed)
{
    std::string data = "\n{\"type\":\"shutdown\"}\n";
    unsigned x = seed;
    while (data.size() < size) {
        x = x * 1664525u + 1013904223u;
        data += static_cast<char>(x >> 16);
    }
    data.resize(size);
    return data;
}

json hello() { return {{"type", "hello"}, {"client", "test-app"}, {"version", "0"}}; }

json listOf(LineClient &app, const std::string &id)
{
    for (int i = 0; i < 20; ++i) {
        const json list = app.next("proposals");
        if (list.empty())
            break;
        for (const auto &p : list["proposals"])
            if (p.value("id", "") == id)
                return p;
    }
    return json::object();
}

std::string proposalId(const RunResult &r) { return r.out.substr(0, r.out.find(' ')); }

std::size_t count(const json &proposals) { return proposals.empty() ? 0 : proposals["proposals"].size(); }

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

    // Without an app it still waits, and says so.
    RunResult r = run("propose taifun/w1 'hello there'", "PVAI_ID=wsender");
    check(r.status == 0 && r.out.find("no app connected now") != std::string::npos, "propose without an app: " + r.out);

    // An app sees it after welcome, with the sender's label once it is known.
    LineClient wrapper;
    wrapper.send({{"type", "register"}, {"id", "wsender"}, {"label", "sender-label"}, {"cwd", "/x"}, {"running", true}});
    LineClient app;
    app.send({{"type", "hello"}, {"client", "test-app"}, {"version", "0"}});
    check(!app.next("welcome").empty(), "the app is welcome");
    json list = app.next("proposals");
    check(count(list) == 1, "proposals follow welcome");
    if (count(list) == 1) {
        const json &p = list["proposals"][0];
        check(p.value("from", "") == "wsender" && p.value("to", "") == "taifun/w1" && p.value("text", "") == "hello there"
                  && p.value("id", "").size() == 9 && p.value("at", 0LL) > 0,
              "the proposal's fields: " + p.dump());
    }

    r = run("propose M4/w1", "PVAI_ID=wsender", "from stdin\nsecond line");
    check(r.status == 0 && r.out.find("1 app connected") != std::string::npos, "propose from stdin counts the app: " + r.out);
    list = app.next("proposals");
    check(count(list) == 2 && list["proposals"][1].value("text", "") == "from stdin\nsecond line"
              && list["proposals"][1].value("fromLabel", "") == "sender-label",
          "every app gets the new list, with the label");
    const std::string first = count(list) ? list["proposals"][0].value("id", "") : "";

    // Refused: no text, a bad target, too long.
    r = run("propose taifun/w1 '   '");
    check(r.status == 1 && r.out.find("empty") != std::string::npos, "empty text: " + r.out);
    r = run("propose 'bad host/w1' text");
    check(r.status == 1 && r.out.find("bad-target") != std::string::npos, "bad target: " + r.out);
    r = run("propose taifun/ text");
    check(r.status == 1 && r.out.find("bad-target") != std::string::npos, "empty session: " + r.out);
    // A host without a session is no target: it would land in whichever tab the app has open, the sender's own
    // among them. Refused before the daemon is asked, and by the daemon too.
    r = run("propose M4 text", "PVAI_ID=wsender");
    check(r.status == 1 && r.out.find("needs HOST/SESSION") != std::string::npos && r.out.find("pvai peers") != std::string::npos,
          "a bare host is refused with a pointer to pvai peers: " + r.out);
    r = run("propose --reply p0a1b2c3d M4 text", "PVAI_ID=wsender");
    check(r.status == 1 && r.out.find("needs HOST/SESSION") != std::string::npos, "also as a reply: " + r.out);
    {
        LineClient raw;
        raw.send({{"type", "hello"}, {"client", "pvai"}, {"version", "0"}});
        raw.send({{"type", "propose"}, {"ref", "x"}, {"to", "M4"}, {"text", "t"}, {"from", "wsender"}});
        check(raw.next("rejected").value("reason", "") == "no-session", "the daemon refuses a bare host itself: no-session");
    }
    r = run("propose taifun/w1 \"$(head -c 40000 /dev/zero | tr '\\0' x)\"");
    check(r.status == 1 && r.out.find("too-long") != std::string::npos, "too long: " + r.out);

    // A reply names the proposal it answers; a label is resolved to the one
    // session that has it, two with the same label are refused.
    const std::string self = hostName();
    r = run("propose --reply p0a1b2c3d " + self + "/w1 'the answer'", "PVAI_ID=wsender");
    check(r.status == 0 && r.out.find(" to " + self + "/w1: ") != std::string::npos, "propose --reply: " + r.out);
    list = app.next("proposals");
    check(count(list) == 3 && list["proposals"][2].value("replyTo", "") == "p0a1b2c3d" && list["proposals"][0]["replyTo"].is_null(),
          "the proposal carries replyTo: " + list.dump());
    r = run("propose --reply nonsense " + self + "/w1 text");
    check(r.status == 1 && r.out.find("bad-reply") != std::string::npos, "a bad reply id: " + r.out);
    LineClient second, third;
    second.send({{"type", "register"}, {"id", "wtwo"}, {"label", "sender-label"}, {"cwd", "/x"}, {"running", true}});
    third.send({{"type", "register"}, {"id", "wthree"}, {"label", "only-one"}, {"cwd", "/x"}, {"running", true}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    r = run("propose " + self + "/sender-label text");
    check(r.status == 1 && r.out.find("ambiguous-target") != std::string::npos, "two sessions with that label: " + r.out);
    r = run("propose " + self + "/only-one text");
    check(r.status == 0 && r.out.find(" to " + self + "/wthree: ") != std::string::npos, "one label is the session's id: " + r.out);
    list = app.next("proposals");
    check(count(list) == 4 && list["proposals"][3].value("to", "") == self + "/wthree", "and the proposal goes to the id");
    for (std::size_t i = 2; i < count(list); ++i) {
        app.send({{"type", "dismiss"}, {"ref", 70 + static_cast<int>(i)}, {"proposal", list["proposals"][i].value("id", "")}});
        app.next("dismissed");
    }
    while (!app.next().empty()) {
    }

    // Dismiss: gone from the list; an unknown one is answered alike.
    app.send({{"type", "dismiss"}, {"ref", 7}, {"proposal", first}});
    list = app.next("proposals");
    check(count(list) == 1, "dismiss removes it from the list");
    json answer = app.next("dismissed");
    check(!answer.empty() && answer.value("proposal", "") == first && answer.value("ref", 0) == 7, "dismissed answers");
    app.send({{"type", "dismiss"}, {"ref", 8}, {"proposal", "pnothere0"}});
    answer = app.next("dismissed");
    check(!answer.empty() && answer.value("proposal", "") == "pnothere0", "an unknown proposal is dismissed too");

    // Ten a minute per session, sixteen open in all.
    int ok = 0;
    for (int i = 0; i < 12; ++i)
        if (run("propose taifun/w1 text" + std::to_string(i), "PVAI_ID=wbusy").status == 0)
            ++ok;
    check(ok == 10, "ten a minute per session (" + std::to_string(ok) + ")");
    ok = 0;
    for (int i = 0; i < 8; ++i)
        if (run("propose taifun/w1 text", "PVAI_ID=wother" + std::to_string(i)).status == 0)
            ++ok;
    check(ok == 5, "sixteen open in all (" + std::to_string(ok) + " more)");
    r = run("propose taifun/w1 text", "PVAI_ID=wlast");
    check(r.out.find("too-many") != std::string::npos, "too-many when full: " + r.out);

    // Peers: before any list only this host; then the app's list, cleaned.
    r = run("peers", "PVAI_ID=wsender");
    check(r.status == 0 && r.out.find("/wsender  claude") != std::string::npos
              && r.out.find("(this session)") != std::string::npos && r.out.find("not known here") != std::string::npos,
          "peers without a list: " + r.out);
    app.send({{"type", "peers"}, {"self", "tf"},
              {"peers", {{{"host", "M4"}, {"session", "w97dde1bd"}, {"label", "Projekte"}, {"assistant", "claude"},
                          {"state", "idle"}, {"cwd", "/Users/m/p"}},
                         {{"host", "bad host"}, {"session", "x"}},
                         {{"host", "pi5"}, {"session", "t1"}, {"label", 7}}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r = run("peers");
    check(r.out.find("this host (tf):\n  tf/wsender") != std::string::npos
              && r.out.find("  M4/w97dde1bd  claude  idle  \"Projekte\"  /Users/m/p") != std::string::npos
              && r.out.find("  pi5/t1\n") != std::string::npos && r.out.find("bad host") == std::string::npos
              && r.out.find("(this session)") == std::string::npos,
          "peers with the app's list: " + r.out);

    // A released message: the first line tells who wrote and how to answer.
    while (!app.next().empty()) {
    }
    app.send({{"type", "prompt"}, {"ref", 9}, {"session", "wsender"}, {"text", "please check"}, {"from", "M4/w8bb65867"},
              {"fromLabel", "phi-claude-voice"}});
    json typed = wrapper.next("type");
    check(typed.value("text", "")
              == "[Message from M4/w8bb65867 (phi-claude-voice), released by the owner. Answer with: pvai propose "
                 "M4/w8bb65867 \"...\"]\nplease check",
          "the released prompt starts with the sender line: " + typed.dump());

    // A sender without a session part (a bare host) cannot be answered: no "Answer with" line, and the
    // session is not offered as a target.
    LineClient headerWrapper;
    headerWrapper.send({{"type", "register"}, {"id", "wheader"}, {"label", "header"}, {"cwd", "/x"}, {"running", true}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    app.send({{"type", "prompt"}, {"ref", 90}, {"session", "wheader"}, {"text", "no session"}, {"from", "M4"}, {"proposal", "p1234abce"}});
    typed = headerWrapper.next("type");
    check(typed.value("text", "")
              == "[Message p1234abce from M4, released by the owner. It cannot be answered: the sender is not a session.]\nno session",
          "a sender with no session part: no Answer with line: " + typed.dump());
    // A message from this very session to itself is refused whatever the app thinks the target is.
    app.send({{"type", "prompt"}, {"ref", 91}, {"session", "wheader"}, {"text", "to myself"}, {"from", hostName() + "/wheader"}});
    check(app.next("rejected", [](const json &m) { return m.value("ref", 0) == 91; }).value("reason", "") == "self-message",
          "a message from a session to itself is refused: self-message");

    // A plain prompt has no such line.
    LineClient hook;
    hook.send({{"type", "hook"}, {"id", "wsender"}, {"event", {{"hook_event_name", "Stop"}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.send({{"type", "prompt"}, {"ref", 10}, {"session", "wsender"}, {"text", "plain"}});
    typed = wrapper.next("type");
    check(typed.value("text", "") == "plain", "a prompt without from is typed as it is: " + typed.dump());

    // With the proposal's id and what it answers: both in the line, and the
    // way to answer names it.
    LineClient hook2;
    hook2.send({{"type", "hook"}, {"id", "wsender"}, {"event", {{"hook_event_name", "Stop"}, {"session_id", "x"}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.send({{"type", "prompt"}, {"ref", 11}, {"session", "wsender"}, {"text", "done"}, {"from", "M4/w8bb65867"},
              {"proposal", "p1234abcd"}, {"replyTo", "p0a1b2c3d"}});
    typed = wrapper.next("type");
    check(typed.is_object() && typed.value("text", "")
              == "[Message p1234abcd from M4/w8bb65867, answering p0a1b2c3d, released by the owner. Answer with: pvai propose "
                 "--reply p1234abcd M4/w8bb65867 \"...\"]\ndone",
          "the line with the proposal's id and what it answers: " + typed.dump());

    // Stopping the daemon: not while a prompt waits (they live in memory),
    // unless forced, and then the apps hear which went.
    app.send({{"type", "prompt"}, {"ref", 12}, {"session", "wsender"}, {"text", "waits"}});
    json accepted = app.next("accepted");
    while (!accepted.empty() && accepted.value("ref", 0) != 12)
        accepted = app.next("accepted");
    check(!accepted.empty() && accepted.value("delivery", "") == "queued", "a prompt waits behind the running turn");
    r = run("daemon stop");
    check(r.status == 1 && r.out.find("not stopped: prompts are waiting") != std::string::npos
              && r.out.find("wsender \"sender-label\": 1 queued") != std::string::npos,
          "daemon stop is refused while one waits: " + r.out);
    r = run("daemon stop --force");
    check(r.status == 0 && r.out.find("dropped: wsender ref 12: waits") != std::string::npos && r.out.find("daemon stopped") != std::string::npos,
          "--force stops it and names what went: " + r.out);
    const json dropped = app.next("dropped");
    check(!dropped.empty() && dropped.value("prompts", json::array()).size() == 1
              && dropped["prompts"][0].value("ref", 0) == 12 && dropped["prompts"][0].value("session", "") == "wsender",
          "every app gets dropped: " + dropped.dump());
    const int status = daemon.wait();
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the daemon ended by itself");

    // ---- Files with a proposal: a new daemon, small limits, a short idle time.
    namespace fs = std::filesystem;
    const std::string outbox = home.path + "/.cache/pvai/outbox";
    fs::create_directories(outbox + "/pold1");
    writeFile(outbox + "/pold1/01-left.txt", "left over from an earlier run");
    DaemonProcess daemon2;
    daemon2.start(g_pvai, appPort + 1, 500, false,
                 {{"PVAI_PROPOSAL_LIMITS", "3145728,4194304,7340032"}, {"PVAI_UPLOAD_IDLE", "2"}, {"PVAI_CODEX", ""}});
    check(!fs::exists(outbox + "/pold1"), "a daemon start empties the outbox");

    const std::string src = home.path + "/src";
    const std::string big = content(2 * 1024 * 1024, 7), small = content(700, 9);
    writeFile(src + "/report.pdf", big);
    writeFile(src + "/my file.txt", small);
    LineClient target;
    target.send({{"type", "register"}, {"id", "wtarget"}, {"label", "target"}, {"cwd", "/x"}, {"running", true}});
    LineClient asker;
    asker.send(hello());
    const json welcome = asker.next("welcome");
    check(welcome.value("proposalFiles", false) && welcome["upload"].value("maxProposal", 0ULL) == 4194304ULL
              && welcome["upload"].value("maxOutbox", 0ULL) == 7340032ULL,
          "welcome.proposalFiles and the limits: " + welcome.dump());
    asker.next("proposals");

    // A proposal without files lists none.
    r = run("propose testhost/wtarget 'no files'", "PVAI_ID=wfrom");
    check(r.status == 0, "plain proposal: " + r.out);
    json plain = listOf(asker, proposalId(r));
    check(plain.contains("files") && plain["files"].is_array() && plain["files"].empty(), "a proposal without files has files: []");

    // Two files, in either place of the command line.
    r = run("propose testhost/wtarget --file '" + src + "/report.pdf' --file '" + src + "/my file.txt' 'with files'", "PVAI_ID=wfrom");
    check(r.status == 0, "propose with two files: " + r.out);
    const std::string pid = proposalId(r);
    const json shown = listOf(asker, pid);
    check(shown.value("text", "") == "with files" && shown["files"].size() == 2, "the proposal lists two files: " + shown.dump());
    if (shown["files"].size() == 2) {
        const json &f1 = shown["files"][0], &f2 = shown["files"][1];
        check(f1.value("id", "") == "f1" && f1.value("name", "") == "report.pdf" && f1.value("size", 0ULL) == big.size()
                  && f1.value("sha256", "") == sha256(big) && f1.value("source", "") == src + "/report.pdf",
              "file f1: " + f1.dump());
        check(f2.value("id", "") == "f2" && f2.value("name", "") == "my file.txt" && f2.value("size", 0ULL) == small.size()
                  && f2.value("sha256", "") == sha256(small) && f2.value("source", "") == src + "/my file.txt",
              "file f2: " + f2.dump());
    }
    const std::string out1 = outbox + "/" + pid + "/01-report.pdf", out2 = outbox + "/" + pid + "/02-my file.txt";
    auto mode = [](const std::string &path) {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 ? st.st_mode & 07777 : 0u;
    };
    check(fs::exists(out1) && fs::exists(out2) && mode(out1) == 0600 && mode(out2) == 0600 && mode(outbox + "/" + pid) == 0700,
          "the outbox copies exist, 0600 in a 0700 directory");

    // After the propose the original changes: the download still gives the copy.
    writeFile(src + "/report.pdf", "changed");
    const std::string got = home.path + "/got.bin";
    r = run("download " + pid + " f1 '" + got + "'");
    check(r.status == 0 && readFile(got) == big, "download gives the bytes as proposed, not the changed original: " + r.out);
    const json header = json::parse(r.out, nullptr, false);
    check(header.value("type", "") == "download" && header.value("name", "") == "report.pdf" && header.value("size", 0ULL) == big.size()
              && header.value("sha256", "") == sha256(big) && header.value("proposal", "") == pid && header.value("file", "") == "f1",
          "the download header: " + r.out);
    r = run("download " + pid + " f2 '" + got + "'");
    check(r.status == 0 && readFile(got) == small, "the second file: " + r.out);

    // On the wire: header line, exactly size bytes, then the end.
    {
        LineClient c;
        c.send(hello());
        c.send({{"type", "download"}, {"ref", "d1"}, {"proposal", pid}, {"file", "f2"}});
        const json h = c.next("download");
        check(h.value("ref", "") == "d1" && h.value("size", 0ULL) == small.size(), "wire header: " + h.dump());
        check(c.readRaw(small.size()) == small && c.atEnd(), "exactly size bytes, then the daemon closes");
    }
    // A second download at once, and a dismissed proposal finishes from its open descriptor.
    {
        LineClient c;
        c.send(hello());
        c.send({{"type", "download"}, {"ref", "d2"}, {"proposal", pid}, {"file", "f1"}});
        const json h = c.next("download");
        check(!h.empty(), "a stream starts");
        asker.send({{"type", "dismiss"}, {"ref", 5}, {"proposal", pid}});
        asker.next("dismissed");
        check(!fs::exists(outbox + "/" + pid), "dismiss removes the outbox directory");
        check(c.readRaw(big.size()) == big, "a download open at the dismissal finishes");
        LineClient after;
        after.send(hello());
        after.send({{"type", "download"}, {"ref", "d3"}, {"proposal", pid}, {"file", "f1"}});
        const json no = after.next("rejected");
        check(no.value("reason", "") == "unknown-proposal" && no.value("ref", "") == "d3", "a dismissed proposal is unknown: " + no.dump());
    }

    // Refusals.
    r = run("propose testhost/wtarget --file '" + src + "/my file.txt' 'again'", "PVAI_ID=wfrom");
    const std::string pid2 = proposalId(r);
    {
        LineClient c;
        c.send(hello());
        c.send({{"type", "download"}, {"ref", "x"}, {"proposal", pid2}, {"file", "f9"}});
        check(c.next("rejected").value("reason", "") == "unknown-file", "unknown file");
        LineClient d;
        d.send(hello());
        d.send({{"type", "download"}, {"ref", "y"}, {"file", "f1"}});
        check(d.next("rejected").value("reason", "") == "bad-request", "no proposal: bad-request");
        LineClient e;
        e.send(hello());
        e.send({{"type", "download"}, {"ref", "z"}, {"proposal", 5}, {"file", "f1"}});
        check(e.next("rejected").value("reason", "") == "bad-request", "a proposal that is no string: bad-request");
        // No hello, no download: the same rule as for an upload.
        LineClient f;
        f.send({{"type", "download"}, {"ref", "w"}, {"proposal", pid2}, {"file", "f1"}});
        const json no = f.next();
        check(no.value("type", "") == "error" && no.value("code", "") == "auth", "download without hello: " + no.dump());
        check(f.atEnd(), "and the connection closes");
    }
    // A download that is not read: no progress for the idle time, the daemon drops it; other apps are unaffected.
    writeFile(src + "/stall.bin", content(3 * 1024 * 1024, 3));
    r = run("propose testhost/wtarget --file '" + src + "/stall.bin' 'stall'", "PVAI_ID=wfrom");
    const std::string pid3 = proposalId(r);
    {
        LineClient c;
        c.send(hello());
        c.send({{"type", "download"}, {"ref", "s"}, {"proposal", pid3}, {"file", "f1"}});
        check(c.next("download").value("size", 0ULL) == 3 * 1024 * 1024ULL, "the stalled download starts");
        asker.send({{"type", "ping"}});
        check(!asker.next("pong").empty(), "the daemon answers while a download waits");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const std::string rest = c.readRaw(4 * 1024 * 1024, 1500);
        check(rest.size() < 3 * 1024 * 1024, "a stalled download is cut (" + std::to_string(rest.size()) + " bytes came)");
    }

    // Limits.
    r = run("propose testhost/wtarget --file /nonexistent/x 'text'", "PVAI_ID=wlim");
    check(r.status == 1 && r.out.find("bad-file") != std::string::npos, "a missing file: " + r.out);
    r = run("propose testhost/wtarget --file '" + src + "' 'text'", "PVAI_ID=wlim");
    check(r.status == 1 && r.out.find("bad-file") != std::string::npos, "a directory is no file: " + r.out);
    r = run("propose testhost/wtarget --file '" + src + "/my file.txt' --file /nonexistent/x 'text'", "PVAI_ID=wlim");
    check(r.status == 1 && r.out.find("bad-file") != std::string::npos, "one missing file refuses all: " + r.out);
    {
        LineClient c;
        c.send(hello());
        c.send({{"type", "propose"}, {"ref", "a"}, {"to", "testhost/wtarget"}, {"text", "t"}, {"from", "wlim"}, {"files", "nope"}});
        check(c.next("rejected").value("reason", "") == "bad-file", "files that is no array: bad-file");
        c.send({{"type", "propose"}, {"ref", "b"}, {"to", "testhost/wtarget"}, {"text", "t"}, {"from", "wlim"}, {"files", {5}}});
        check(c.next("rejected").value("reason", "") == "bad-file", "files with a non-string: bad-file");
        json many = json::array();
        for (int i = 0; i < 33; ++i)
            many.push_back(src + "/my file.txt");
        c.send({{"type", "propose"}, {"ref", "c"}, {"to", "testhost/wtarget"}, {"text", "t"}, {"from", "wlim"}, {"files", many}});
        check(c.next("rejected").value("reason", "") == "bad-file", "33 files: bad-file");
        c.send({{"type", "propose"}, {"ref", "d"}, {"to", "testhost/wtarget"}, {"text", ""}, {"from", "wlim"}, {"files", json::array()}});
        check(c.next("rejected").value("reason", "") == "empty", "no text and no files: empty");
    }
    // An empty text is fine with a file.
    r = run("propose testhost/wtarget --file '" + src + "/my file.txt'", "PVAI_ID=wlim");
    check(r.status == 0, "files without a text: " + r.out);
    const std::string pidNoText = proposalId(r);
    writeFile(src + "/huge.bin", std::string(3 * 1024 * 1024 + 1, 'h'));
    r = run("propose testhost/wtarget --file '" + src + "/huge.bin' 'text'", "PVAI_ID=wlim");
    check(r.status == 1 && r.out.find("too-large") != std::string::npos, "one file over the limit: " + r.out);
    writeFile(src + "/two.bin", std::string(2 * 1024 * 1024 + 10, 't'));
    r = run("propose testhost/wtarget --file '" + src + "/two.bin' --file '" + src + "/two.bin' 'text'", "PVAI_ID=wlim");
    check(r.status == 1 && r.out.find("too-large") != std::string::npos, "the proposal's files over the limit together: " + r.out);
    // Rejected proposals leave nothing in the outbox.
    std::size_t dirs = 0;
    for (const auto &entry : fs::directory_iterator(outbox))
        (void)entry, ++dirs;
    // Open now: pid2, pid3, pidNoText, the plain one has no directory.
    check(dirs == 3, "rejections leave no directories behind (" + std::to_string(dirs) + ")");
    // Quota: pid3 holds 3 MiB, pid2 and pidNoText 700 bytes each; 7 MiB in all.
    writeFile(src + "/quota.bin", std::string(3 * 1024 * 1024, 'q'));
    r = run("propose testhost/wtarget --file '" + src + "/quota.bin' 'text'", "PVAI_ID=wq1");
    check(r.status == 0, "a third 3 MiB proposal still fits: " + r.out);
    r = run("propose testhost/wtarget --file '" + src + "/quota.bin' 'text'", "PVAI_ID=wq2");
    check(r.status == 1 && r.out.find("quota") != std::string::npos, "the outbox is full: quota: " + r.out);
    const std::string pidQuota = proposalId(run("propose testhost/wtarget 'x'", "PVAI_ID=wq3"));
    (void)pidQuota;

    // A prompt with `from` and files only: the [Message ...] line, then the paths, never a path first.
    LineClient hook3;
    hook3.send({{"type", "hook"}, {"id", "wtarget"}, {"event", {{"hook_event_name", "SessionStart"}, {"session_id", "c-1"}, {"source", "startup"}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    writeFile(src + "/plain.txt", small);
    r = run("upload wtarget '" + src + "/plain.txt'");
    check(r.status == 0, "upload into the target: " + r.out);
    const std::string uploaded = json::parse(r.out, nullptr, false).value("path", "");
    asker.drain();
    asker.send({{"type", "prompt"}, {"ref", 40}, {"session", "wtarget"}, {"text", ""}, {"files", {uploaded}}, {"from", "M4/w8bb65867"},
                {"fromLabel", "other"}, {"proposal", "p1234abcd"}});
    const json typed3 = target.next("type");
    check(typed3.value("text", "")
              == "[Message p1234abcd from M4/w8bb65867 (other), released by the owner. Answer with: pvai propose --reply p1234abcd "
                 "M4/w8bb65867 \"...\"]\n\n" + uploaded,
          "from + files only: the message line, then the path: " + typed3.dump());

    // A restart empties the outbox, as the proposals are gone too.
    check(fs::exists(outbox + "/" + pid3), "before the restart a copy is there");
    daemon2.stop();
    DaemonProcess daemon3;
    daemon3.start(g_pvai, appPort + 2, 500, false, {{"PVAI_CODEX", ""}});
    check(fs::is_empty(outbox), "a daemon restart empties the outbox");
    return finish();
}
