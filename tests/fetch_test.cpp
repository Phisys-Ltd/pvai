// `fetch` (docs/PROTOCOL.md, "Files") against a real daemon: a read-only copy
// of a file a session printed. Fake wrappers stand in for Claude Code sessions.
//
// usage: pvai-fetch-test /path/to/pvai
#include "support.h"

#include <openssl/evp.h>
#include <sys/stat.h>

#include <fstream>

using namespace pvai;
using namespace pvai::test;
namespace fs = std::filesystem;

namespace {

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

void writeFile(const std::string &path, const std::string &text)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

json hello() { return {{"type", "hello"}, {"client", "test-fetch"}, {"version", "0"}}; }

// The first `fetch` or `rejected` line of a fetch connection; with a header, `body` gets its bytes.
json fetch(const json &request, std::string *body = nullptr)
{
    LineClient c;
    c.send(hello());
    c.send(request);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        const json m = c.next(500);
        const std::string type = m.is_object() ? m.value("type", "") : "";
        if (type == "rejected")
            return m;
        if (type == "fetch") {
            const std::string bytes = c.readRaw(m.value("size", 0ull));
            if (body)
                *body = bytes;
            check(c.atEnd(), "the connection closes after the bytes");
            return m;
        }
    }
    return json::object();
}

json req(const std::string &session, const std::string &path, const std::string &ref = "f1")
{
    return {{"type", "fetch"}, {"ref", ref}, {"session", session}, {"path", path}};
}

std::string reason(const json &answer) { return answer.value("type", "") == "rejected" ? answer.value("reason", "") : "(" + answer.dump() + ")"; }

