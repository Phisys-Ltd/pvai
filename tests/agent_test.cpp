// Agent messages against a real daemon: `pvai propose`, the proposals an app
// sees, dismiss, the limits, and the first line a released message gets.
// Runs in its own HOME and runtime directory, with a fake wrapper as the
// target session.
//
// usage: pvai-agent-test /path/to/pvai
#include "common.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace pvai;

namespace {

std::string g_pvai;
int g_failed = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

struct Result {
    int status = -1;
    std::string out;
};

// Runs pvai with `env` before it and `input` on stdin.
Result run(const std::string &args, const std::string &env = "", const std::string &input = "")
{
    Result result;
    const std::string command = (input.empty() ? "" : "printf %s '" + input + "' | ") + env + " " + g_pvai + " "
        + args + " 2>&1";
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe)
        return result;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe))
        result.out += buffer;
    const int status = ::pclose(pipe);
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    while (!result.out.empty() && result.out.back() == '\n')
        result.out.pop_back();
    return result;
}

// A line client on the daemon's unix socket.
struct Client {
    int fd = -1;
    std::string buffer;

    Client()
    {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string path = daemonSocket();
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        timeval limit{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    }
    ~Client() { ::close(fd); }

    void send(const json &message)
    {
        const std::string line = message.dump() + "\n";
        (void)::write(fd, line.data(), line.size());
    }

    // The next message, null after 2 s.
    json next()
    {
        for (;;) {
            if (const auto end = buffer.find('\n'); end != std::string::npos) {
                const std::string line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                return json::parse(line, nullptr, false);
            }
            char chunk[4096];
            const ssize_t n = ::read(fd, chunk, sizeof chunk);
            if (n <= 0)
                return nullptr;
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }

    // The next message of that type, skipping others.
    json next(const std::string &type)
    {
        for (json message = next(); !message.is_null(); message = next())
            if (message.is_object() && message.value("type", "") == type)
                return message;
        return nullptr;
    }
};

std::size_t count(const json &proposals) { return proposals.is_null() ? 0 : proposals["proposals"].size(); }

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    g_pvai = std::filesystem::absolute(argv[1]).string();
    char home[] = "/tmp/pvai-test-XXXXXX";
    if (!::mkdtemp(home))
        return 2;
    const std::string runtime = std::string(home) + "/run";
    std::filesystem::create_directories(runtime);
    std::filesystem::permissions(runtime, std::filesystem::perms::owner_all);
    ::setenv("HOME", home, 1);
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    ::setenv("PVAI_CODEX", "", 1);
    ::unsetenv(kEnvId);
    const int appPort = 20000 + static_cast<int>(::getpid() % 20000);

    const pid_t daemon = ::fork();
    if (daemon == 0) {
        const std::string port = std::to_string(appPort);
        ::execl(g_pvai.c_str(), "pvai", "daemon", "--port", port.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Without an app it still waits, and says so.
    Result r = run("propose taifun/w1 'hello there'", "PVAI_ID=wsender");
    check(r.status == 0 && r.out.find("no app connected now") != std::string::npos, "propose without an app: " + r.out);

    // An app sees it after welcome, with the sender's label once it is known.
    Client wrapper;
    wrapper.send({{"type", "register"}, {"id", "wsender"}, {"label", "sender-label"}, {"cwd", "/x"}, {"running", true}});
    Client app;
    app.send({{"type", "hello"}, {"client", "test-app"}, {"version", "0"}});
    check(!app.next("welcome").is_null(), "the app is welcome");
    json list = app.next("proposals");
    check(count(list) == 1, "proposals follow welcome");
    if (count(list) == 1) {
        const json &p = list["proposals"][0];
        check(p.value("from", "") == "wsender" && p.value("to", "") == "taifun/w1" && p.value("text", "") == "hello there"
                  && p.value("id", "").size() == 9 && p.value("at", 0LL) > 0,
              "the proposal's fields: " + p.dump());
    }

    r = run("propose M4", "PVAI_ID=wsender", "from stdin\nsecond line");
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
    r = run("propose taifun \"$(head -c 40000 /dev/zero | tr '\\0' x)\"");
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
    Client second, third;
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
    while (!app.next().is_null()) {
    }

    // Dismiss: gone from the list; an unknown one is answered alike.
    app.send({{"type", "dismiss"}, {"ref", 7}, {"proposal", first}});
    list = app.next("proposals");
    check(count(list) == 1, "dismiss removes it from the list");
    json answer = app.next("dismissed");
    check(!answer.is_null() && answer.value("proposal", "") == first && answer.value("ref", 0) == 7, "dismissed answers");
    app.send({{"type", "dismiss"}, {"ref", 8}, {"proposal", "pnothere0"}});
    answer = app.next("dismissed");
    check(!answer.is_null() && answer.value("proposal", "") == "pnothere0", "an unknown proposal is dismissed too");

    // Ten a minute per session, sixteen open in all.
    int ok = 0;
    for (int i = 0; i < 12; ++i)
        if (run("propose taifun text" + std::to_string(i), "PVAI_ID=wbusy").status == 0)
            ++ok;
    check(ok == 10, "ten a minute per session (" + std::to_string(ok) + ")");
    ok = 0;
    for (int i = 0; i < 8; ++i)
        if (run("propose taifun text", "PVAI_ID=wother" + std::to_string(i)).status == 0)
            ++ok;
    check(ok == 5, "sixteen open in all (" + std::to_string(ok) + " more)");
    r = run("propose taifun text", "PVAI_ID=wlast");
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
    while (!app.next().is_null()) {
    }
    app.send({{"type", "prompt"}, {"ref", 9}, {"session", "wsender"}, {"text", "please check"}, {"from", "M4/w8bb65867"},
              {"fromLabel", "phi-claude-voice"}});
    json typed = wrapper.next("type");
    check(typed.value("text", "")
              == "[Message from M4/w8bb65867 (phi-claude-voice), released by the owner. Answer with: pvai propose "
                 "M4/w8bb65867 \"...\"]\nplease check",
          "the released prompt starts with the sender line: " + typed.dump());

    // A plain prompt has no such line.
    Client hook;
    hook.send({{"type", "hook"}, {"id", "wsender"}, {"event", {{"hook_event_name", "Stop"}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.send({{"type", "prompt"}, {"ref", 10}, {"session", "wsender"}, {"text", "plain"}});
    typed = wrapper.next("type");
    check(typed.value("text", "") == "plain", "a prompt without from is typed as it is: " + typed.dump());

    // With the proposal's id and what it answers: both in the line, and the
    // way to answer names it.
    Client hook2;
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
    while (!accepted.is_null() && accepted.value("ref", 0) != 12)
        accepted = app.next("accepted");
    check(!accepted.is_null() && accepted.value("delivery", "") == "queued", "a prompt waits behind the running turn");
    r = run("daemon stop");
    check(r.status == 1 && r.out.find("not stopped: prompts are waiting") != std::string::npos
              && r.out.find("wsender \"sender-label\": 1 queued") != std::string::npos,
          "daemon stop is refused while one waits: " + r.out);
    r = run("daemon stop --force");
    check(r.status == 0 && r.out.find("dropped: wsender ref 12: waits") != std::string::npos && r.out.find("daemon stopped") != std::string::npos,
          "--force stops it and names what went: " + r.out);
    const json dropped = app.next("dropped");
    check(!dropped.is_null() && dropped.value("prompts", json::array()).size() == 1
              && dropped["prompts"][0].value("ref", 0) == 12 && dropped["prompts"][0].value("session", "") == "wsender",
          "every app gets dropped: " + dropped.dump());
    int status = -1;
    ::waitpid(daemon, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the daemon ended by itself");
    std::filesystem::remove_all(home);
    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
