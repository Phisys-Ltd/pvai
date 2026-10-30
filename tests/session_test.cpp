// The session lifecycle against a real daemon: Claude sessions through a
// fake wrapper and fake hooks, Codex sessions through a fake app server, and
// for both versions, updates, ended conversations and resume. `claude` and
// `codex` are small scripts here, so nothing real starts.
//
// usage: pvai-session-test /path/to/pvai
#include "common.h"

#include <poll.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

using namespace pvai;
namespace fs = std::filesystem;

namespace {

std::string g_pvai;
int g_failed = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

void writeFile(const std::string &path, const std::string &text, bool executable = false)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path) << text;
    if (executable)
        fs::permissions(path, fs::perms::owner_all);
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
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
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", daemonSocket().c_str());
        ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    }
    ~Client() { ::close(fd); }

    void send(const json &message)
    {
        const std::string line = message.dump() + "\n";
        (void)::write(fd, line.data(), line.size());
    }

    // The next message within `ms`; an empty object after (so that a
    // missing answer fails a check instead of the test).
    json next(int ms = 2000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (const auto end = buffer.find('\n'); end != std::string::npos) {
                const std::string line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                return json::parse(line, nullptr, false);
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            pollfd p{fd, POLLIN, 0};
            if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0)
                return json::object();
            char chunk[65536];
            const ssize_t n = ::read(fd, chunk, sizeof chunk);
            if (n <= 0)
                return json::object();
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }

    // The next message of that type (and, if given, matching `pred`).
    json next(const std::string &type, const std::function<bool(const json &)> &pred = {}, int ms = 3000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            const json message =
                next(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()));
            if (message.empty())
                break;
            if (message.is_object() && message.value("type", "") == type && (!pred || pred(message)))
                return message;
        }
        return json::object();
    }

    void drain()
    {
        while (!next(100).empty()) {
        }
    }
};

json app()
{
    return {{"type", "hello"}, {"client", "test-app"}, {"version", "0"}};
}

void hook(const std::string &id, json event)
{
    Client c;
    c.send({{"type", "hook"}, {"id", id}, {"event", std::move(event)}});
    c.next(300);
}

// The session `id` in a `sessions` message, or an empty object.
json sessionIn(const json &sessions, const std::string &id)
{
    if (sessions.is_object() && sessions.contains("sessions"))
        for (const auto &s : sessions["sessions"])
            if (s.value("id", "") == id)
                return s;
    return json::object();
}

// `j[key]` when that is an object, else an empty one.
json sub(const json &j, const char *key)
{
    return j.is_object() && j.contains(key) && j[key].is_object() ? j[key] : json::object();
}

// The next `sessions` list in which `id` matches `pred`.
json waitSession(Client &client, const std::string &id, const std::function<bool(const json &)> &pred, int ms = 3000)
{
    const json found = client.next("sessions", [&](const json &m) {
        const json s = sessionIn(m, id);
        return !s.empty() && pred(s);
    }, ms);
    return sessionIn(found, id);
}

// ---- A fake Codex app server: one WebSocket client on its control socket.