void startSession(const std::string &id, const std::string &cwd, const std::string &transcript = "")
{
    auto *wrapper = new LineClient; // lives to the end of the test
    wrapper->send({{"type", "register"}, {"version", kVersion}, {"id", id}, {"cwd", cwd}, {"label", id}});
    json event = {{"hook_event_name", "SessionStart"}, {"session_id", "c-" + id}, {"source", "startup"}};
    if (!transcript.empty())
        event["transcript_path"] = transcript;
    LineClient hookClient;
    hookClient.send({{"type", "hook"}, {"id", id}, {"event", event}});
    hookClient.next(300);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    const std::string pvai = fs::absolute(argv[1]).string();
    const TestHome home;
    ::setenv("PVAI_CODEX", "", 1);
    ::unsetenv(kEnvId);
    char realBuf[PATH_MAX];
    const std::string realHome = ::realpath(home.path.c_str(), realBuf) ? realBuf : home.path;

    const std::string proj = realHome + "/proj", outside = realHome + "/outside";
    const std::string body = std::string("line1\n{\"type\":\"shutdown\"}\n\0bin\xff", 33) + std::string(100000, 'x');
    writeFile(proj + "/src/manifest.ts", body);
    writeFile(proj + "/empty.txt", "");
    writeFile(outside + "/secret.txt", "outside the cwd");
    writeFile(outside + "/other.txt", "not mentioned");
    writeFile(realHome + "/.claude/.credentials.json", "{\"token\":\"x\"}");
    writeFile(realHome + "/.codex/auth.json", "{}");
    writeFile(realHome + "/.ssh/id_test", "key");
    writeFile(realHome + "/.config/phi-secrets/api.env", "KEY=1");
    writeFile(realHome + "/.config/pvai/devices.json", "[]");
    writeFile(realHome + "/homefile.txt", "in the home directory");
    writeFile(proj + "/big.bin", std::string(50 * 1024 * 1024 + 1, 'b'));
    check(::symlink((outside + "/secret.txt").c_str(), (proj + "/escape.txt").c_str()) == 0, "symlink made");
    check(::symlink((realHome + "/.ssh/id_test").c_str(), (proj + "/ssh-link").c_str()) == 0, "symlink to a protected file");
    check(::symlink(proj.c_str(), (realHome + "/projlink").c_str()) == 0, "symlink to the cwd");
    ::mkdir((proj + "/dir").c_str(), 0755);
    // A transcript in which the session printed two paths outside its cwd.
    writeFile(realHome + "/t.jsonl",
              "{\"type\":\"assistant\",\"uuid\":\"u1\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"See [Secret](" + outside
                  + "/secret.txt:3) and " + outside + "/oth for more.\"}]}}\n");

    const int port = 20000 + static_cast<int>(::getpid() % 20000);
    DaemonProcess daemon;
    daemon.start(pvai, port, 500, false, {{"PVAI_CODEX", ""}});

    LineClient app;
    app.send(hello());
    const json welcome = app.next("welcome");
    check(welcome.value("fetch", json()) == json({{"maxFile", 52428800}}), "welcome.fetch: " + welcome.dump());

    startSession("wf", proj);
    startSession("wroot", "/", realHome + "/t.jsonl");
    startSession("whome", realHome, realHome + "/t.jsonl");
    startSession("wmen", outside + "/nowhere", realHome + "/t.jsonl");
    app.next("sessions", [](const json &m) { return m.value("sessions", json::array()).size() >= 4; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---- Success.
    std::string got;
    json h = fetch(req("wf", proj + "/src/manifest.ts"), &got);
    check(h.value("type", "") == "fetch" && h.value("ref", "") == "f1" && h.value("session", "") == "wf" && h.value("path", "") == proj + "/src/manifest.ts"
              && h.value("name", "") == "manifest.ts" && h.value("size", 0ull) == body.size() && h.value("sha256", "") == sha256(body)
              && h.value("mtime", 0ll) > 1000000000,
          "header: " + h.dump());
    check(got == body && sha256(got) == h.value("sha256", ""), "bytes and sha256 match");
    h = fetch(req("wf", proj + "/empty.txt"), &got);
    check(h.value("size", 1ull) == 0 && got.empty(), "an empty file: " + h.dump());
    h = fetch(req("wf", realHome + "/projlink/src/manifest.ts"), &got);
    check(h.value("path", "") == proj + "/src/manifest.ts" && got == body, "path is the realpath: " + h.dump());

    // ---- Rejections, in order.
    check(reason(fetch(req("nosuch", proj + "/src/manifest.ts"))) == "unknown-session", "unknown-session");
    check(reason(fetch({{"type", "fetch"}, {"ref", "f1"}, {"session", "wf"}})) == "bad-request", "no path: bad-request");
    check(reason(fetch({{"type", "fetch"}, {"ref", "f1"}, {"session", "wf"}, {"path", 5}})) == "bad-request", "path not a string");
    check(reason(fetch({{"type", "fetch"}, {"ref", "f1"}, {"path", "/x"}})) == "bad-request", "no session: bad-request");
    check(reason(fetch(req("wf", "src/manifest.ts"))) == "bad-request", "relative path");
    check(reason(fetch(req("wf", proj + "/nope.ts"))) == "not-found", "not-found");
    check(reason(fetch(req("wf", proj + "/dir"))) == "not-a-file", "a directory: not-a-file");
    check(reason(fetch(req("wf", proj + "/big.bin"))) == "too-large", "over 50 MiB: too-large");
    // The reply carries ref and session.
    h = fetch(req("wf", proj + "/nope.ts", "zz"));
    check(h.value("ref", "") == "zz" && h.value("session", "") == "wf", "rejected carries ref and session: " + h.dump());

    // ---- not-allowed.
    check(reason(fetch(req("wf", outside + "/other.txt"))) == "not-allowed", "outside the cwd, not mentioned");
    check(reason(fetch(req("wf", proj + "/escape.txt"))) == "not-allowed", "a symlink out of the cwd");
    check(reason(fetch(req("wf", realHome + "/.claude/.credentials.json"))) == "not-allowed", "credentials");
    check(reason(fetch(req("wf", realHome + "/.codex/auth.json"))) == "not-allowed", "codex auth");
    check(reason(fetch(req("wf", realHome + "/.ssh/id_test"))) == "not-allowed", "~/.ssh");
    check(reason(fetch(req("wf", realHome + "/.config/phi-secrets/api.env"))) == "not-allowed", "phi-secrets");
    check(reason(fetch(req("wf", realHome + "/.config/pvai/devices.json"))) == "not-allowed", "pvai config");
    check(reason(fetch(req("wf", proj + "/ssh-link"))) == "not-allowed", "a symlink to a protected file inside the cwd");
    // A working directory of / or the home directory grants nothing.
    check(reason(fetch(req("wroot", proj + "/empty.txt"))) == "not-allowed", "cwd / grants nothing");
    check(reason(fetch(req("whome", realHome + "/homefile.txt"))) == "not-allowed", "cwd = home grants nothing");
    // Mentioned in the session's output, also with a ":3" suffix in the text, also when protected-free.
    h = fetch(req("wmen", outside + "/secret.txt"), &got);
    check(h.value("type", "") == "fetch" && got == "outside the cwd", "a path in the session's output is allowed: " + h.dump());
    h = fetch(req("wroot", outside + "/secret.txt"), &got);
    check(h.value("type", "") == "fetch", "mentioned: also with cwd /");
    check(reason(fetch(req("wmen", outside + "/other.txt"))) == "not-allowed", "a path that is only a prefix of a mention (/oth) or never named stays not-allowed");
    check(reason(fetch(req("wf", outside + "/secret.txt"))) == "not-allowed", "another session's mention does not count");
    // The inbox of the session.
    writeFile(realHome + "/.cache/pvai/inbox/wf/up.txt", "uploaded");
    h = fetch(req("wf", realHome + "/.cache/pvai/inbox/wf/up.txt"), &got);
    check(h.value("type", "") == "fetch" && got == "uploaded", "the session's inbox: " + h.dump());
    check(reason(fetch(req("wmen", realHome + "/.cache/pvai/inbox/wf/up.txt"))) == "not-allowed", "another session's inbox");

    return finish();
}