class FakeCodex {
public:
    explicit FakeCodex(const std::string &path)
    {
        fs::create_directories(fs::path(path).parent_path());
        listen_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        ::bind(listen_, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        ::listen(listen_, 4);
        thread_ = std::thread([this] { loop(); });
    }
    ~FakeCodex()
    {
        stop_ = true;
        thread_.join();
        ::close(listen_);
    }

    void send(const json &message)
    {
        std::lock_guard lock(mutex_);
        out_.push_back(message);
    }

    // The first message from the daemon with that method (or a response with
    // that id) since the start, waiting up to 3 s; null when none came.
    json received(const std::function<bool(const json &)> &pred, int ms = 3000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard lock(mutex_);
                for (const auto &m : in_)
                    if (pred(m))
                        return m;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return nullptr;
    }
    json call(const std::string &method, const std::function<bool(const json &)> &params = {})
    {
        return received([&](const json &m) { return m.value("method", "") == method && (!params || params(m.value("params", json()))); });
    }

    std::atomic<int> threadListCalls{0};

private:
    void loop()
    {
        while (!stop_) {
            pollfd p[2] = {{listen_, POLLIN, 0}, {client_, POLLIN, 0}};
            ::poll(p, 2, 20);
            if (p[0].revents & POLLIN)
                accept();
            if (client_ >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
                char chunk[65536];
                const ssize_t n = ::read(client_, chunk, sizeof chunk);
                if (n <= 0) {
                    ::close(client_);
                    client_ = -1;
                } else {
                    in_raw_.append(chunk, static_cast<std::size_t>(n));
                    frames();
                }
            }
            std::deque<json> out;
            {
                std::lock_guard lock(mutex_);
                out.swap(out_);
            }
            for (const auto &m : out)
                frame(m.dump());
        }
        if (client_ >= 0)
            ::close(client_);
    }

    void accept()
    {
        const int fd = ::accept(listen_, nullptr, nullptr);
        std::string request;
        char c;
        while (request.find("\r\n\r\n") == std::string::npos && ::read(fd, &c, 1) == 1)
            request += c;
        const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        (void)::write(fd, reply.data(), reply.size());
        client_ = fd;
    }

    void frame(const std::string &payload)
    {
        std::string f(1, static_cast<char>(0x81));
        if (payload.size() < 126) {
            f += static_cast<char>(payload.size());
        } else {
            f += static_cast<char>(126);
            f += static_cast<char>((payload.size() >> 8) & 0xff);
            f += static_cast<char>(payload.size() & 0xff);
        }
        f += payload;
        if (client_ >= 0)
            (void)::write(client_, f.data(), f.size());
    }

    void frames()
    {
        for (;;) {
            if (in_raw_.size() < 2)
                return;
            std::size_t length = static_cast<unsigned char>(in_raw_[1]) & 0x7f, at = 2;
            if (length == 126) {
                if (in_raw_.size() < 4)
                    return;
                length = (static_cast<unsigned char>(in_raw_[2]) << 8) | static_cast<unsigned char>(in_raw_[3]);
                at = 4;
            }
            if (in_raw_.size() < at + 4 + length)
                return;
            std::string payload = in_raw_.substr(at + 4, length);
            for (std::size_t i = 0; i < length; ++i)
                payload[i] = static_cast<char>(payload[i] ^ in_raw_[at + i % 4]);
            const unsigned opcode = static_cast<unsigned char>(in_raw_[0]) & 0x0f;
            in_raw_.erase(0, at + 4 + length);
            if (opcode != 1)
                continue;
            const json message = json::parse(payload, nullptr, false);
            {
                std::lock_guard lock(mutex_);
                in_.push_back(message);
            }
            answer(message);
        }
    }

    void reply(const json &id, const json &result) { frame(json({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).dump()); }

    // What the app server would say to each call.
    void answer(const json &m)
    {
        const std::string method = m.value("method", "");
        if (!m.contains("id") || method.empty())
            return;
        const json id = m["id"];
        const json params = m.value("params", json::object());
        auto thread = [](const std::string &tid, const std::string &cwd) {
            return json{{"id", tid}, {"cwd", cwd}, {"status", {{"type", "idle"}}}, {"model", "gpt-test"}, {"parentThreadId", nullptr}};
        };
        if (method == "initialize")
            reply(id, {{"userAgent", "codex_test/7.7.7 (Linux; test)"}});
        else if (method == "account/rateLimits/read")
            reply(id, {{"rateLimits", json::object()}});
        else if (method == "model/list")
            reply(id, {{"data", {{{"model", "gpt-test"}, {"hidden", false}}, {{"model", "gpt-hidden"}, {"hidden", true}}}}});
        else if (method == "thread/loaded/list")
            reply(id, {{"data", {"t-tui"}}});
        else if (method == "thread/resume") {
            const std::string tid = params.value("threadId", "");
            if (tid == "t-tui" || tid == "t-arch")
                reply(id, {{"thread", thread(tid, "/tmp")}});
            else
                frame(json({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", "no such thread"}}}}).dump());
        } else if (method == "thread/items/list") {
            json data = json::array();
            if (params.value("threadId", "") == "t-tui")
                data.push_back({{"item", {{"type", "agentMessage"}, {"id", "i1"}, {"text", "earlier answer"}}}, {"completedAtMs", 1000}});
            reply(id, {{"data", data}});
        } else if (method == "thread/start")
            reply(id, {{"thread", thread("t-new", params.value("cwd", ""))}});
        else if (method == "thread/list") {
            ++threadListCalls;
            const bool archived = params.value("archived", false);
            json data = json::array();
            if (archived)
                data.push_back({{"id", "t-arch"}, {"preview", "\xF0\x9F\x8E\x99 archived one"}, {"updatedAt", 200}, {"parentThreadId", nullptr}});
            else {
                data.push_back({{"id", "t-old"}, {"preview", "old prompt"}, {"updatedAt", 100}, {"parentThreadId", nullptr}});
                data.push_back({{"id", "t-new"}, {"preview", "loaded"}, {"updatedAt", 300}, {"parentThreadId", nullptr}});
                data.push_back({{"id", "t-sub"}, {"preview", "a subagent"}, {"updatedAt", 400}, {"parentThreadId", "t-old"}});
            }
            reply(id, {{"data", data}});
        } else
            reply(id, json::object());
    }

    int listen_ = -1;
    int client_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<json> out_;
    std::deque<json> in_;
    std::string in_raw_;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    g_pvai = fs::canonical(argv[1]).string();
    char homeBuf[] = "/tmp/pvai-test-XXXXXX";
    if (!::mkdtemp(homeBuf))
        return 2;
    const std::string home = homeBuf;
    const std::string runtime = home + "/run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all);
    const std::string bin = home + "/bin";
    // claude: a version that `update` raises, and a session that records
    // its arguments and waits. codex: an update that fails.
    writeFile(bin + "/claude.version", "1.0.0\n");
    writeFile(bin + "/claude",
              "#!/bin/sh\nd=$(dirname \"$0\")\ncase \"$1\" in\n"
              "--version) echo \"$(cat \"$d/claude.version\") (Claude Code)\";;\n"
              "update) echo 2.0.0 > \"$d/claude.version\"; echo updated;;\n"
              "*) echo \"$@\" >> \"$d/claude.args\"; exec sleep 30;;\nesac\n",
              true);
    writeFile(bin + "/codex",
              "#!/bin/sh\ncase \"$1\" in\n--version) echo codex-cli 5.5.5;;\n"
              "update) echo checking; echo 'npm ERR! needs root' >&2; exit 1;;\nesac\n",
              true);
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    ::setenv("PATH", (bin + ":" + std::getenv("PATH")).c_str(), 1);
    ::setenv("PVAI_CODEX", (bin + "/codex").c_str(), 1);
    ::unsetenv(kEnvId);
    const int appPort = 20000 + static_cast<int>(::getpid() % 20000);

    // Ended Claude conversations in a project folder.
    const std::string project = home + "/proj";
    fs::create_directories(project);
    std::string encoded = project;
    for (char &c : encoded)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '-';
    const std::string transcripts = home + "/.claude/projects/" + encoded;
    auto userLine = [](const std::string &text, bool meta = false) {
        return json({{"type", "user"}, {"isMeta", meta}, {"message", {{"role", "user"}, {"content", text}}}}).dump() + "\n";
    };
    writeFile(transcripts + "/c0ffee00-1111-2222-3333-444455556666.jsonl",
              userLine("caveat", true) + userLine("<command-name>/model</command-name>") + userLine("\xF0\x9F\x8E\x99 Erste Frage hier"));
    writeFile(transcripts + "/deadbeef-only-a-command.jsonl", userLine("<command-name>/model</command-name>"));
    writeFile(transcripts + "/running-1.jsonl", userLine("still running"));

    FakeCodex codex(home + "/.codex/app-server-control/app-server-control.sock");

    const pid_t daemon = ::fork();
    if (daemon == 0) {
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGTERM); // gone with the test, even when it crashes
#endif
        const std::string port = std::to_string(appPort);
        ::execl(g_pvai.c_str(), "pvai", "daemon", "--port", port.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    Client a;
    a.send(app());
    json welcome = a.next("welcome");
    const json assistants = welcome.value("assistants", json());
    check(assistants.size() == 2 && assistants[0].value("name", "") == "claude" && assistants[0].value("title", "") == "Claude Code"
              && assistants[0].value("models", json()).size() == 3 && assistants[1].value("name", "") == "codex",
          "welcome lists both assistants as objects: " + assistants.dump());
    check(welcome.value("installed", json()) == json({{"claude", "1.0.0"}, {"codex", "5.5.5"}}),
          "welcome.installed: " + welcome.value("installed", json()).dump());
    check(welcome.value("conversations", false), "welcome.conversations");

    // ---- Codex: a TUI's thread is adopted, with its history and version.
    json tui = waitSession(a, "t-tui", [](const json &) { return true; });
    check(tui.value("assistant", "") == "codex" && !tui.value("detached", true), "a loaded thread becomes a session: " + tui.dump());
    check(sub(tui, "usage").value("version", json()) == "7.7.7", "usage.version is the app server's");
    a.send({{"type", "subscribe"}, {"session", "t-tui"}});
    json history = a.next("history");
    const json entries = history.value("entries", json::array());
    check(entries.size() == 1 && entries[0].value("text", "") == "earlier answer",
          "its history comes from thread/items/list");
    a.send({{"type", "stop"}, {"ref", 1}, {"session", "t-tui"}});
    check(a.next("rejected").value("reason", "") == "not-detached", "a TUI's thread cannot be stopped");

    // A new thread: prompt, turn, spoken answer.
    a.send({{"type", "start"}, {"ref", 2}, {"assistant", "codex"}, {"cwd", project}, {"label", "neu"}, {"args", {"-m", "gpt-test"}}});
    json started = a.next("started");
    check(started.value("session", "") == "t-new", "codex start: " + started.dump());
    check(!codex.call("thread/start", [](const json &p) { return p.value("model", "") == "gpt-test"; }).is_null(), "the model goes to thread/start");
    json fresh = waitSession(a, "t-new", [](const json &s) { return s.value("label", "") == "neu"; });
    check(fresh.value("detached", false), "a started thread is detached and labelled");
    a.send({{"type", "prompt"}, {"ref", 3}, {"session", "t-new"}, {"text", "hello"}});
    check(a.next("accepted").value("delivery", "") == "typed", "the prompt goes in");
    check(!codex.call("turn/start", [](const json &p) { return p.dump().find("hello") != std::string::npos; }).is_null(), "as turn/start");
    codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u1"}}}}}});
    codex.send({{"method", "item/completed"},
                {"params", {{"threadId", "t-new"}, {"item", {{"type", "agentMessage"}, {"id", "i2"}, {"text", "Answer.\n\xF0\x9F\x94\x8A Spoken."}}}}}});
    codex.send({{"method", "turn/completed"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u1"}, {"status", "completed"}}}}}});
    json say = a.next("say", [](const json &m) { return m.value("session", "") == "t-new"; });
    check(say.value("text", "") == "Spoken.", "the turn's 🔊 line is said: " + say.dump());

    // An approval, answered from the app; then an interrupt.
    codex.send({{"id", 99}, {"method", "item/commandExecution/requestApproval"}, {"params", {{"threadId", "t-new"}, {"command", "ls"}}}});
    json attention = a.next("attention");
    check(attention.value("kind", "") == "permission" && attention.value("text", "") == "ls", "an approval asks the owner");
    a.send({{"type", "answer"}, {"ref", 4}, {"session", "t-new"}, {"choice", "allow"}});
    check(!a.next("answered").empty(), "answered");
    check(!codex.received([](const json &m) { return m.value("id", json()) == 99 && m.value("result", json()).value("decision", "") == "accept"; }).is_null(),
          "allow is accept");
    codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u2"}}}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    a.send({{"type", "interrupt"}, {"ref", 5}, {"session", "t-new"}});
    check(!a.next("interrupting").empty(), "interrupting");
    check(!codex.call("turn/interrupt", [](const json &p) { return p.value("turnId", "") == "u2"; }).is_null(), "as turn/interrupt");

    // Ended threads of the folder: archived too, without loaded ones and
    // subagents, newest first.
    a.send({{"type", "conversations"}, {"assistant", "codex"}, {"cwd", project}});
    json list = a.next("conversations");
    check(list.value("assistant", "") == "codex" && list.value("cwd", "") == project, "conversations echoes assistant and cwd");
    check(list.value("conversations", json()) == json({{{"id", "t-arch"}, {"title", "archived one"}, {"at", 200}},
                                                       {{"id", "t-old"}, {"title", "old prompt"}, {"at", 100}}}),
          "codex conversations: " + list.value("conversations", json()).dump());

    a.send({{"type", "start"}, {"ref", 6}, {"assistant", "codex"}, {"cwd", project}, {"resume", "t-arch"}});
    started = a.next("started");
    check(started.value("session", "") == "t-arch", "resume an archived thread: " + started.dump());
    check(!codex.call("thread/unarchive", [](const json &p) { return p.value("threadId", "") == "t-arch"; }).is_null(), "it is unarchived first");
    check(!waitSession(a, "t-arch", [](const json &s) { return s.value("detached", false); }).empty(), "and a detached session");
    a.send({{"type", "start"}, {"ref", 7}, {"assistant", "codex"}, {"cwd", project}, {"resume", "t-missing"}});
    check(a.next("rejected").value("reason", "") == "unknown-conversation", "an unknown thread is unknown-conversation");

    a.send({{"type", "stop"}, {"ref", 8}, {"session", "t-new"}});
    check(!a.next("stopping").empty(), "stopping a started thread");
    check(!codex.call("thread/archive", [](const json &p) { return p.value("threadId", "") == "t-new"; }).is_null(), "archives it");
    check(!waitSession(a, "t-arch", [](const json &) { return true; }).empty(), "the others stay");

    // ---- Claude in a terminal: a fake wrapper and hooks.
    Client wrapper;
    wrapper.send({{"type", "register"}, {"id", "wterm"}, {"cwd", project}, {"label", "term"}});
    // (An old wrapper: no `restart` in its registration.)
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "starting"; }).empty(), "a wrapper registers, starting");
    hook("wterm", {{"hook_event_name", "SessionStart"}, {"session_id", "running-1"}, {"source", "startup"}});
    json term = waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; });
    check(term.value("assistant", "") == "claude" && !term.value("detached", true) && term.value("claudeSession", "") == "running-1",
          "SessionStart makes it idle: " + term.dump());
    check(!term.value("restartable", true), "an old wrapper's session is not restartable");
    a.send({{"type", "prompt"}, {"ref", 10}, {"session", "wterm"}, {"text", "one"}});
    check(wrapper.next("type").value("text", "") == "one", "the first prompt is typed");
    a.send({{"type", "prompt"}, {"ref", 11}, {"session", "wterm"}, {"text", "two"}, {"voice", true}});
    check(a.next("accepted", [](const json &m) { return m.value("ref", 0) == 11; }).value("delivery", "") == "queued", "the second waits");
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "one"}});
    hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "Done.\n\xF0\x9F\x94\x8A Fertig."}});
    say = a.next("say", [](const json &m) { return m.value("session", "") == "wterm"; });
    check(say.value("text", "") == "Fertig." && !say.value("voice", true), "Stop says the 🔊 line");
    check(wrapper.next("type").value("text", "") == "\xF0\x9F\x8E\x99 two", "then the queued, spoken prompt goes in with 🎙");
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "\xF0\x9F\x8E\x99 two"}});
    hook("wterm", {{"hook_event_name", "Notification"}, {"notification_type", "permission_prompt"}, {"message", "Claude needs your permission"}});
    check(a.next("attention").value("kind", "") == "permission", "a permission dialog asks the owner");
    a.send({{"type", "answer"}, {"ref", 12}, {"session", "wterm"}, {"choice", "allow"}});
    check(wrapper.next("keys").value("data", "") == "\r", "allow is Return");
    a.send({{"type", "stop"}, {"ref", 13}, {"session", "wterm"}});
    check(a.next("rejected").value("reason", "") == "not-detached", "a terminal session is the owner's to end");
    {
        Client status;
        status.send({{"type", "status"}, {"id", "wterm"}, {"status", {{"version", "1.0.0"}, {"model", {{"display_name", "Opus"}}}}}});
        json usage = a.next("usage", [](const json &m) { return m.value("session", "") == "wterm"; });
        check(sub(usage, "usage").value("version", "") == "1.0.0", "usage.version from the status line");
    }
    a.send({{"type", "interrupt"}, {"ref", 14}, {"session", "wterm"}});
    check(wrapper.next("keys").value("data", "") == "\x1b", "interrupt is Esc");
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 6000).empty(),
          "without an interrupt in the transcript it is idle after about 4 s");

    // Ended Claude conversations: titles, no command-only ones, not the
    // running one.
    a.send({{"type", "conversations"}, {"assistant", "claude"}, {"cwd", project}});
    list = a.next("conversations");
    const json found = list.value("conversations", json());
    check(found.size() == 1 && found[0].value("id", "") == "c0ffee00-1111-2222-3333-444455556666"
              && found[0].value("title", "") == "Erste Frage hier" && found[0].value("at", 0LL) > 0,
          "claude conversations: " + found.dump());
    a.send({{"type", "start"}, {"ref", 15}, {"assistant", "claude"}, {"cwd", project}, {"resume", "../../etc/passwd"}});
    check(a.next("rejected").value("reason", "") == "unknown-conversation", "a bad id is unknown-conversation");
    a.send({{"type", "start"}, {"ref", 16}, {"assistant", "claude"}, {"cwd", project}, {"resume", "c0ffee00-1111-2222-3333-444455556666"}});
    started = a.next("started");
    const std::string resumed = started.value("session", "");
    check(!resumed.empty(), "resume a claude conversation");
    json detached = waitSession(a, resumed, [](const json &) { return true; }, 5000);
    check(detached.value("detached", false), "a detached wrapper registers: " + detached.dump());
    for (int i = 0; i < 50 && readFile(bin + "/claude.args").empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(readFile(bin + "/claude.args").find("--resume c0ffee00-1111-2222-3333-444455556666") != std::string::npos,
          "claude runs with --resume: " + readFile(bin + "/claude.args"));
    a.send({{"type", "stop"}, {"ref", 17}, {"session", resumed}});
    check(!a.next("stopping").empty(), "a detached session stops");
    check(!a.next("sessions", [&](const json &m) { return sessionIn(m, resumed).empty(); }, 5000).empty(), "and leaves the list");

    // Restart in place: the wrapper ends claude and starts it again with
    // --resume, the session keeps its id.
    a.send({{"type", "restart"}, {"ref", 18}, {"session", "t-arch"}});
    check(a.next("rejected").value("reason", "") == "not-restartable", "a Codex thread is not restartable");
    fs::remove(bin + "/claude.args");
    a.send({{"type", "start"}, {"ref", 19}, {"assistant", "claude"}, {"cwd", project}, {"args", {"--model", "opus", "--continue"}}});
    const std::string fresh2 = a.next("started").value("session", "");
    check(!waitSession(a, fresh2, [](const json &s) { return !s.value("restartable", true); }, 5000).empty(),
          "not restartable before Claude Code's session id is known");
    hook(fresh2, {{"hook_event_name", "SessionStart"}, {"session_id", "sess-1"}, {"source", "startup"}});
    check(!waitSession(a, fresh2, [](const json &s) { return s.value("restartable", false) && s.value("state", "") == "idle"; }).empty(),
          "restartable once it is");
    hook(fresh2, {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "x"}});
    a.send({{"type", "restart"}, {"ref", 20}, {"session", fresh2}});
    check(a.next("rejected").value("reason", "") == "busy", "not during a turn");
    hook(fresh2, {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
    a.send({{"type", "restart"}, {"ref", 21}, {"session", fresh2}});
    check(a.next("restarting").value("session", "") == fresh2, "restarting");
    check(!waitSession(a, fresh2, [](const json &s) { return s.value("state", "") == "starting"; }).empty(), "starting again");
    std::string args;
    for (int i = 0; i < 50 && args.find("--resume sess-1") == std::string::npos; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        args = readFile(bin + "/claude.args");
    }
    check(args == "--model opus --continue\n--model opus --resume sess-1\n", "the same arguments, --continue gives way to --resume: " + args);
    hook(fresh2, {{"hook_event_name", "SessionStart"}, {"session_id", "sess-1"}, {"source", "resume"}});
    check(!waitSession(a, fresh2, [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle again with the same id");
    a.send({{"type", "stop"}, {"ref", 22}, {"session", fresh2}});
    check(!a.next("sessions", [&](const json &m) { return sessionIn(m, fresh2).empty(); }, 5000).empty(), "and it still stops");

    // ---- Versions and updates.
    a.send({{"type", "update"}, {"ref", 20}});
    json updated = a.next("updated", {}, 10000);
    check(updated.value("ref", 0) == 20, "update answers updated");
    check(updated.value("changed", json()) == json({{"claude", {"1.0.0", "2.0.0"}}}), "changed: " + updated.value("changed", json()).dump());
    check(updated.value("errors", json()) == json({{"codex", "npm ERR! needs root"}}), "errors: " + updated.value("errors", json()).dump());
    check(updated.value("installed", json()) == json({{"claude", "2.0.0"}, {"codex", "5.5.5"}}), "installed after the update");
    a.send({{"type", "start"}, {"ref", 21}, {"assistant", "kimi"}, {"cwd", project}});
    check(a.next("rejected").value("reason", "") == "unknown-assistant", "an unknown assistant is refused");
    a.send({{"type", "conversations"}, {"assistant", "kimi"}, {"cwd", project}});
    check(a.next("conversations").value("conversations", json()) == json::array(), "and has no conversations");

    // ---- Hooks: pvai run puts them in place, once.
    auto shell = [](const std::string &command) {
        std::string out;
        if (FILE *pipe = ::popen((command + " 2>&1 </dev/null").c_str(), "r")) {
            char buffer[4096];
            while (std::fgets(buffer, sizeof buffer, pipe))
                out += buffer;
            ::pclose(pipe);
        }
        return out;
    };
    const std::string settings = home + "/.claude/settings.json";
    writeFile(settings, R"({"model": "opus", "statusLine": {"type": "command", "command": "my-line"}})");
    std::string out = shell(g_pvai + " run -- claude --version");
    check(out.find("hooks and status line") != std::string::npos && out.find("(Claude Code)") != std::string::npos,
          "pvai run writes missing hooks, then runs claude: " + out);
    const json written = json::parse(readFile(settings), nullptr, false);
    check(written.is_object() && written.value("model", "") == "opus" && sub(written, "hooks").size() == 4
              && sub(written, "statusLine").value("command", "") == g_pvai + " statusline --chain 'my-line'",
          "the rest of the file stays, the own status line is chained: " + written.dump());
    out = shell(g_pvai + " run -- claude --version");
    check(out.find("hooks and status line") == std::string::npos, "a second run leaves the file alone: " + out);
    check(shell(g_pvai + " install-hooks").find("in place") != std::string::npos, "install-hooks says they are in place");

    ::kill(daemon, SIGTERM);
    ::waitpid(daemon, nullptr, 0);
    fs::remove_all(home);
    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
