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
    json call(const std::string &method, const std::function<bool(const json &)> &params = {}, int ms = 3000)
    {
        return received([&](const json &m) { return m.value("method", "") == method && (!params || params(m.value("params", json()))); }, ms);
    }

    std::atomic<int> threadListCalls{0};
    std::atomic<bool> kidActive{true}; // the subagent thread t-kid of t-new
    json kid() const
    {
        return {{"id", "t-kid"}, {"parentThreadId", "t-new"}, {"agentRole", "explorer"}, {"agentNickname", "Hume"},
                {"preview", "Look around the repo"}, {"createdAt", 1793000000},
                {"status", {{"type", kidActive ? "active" : "idle"}, {"activeFlags", json::array()}}}};
    }

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
            reply(id, {{"data", {"t-tui", "t-kid"}}});
        else if (method == "thread/resume") {
            const std::string tid = params.value("threadId", "");
            if (tid == "t-tui" || tid == "t-arch" || tid == "t-tui2" || tid == "t-free")
                reply(id, {{"thread", thread(tid, "/tmp")}});
            else if (tid == "t-kid")
                reply(id, {{"thread", kid()}});
            else
                frame(json({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", "no such thread"}}}}).dump());
        } else if (method == "thread/items/list") {
            json data = json::array();
            if (params.value("threadId", "") == "t-tui")
                data.push_back({{"item", {{"type", "agentMessage"}, {"id", "i1"}, {"text", "earlier answer"}}}, {"completedAtMs", 1000}});
            reply(id, {{"data", data}});
        } else if (method == "thread/start")
            reply(id, {{"thread", thread("t-new", params.value("cwd", ""))}, {"approvalPolicy", "on-request"},
                       {"sandbox", {{"type", "workspaceWrite"}, {"networkAccess", false}}}, {"model", "gpt-test"}});
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

// Kimi Code's `kimi acp`: the daemon starts bin/kimi, which runs this test
// binary as a relay (--fake-kimi) between its stdio and this socket; the
// test plays the agent, one JSON-RPC message per line.
class FakeKimi {
public:
    explicit FakeKimi(const std::string &path)
    {
        listen_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        ::bind(listen_, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        ::listen(listen_, 4);
        thread_ = std::thread([this] { loop(); });
    }
    ~FakeKimi()
    {
        stop_ = true;
        thread_.join();
        ::close(listen_);
    }

    void send(const json &message)
    {
        std::lock_guard lock(mutex_);
        out_ += message.dump() + "\n";
    }
    void update(const std::string &session, const json &update)
    {
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", {{"sessionId", session}, {"update", update}}}});
    }
    void respond(const json &request, const json &result)
    {
        send({{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}});
    }

    // The first message from the daemon since the last take() that matches,
    // waiting up to `ms`; null when none came. It is taken off the list.
    json take(const std::function<bool(const json &)> &pred, int ms = 3000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard lock(mutex_);
                for (auto it = in_.begin(); it != in_.end(); ++it)
                    if (pred(*it)) {
                        json m = *it;
                        in_.erase(it);
                        return m;
                    }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return nullptr;
    }
    json call(const std::string &method, int ms = 3000)
    {
        return take([&](const json &m) { return m.value("method", "") == method; }, ms);
    }
    json answerTo(const json &id, int ms = 3000)
    {
        return take([&](const json &m) { return !m.contains("method") && m.value("id", json()) == id; }, ms);
    }

private:
    void loop()
    {
        std::string buffer;
        while (!stop_) {
            pollfd p[2] = {{listen_, POLLIN, 0}, {client_, POLLIN, 0}};
            ::poll(p, 2, 20);
            if (p[0].revents & POLLIN)
                client_ = ::accept(listen_, nullptr, nullptr);
            if (client_ >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
                char chunk[65536];
                const ssize_t n = ::read(client_, chunk, sizeof chunk);
                if (n <= 0) {
                    ::close(client_);
                    client_ = -1;
                } else {
                    buffer.append(chunk, static_cast<std::size_t>(n));
                    for (auto end = buffer.find('\n'); end != std::string::npos; end = buffer.find('\n')) {
                        json m = json::parse(buffer.substr(0, end), nullptr, false);
                        buffer.erase(0, end + 1);
                        std::lock_guard lock(mutex_);
                        in_.push_back(std::move(m));
                    }
                }
            }
            std::string out;
            {
                std::lock_guard lock(mutex_);
                if (client_ >= 0)
                    out.swap(out_);
            }
            if (!out.empty())
                (void)::write(client_, out.data(), out.size());
        }
        if (client_ >= 0)
            ::close(client_);
    }

    int listen_ = -1;
    int client_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<json> in_;
    std::string out_;
};

// --fake-kimi SOCKET: stdin to the socket, the socket to stdout.
int relay(const char *path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0)
        return 1;
    for (;;) {
        pollfd p[2] = {{STDIN_FILENO, POLLIN, 0}, {fd, POLLIN, 0}};
        if (::poll(p, 2, -1) < 0)
            return 1;
        char chunk[65536];
        for (int i = 0; i < 2; ++i)
            if (p[i].revents & (POLLIN | POLLHUP)) {
                const ssize_t n = ::read(p[i].fd, chunk, sizeof chunk);
                if (n <= 0)
                    return 0;
                if (::write(i == 0 ? fd : STDOUT_FILENO, chunk, static_cast<std::size_t>(n)) != n)
                    return 1;
            }
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--fake-kimi")
        return relay(argv[2]);
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
              "auth) if [ \"$2\" = status ]; then [ -e \"$d/claude.in\" ] && echo '{\"loggedIn\": true}' || echo '{\"loggedIn\": false}';\n"
              "  else echo 'Opening browser to sign in'; echo 'If the browser did not open, visit: https://claude.com/cai/oauth/authorize?code=true&state=s1';\n"
              "  printf 'Paste code here if prompted > '; read code; [ \"$code\" = good#s1 ] || { echo 'Invalid code'; exit 1; }\n"
              "  touch \"$d/claude.in\"; echo 'Login successful'; fi;;\n"
              "*) echo \"$@\" >> \"$d/claude.args\"; exec sleep 30;;\nesac\n",
              true);
    writeFile(bin + "/codex",
              "#!/bin/sh\nd=$(dirname \"$0\")\ncase \"$1\" in\n--version) echo codex-cli 5.5.5;;\n"
              "update) echo checking; echo 'npm ERR! needs root' >&2; exit 1;;\n"
              "login) if [ \"$2\" = status ]; then [ -e \"$d/codex.in\" ] && { echo 'Logged in using ChatGPT'; exit 0; }; echo 'Not logged in'; exit 1; fi\n"
              "  echo '1. Open this link in your browser and sign in to your account'; echo '   https://auth.openai.com/codex/device';\n"
              "  echo '2. Enter this one-time code (expires in 15 minutes)'; echo '   ABCD-12345'; sleep 1; touch \"$d/codex.in\";;\nesac\n",
              true);
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    ::setenv("PATH", (bin + ":" + std::getenv("PATH")).c_str(), 1);
    ::setenv("PVAI_CODEX", (bin + "/codex").c_str(), 1);
    ::setenv("PVAI_CLAUDE", (bin + "/claude").c_str(), 1);
    // An install of claude puts the program back that a test moved away.
    ::setenv("PVAI_INSTALL_CLAUDE",
             ("echo 'pvai-step: download'; echo fetching; sleep 0.3; mv '" + bin + "/claude.away' '" + bin
              + "/claude'; echo 'pvai-step: check'; '" + bin + "/claude' --version")
                 .c_str(),
             1);
    const std::string kimiSocket = home + "/kimi.sock";
    writeFile(bin + "/kimi",
              "#!/bin/sh\ncase \"$1\" in\n--version) echo 2.4.1;;\nacp) exec '" + fs::canonical(argv[0]).string()
                  + "' --fake-kimi '" + kimiSocket + "';;\nesac\n",
              true);
    ::setenv("PVAI_KIMI", (bin + "/kimi").c_str(), 1);
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
    FakeKimi kimi(kimiSocket);

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
    check(assistants.size() == 3 && assistants[0].value("name", "") == "claude" && assistants[0].value("title", "") == "Claude Code"
              && assistants[0].value("models", json()).size() == 4 && assistants[1].value("name", "") == "codex"
              && assistants[2].value("name", "") == "kimi" && assistants[2].value("title", "") == "Kimi Code"
              && assistants[2].contains("loggedIn") && assistants[2]["loggedIn"].is_null()
              && assistants[0].value("installed", false) && assistants[1].value("installed", false) && assistants[2].value("installed", false),
          "welcome lists the assistants as objects: " + assistants.dump());
    check(welcome.value("installed", json()) == json({{"claude", "1.0.0"}, {"codex", "5.5.5"}, {"kimi", "2.4.1"}}),
          "welcome.installed: " + welcome.value("installed", json()).dump());

    // ---- Kimi Code over ACP: the fake agent.
    {
        const json init = kimi.call("initialize");
        check(init.value("params", json()).value("protocolVersion", 0) == 1, "kimi acp is initialized: " + init.dump());
        kimi.respond(init, {{"protocolVersion", 1}, {"agentInfo", {{"name", "Kimi Code CLI"}, {"version", "2.4.1"}}},
                            {"agentCapabilities", {{"loadSession", true}}}});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        a.send({{"type", "start"}, {"ref", 40}, {"assistant", "kimi"}, {"cwd", project}});
        json call = kimi.call("session/new");
        check(call.value("params", json()).value("cwd", "") == project, "start is session/new in the folder");
        kimi.send({{"jsonrpc", "2.0"}, {"id", call["id"]}, {"error", {{"code", -32000}, {"message", "Authentication required"}}}});
        check(a.next("rejected").value("reason", "") == "not-logged-in", "without a login: rejected not-logged-in");
        {
            Client b;
            b.send(app());
            const json again = b.next("welcome").value("assistants", json());
            check(again.size() == 3 && again[2].value("loggedIn", true) == false, "and welcome says loggedIn false");
        }
        a.send({{"type", "start"}, {"ref", 41}, {"assistant", "kimi"}, {"cwd", project}, {"args", {"-m", "kimi-k3"}}});
        call = kimi.call("session/new");
        const json modelOption = {{"id", "model"}, {"currentValue", "kimi-k2"},
                                  {"options", {{{"value", "kimi-k2"}, {"name", "Kimi K2"}}, {{"value", "kimi-k3"}, {"name", "Kimi K3"}}}}};
        kimi.respond(call, {{"sessionId", "k-1"}, {"configOptions", {modelOption}},
                            {"modes", {{"currentModeId", "default"},
                                       {"availableModes", {{{"id", "default"}, {"name", "Default"}}, {{"id", "plan"}, {"name", "Plan"}}}}}}});
        check(a.next("started").value("session", "") == "k-1", "a new Kimi session");
        a.send({{"type", "mode"}, {"ref", 47}, {"session", "k-1"}, {"mode", "plan"}});
        json modeCall = kimi.call("session/set_mode");
        check(modeCall.value("params", json()).value("modeId", "") == "plan", "a mode change is session/set_mode");
        kimi.respond(modeCall, json::object());
        const json modeSet = a.next("mode", [](const json &m) { return m.value("ref", 0) == 47; });
        check(modeSet.value("state", "") == "done" && modeSet.value("mode", "") == "plan", "and done: " + modeSet.dump());
        json set = kimi.call("session/set_config_option");
        check(set.value("params", json()).value("value", "") == "kimi-k3", "-m sets the model option");
        json chosen = modelOption;
        chosen["currentValue"] = "kimi-k3";
        kimi.respond(set, {{"configOptions", {chosen}}});
        const json model = a.next("usage", [](const json &m) { return sub(m, "usage").value("model", json()) == "Kimi K3"; });
        check(!model.empty(), "usage.model by name, after -m");
        json k1;
        a.send({{"type", "subscribe"}, {"session", "k-1"}});
        a.next("history");

        a.send({{"type", "prompt"}, {"ref", 42}, {"session", "k-1"}, {"text", "Hallo"}, {"voice", true}});
        const json prompt = kimi.call("session/prompt");
        const json blocks = prompt.value("params", json()).value("prompt", json());
        check(blocks.size() == 2 && blocks[0].value("text", "") == "\xF0\x9F\x8E\x99 Hallo" && blocks[1].value("type", "") == "resource",
              "session/prompt with the voice context as a resource: " + blocks.dump());
        kimi.update("k-1", {{"sessionUpdate", "tool_call"}, {"toolCallId", "t1"}, {"title", "Run ls"}, {"kind", "execute"},
                            {"status", "pending"}, {"rawInput", {{"command", "ls -la"}}}});
        kimi.send({{"jsonrpc", "2.0"}, {"id", "perm-1"}, {"method", "session/request_permission"},
                   {"params", {{"sessionId", "k-1"},
                               {"toolCall", {{"toolCallId", "t1"}, {"title", "Run ls"}, {"kind", "execute"}, {"rawInput", {{"command", "ls -la"}}}}},
                               {"options", {{{"optionId", "approve_once"}, {"name", "Approve"}, {"kind", "allow_once"}},
                                            {{"optionId", "approve_always"}, {"name", "Approve for this session"}, {"kind", "allow_always"}},
                                            {{"optionId", "reject"}, {"name", "Reject"}, {"kind", "reject_once"}}}}}}});
        const json asked = a.next("attention");
        check(asked.value("kind", "") == "permission" && asked.value("text", "") == "shell: ls -la", "a permission request asks: " + asked.dump());
        k1 = waitSession(a, "k-1", [](const json &s) { return s.value("state", "") == "waiting"; });
        const json info = sub(k1, "dialogInfo");
        check(k1.value("assistant", "") == "kimi" && k1.value("detached", false) && k1.value("stoppable", false)
                  && k1.value("dialog", json()) == "permission" && info.value("tool", "") == "shell" && info.value("text", "") == "ls -la"
                  && info.value("options", json()).size() == 3 && info["options"][1].value("id", "") == "approve_always",
              "dialog permission with the options: " + k1.dump());
        a.send({{"type", "answer"}, {"ref", 43}, {"session", "k-1"}, {"choice", "always"}});
        const json answered = kimi.answerTo("perm-1");
        check(answered.value("result", json()).value("outcome", json()) == json({{"outcome", "selected"}, {"optionId", "approve_always"}}),
              "answer always picks approve_always: " + answered.dump());
        kimi.update("k-1", {{"sessionUpdate", "tool_call_update"}, {"toolCallId", "t1"}, {"status", "completed"},
                            {"content", {{{"type", "content"}, {"content", {{"type", "text"}, {"text", "a.txt"}}}}}}});
        kimi.update("k-1", {{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "Da ist a.txt.\n"}}}});
        kimi.update("k-1", {{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "\xF0\x9F\x94\x8A Eine Datei."}}}});
        kimi.update("k-1", {{"sessionUpdate", "usage_update"}, {"used", 5000}, {"size", 200000}});
        kimi.respond(prompt, {{"stopReason", "end_turn"}});
        const json said = a.next("say", [](const json &m) { return m.value("session", "") == "k-1"; });
        check(said.value("text", "") == "Eine Datei." && said.value("voice", false), "the turn ends and says the 🔊 line: " + said.dump());
        k1 = waitSession(a, "k-1", [](const json &s) { return s.value("state", "") == "idle"; });
        check(sub(sub(k1, "usage"), "context") == json({{"percent", 3}, {"tokens", 5000}, {"size", 200000}}),
              "usage.context from usage_update: " + k1.dump());
        a.send({{"type", "subscribe"}, {"session", "k-1"}});
        const json hist = a.next("history", [](const json &m) { return m.value("session", "") == "k-1"; });
        std::string kinds;
        for (const auto &e : hist.value("entries", json::array()))
            kinds += e.value("kind", "") + ",";
        check(kinds == "prompt,tool,result,text,", "the history: prompt, tool, result, text: " + hist.dump());

        // Earlier sessions, and one played back.
        a.send({{"type", "conversations"}, {"assistant", "kimi"}, {"cwd", project}});
        call = kimi.call("session/list");
        kimi.respond(call, {{"sessions", {{{"sessionId", "k-old"}, {"cwd", project}, {"title", "Alte Frage"}, {"updatedAt", "2026-10-30T10:00:00Z"}},
                                          {{"sessionId", "k-1"}, {"cwd", project}, {"title", "Hallo"}, {"updatedAt", "2026-10-31T10:00:00Z"}}}}});
        const json list = a.next("conversations").value("conversations", json());
        check(list.size() == 1 && list[0].value("id", "") == "k-old" && list[0].value("title", "") == "Alte Frage"
                  && list[0].value("at", 0LL) == 1793354400,
              "conversations from session/list, without the running one: " + list.dump());
        a.send({{"type", "start"}, {"ref", 44}, {"assistant", "kimi"}, {"cwd", project}, {"resume", "k-old"}});
        call = kimi.call("session/load");
        check(call.value("params", json()).value("sessionId", "") == "k-old", "resume is session/load");
        kimi.update("k-old", {{"sessionUpdate", "user_message_chunk"}, {"content", {{"type", "text"}, {"text", "Alte Frage"}}}});
        kimi.update("k-old", {{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "Alte Antwort"}}}});
        kimi.respond(call, {{"configOptions", {modelOption}}});
        check(a.next("started").value("session", "") == "k-old", "the earlier session runs again");
        a.send({{"type", "subscribe"}, {"session", "k-old"}});
        const json old = a.next("history", [](const json &m) { return m.value("session", "") == "k-old"; });
        const json entries = old.value("entries", json::array());
        check(entries.size() == 2 && entries[0].value("kind", "") == "prompt" && entries[0].value("text", "") == "Alte Frage"
                  && entries[1].value("kind", "") == "text" && entries[1].value("text", "") == "Alte Antwort",
              "with its history played back: " + old.dump());
        check(a.next("say", [](const json &m) { return m.value("session", "") == "k-old"; }, 300).empty(), "the playback says nothing");

        a.send({{"type", "stop"}, {"ref", 45}, {"session", "k-old"}});
        check(kimi.call("session/close").value("params", json()).value("sessionId", "") == "k-old", "stop is session/close");
        a.send({{"type", "stop"}, {"ref", 46}, {"session", "k-1"}});
        check(!kimi.call("session/close").is_null(), "for both");
    }
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
    check(tui.value("stoppable", false), "a thread no pvai codex holds can be stopped");

    // A new thread: prompt, turn, spoken answer.
    a.send({{"type", "start"}, {"ref", 2}, {"assistant", "codex"}, {"cwd", project}, {"label", "neu"}, {"args", {"-m", "gpt-test"}}});
    json started = a.next("started");
    check(started.value("session", "") == "t-new", "codex start: " + started.dump());
    check(!codex.call("thread/start", [](const json &p) { return p.value("model", "") == "gpt-test"; }).is_null(), "the model goes to thread/start");
    json fresh = waitSession(a, "t-new", [](const json &s) { return s.value("label", "") == "neu"; });
    check(fresh.value("detached", false), "a started thread is detached and labelled");
    check(fresh.value("mode", "") == "auto" && sub(fresh, "usage").value("model", "") == "gpt-test",
          "on-request with a workspace-write sandbox is Auto, the model from thread/start: " + fresh.dump());
    a.send({{"type", "mode"}, {"ref", 59}, {"session", "t-new"}, {"mode", "never"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 59; }).value("reason", "") == "unknown-mode",
          "the old approval-only modes are gone");
    a.send({{"type", "mode"}, {"ref", 60}, {"session", "t-new"}, {"mode", "full-access"}});
    const json codexMode = a.next("mode", [](const json &m) { return m.value("ref", 0) == 60; });
    check(codexMode.value("state", "") == "done" && codexMode.value("mode", "") == "full-access", "a Codex mode is set: " + codexMode.dump());
    a.send({{"type", "model"}, {"ref", 61}, {"session", "t-new"}, {"model", "gpt-other"}});
    const json codexModel = a.next("model", [](const json &m) { return m.value("ref", 0) == 61; });
    check(codexModel.value("state", "") == "done" && codexModel.value("model", "") == "gpt-other", "a Codex model is set: " + codexModel.dump());
    a.send({{"type", "model"}, {"ref", 62}, {"session", "t-new"}, {"model", "gpt other; rm"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 62; }).value("reason", "") == "bad-model", "a bad model name");
    a.send({{"type", "prompt"}, {"ref", 3}, {"session", "t-new"}, {"text", "hello"}});
    check(a.next("accepted").value("delivery", "") == "typed", "the prompt goes in");
    check(!codex.call("turn/start", [](const json &p) {
              return p.dump().find("hello") != std::string::npos && p.value("approvalPolicy", "") == "never"
                  && p.value("sandboxPolicy", json()) == json({{"type", "dangerFullAccess"}}) && p.value("model", "") == "gpt-other";
          }).is_null(),
          "as turn/start, with approval policy and sandbox of the mode and the model");
    codex.send({{"method", "model/rerouted"},
                {"params", {{"threadId", "t-new"}, {"turnId", "u1"}, {"fromModel", "gpt-other"}, {"toModel", "gpt-small"}, {"reason", "highRiskCyberActivity"}}}});
    const json rerouted = a.next("attention", [](const json &m) { return m.value("kind", "") == "model"; });
    check(rerouted.value("text", "").find("gpt-small") != std::string::npos, "Codex rerouting the model is an attention: " + rerouted.dump());
    check(!waitSession(a, "t-new", [](const json &s) { return sub(s, "usage").value("model", "") == "gpt-small"; }).empty(), "and usage.model follows");
    codex.send({{"method", "thread/settings/updated"},
                {"params", {{"threadId", "t-new"}, {"threadSettings", {{"approvalPolicy", "on-request"}, {"sandboxPolicy", {{"type", "readOnly"}}}, {"model", "gpt-small"}}}}}});
    check(!waitSession(a, "t-new", [](const json &s) { return s.value("mode", "") == "read-only"; }).empty(), "settings updates move the mode");
    codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u1"}}}}}});
    codex.send({{"method", "item/completed"},
                {"params", {{"threadId", "t-new"}, {"item", {{"type", "agentMessage"}, {"id", "i2"}, {"text", "Answer.\n\xF0\x9F\x94\x8A Spoken."}}}}}});
    codex.send({{"method", "turn/completed"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u1"}, {"status", "completed"}}}}}});
    json say = a.next("say", [](const json &m) { return m.value("session", "") == "t-new"; });
    check(say.value("text", "") == "Spoken.", "the turn's 🔊 line is said: " + say.dump());

    // A subagent thread of it: listed while active, gone when idle.
    codex.kidActive = true;
    codex.send({{"method", "thread/started"}, {"params", {{"thread", codex.kid()}}}});
    auto agents = [](const json &s) {
        return s.contains("subagents") && s["subagents"].is_array() ? s["subagents"] : json(nullptr);
    };
    fresh = waitSession(a, "t-new", [&](const json &s) { return agents(s).size() == 1; });
    check(!fresh.empty() && agents(fresh)[0] == json({{"id", "t-kid"}, {"type", "explorer"}, {"description", "Look around the repo"},
                                                       {"name", "Hume"}, {"background", true}, {"startedAt", 1793000000}}),
          "an active subagent thread is listed under its parent: " + fresh.dump());
    check(waitSession(a, "t-kid", [](const json &) { return true; }, 500).empty(), "and is no session of its own");
    codex.kidActive = false;
    codex.send({{"method", "thread/status/changed"}, {"params", {{"threadId", "t-kid"}, {"status", {{"type", "idle"}}}}}});
    check(!waitSession(a, "t-new", [&](const json &s) { return agents(s) == json::array(); }).empty(), "an idle one drops out");

    // An approval, answered from the app; then an interrupt.
    codex.send({{"id", 99}, {"method", "item/commandExecution/requestApproval"}, {"params", {{"threadId", "t-new"}, {"command", "ls"}}}});
    json attention = a.next("attention");
    check(attention.value("kind", "") == "permission" && attention.value("text", "") == "ls", "an approval asks the owner");
    check(!waitSession(a, "t-new", [](const json &s) {
              return s.value("dialogInfo", json()) == json({{"tool", "shell"}, {"text", "ls"}, {"message", nullptr}});
          }).empty(), "dialogInfo names the command");
    a.send({{"type", "answer"}, {"ref", 4}, {"session", "t-new"}, {"choice", "allow"}});
    check(!a.next("answered").empty(), "answered");
    check(!codex.received([](const json &m) { return m.value("id", json()) == 99 && m.value("result", json()).value("decision", "") == "accept"; }).is_null(),
          "allow is accept");
    codex.send({{"method", "item/started"},
                {"params", {{"threadId", "t-new"}, {"item", {{"type", "fileChange"}, {"id", "f1"}, {"changes", {{{"path", "/tmp/a.txt"}}, {{"path", "/tmp/b.txt"}}}}}}}}});
    codex.send({{"id", 98}, {"method", "item/fileChange/requestApproval"},
                {"params", {{"threadId", "t-new"}, {"itemId", "f1"}, {"reason", "needs write access"}}}});
    check(!waitSession(a, "t-new", [](const json &s) {
              return s.value("dialogInfo", json()) == json({{"tool", "edit"}, {"text", "/tmp/a.txt\n/tmp/b.txt"}, {"message", "needs write access"}});
          }).empty(), "a patch's dialogInfo names its files");
    a.send({{"type", "answer"}, {"ref", 41}, {"session", "t-new"}, {"choice", "allow"}});
    check(!a.next("answered").empty(), "answered too");
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

    // ---- A TUI started by pvai codex: its threads leave when it ends.
    {
        Client tui;
        tui.send({{"type", "attach"}, {"assistant", "codex"}, {"cwd", "/tmp"}, {"resume", "t-tui"}, {"pid", 1}});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        codex.send({{"method", "thread/started"}, {"params", {{"thread", {{"id", "t-tui2"}, {"parentThreadId", nullptr}}}}}});
        check(!waitSession(a, "t-tui2", [](const json &s) { return !s.value("detached", true) && !s.value("stoppable", true); }).empty(),
              "a new thread in the TUI's folder is the TUI's");
        a.send({{"type", "stop"}, {"ref", 1}, {"session", "t-tui"}});
        check(a.next("rejected").value("reason", "") == "not-detached", "a thread a TUI holds cannot be stopped");
    }
    json gone = a.next("sessions", [](const json &m) { return sessionIn(m, "t-tui").empty() && sessionIn(m, "t-tui2").empty(); });
    check(!gone.empty(), "the TUI ended: its resumed and its new thread leave the list");
    check(!sessionIn(gone, "t-arch").empty(), "a detached thread in the same folder stays");
    check(!codex.call("thread/unsubscribe", [](const json &p) { return p.value("threadId", "") == "t-tui"; }).empty()
              && !codex.call("thread/unsubscribe", [](const json &p) { return p.value("threadId", "") == "t-tui2"; }).is_null(),
          "both are unsubscribed");
    check(a.next("sessions", [](const json &m) { return !sessionIn(m, "t-tui").empty(); }, 17000).empty(),
          "and not taken on again while still loaded (one rescan later)");

    // Another client's thread that no TUI holds: stop lets go of it.
    codex.send({{"method", "thread/started"}, {"params", {{"thread", {{"id", "t-free"}, {"parentThreadId", nullptr}}}}}});
    check(!waitSession(a, "t-free", [](const json &s) { return s.value("stoppable", false) && !s.value("detached", true); }).empty(),
          "a thread no TUI holds is stoppable");
    a.send({{"type", "stop"}, {"ref", 9}, {"session", "t-free"}});
    check(!a.next("stopping").empty(), "and stops");
    check(!codex.call("thread/unsubscribe", [](const json &p) { return p.value("threadId", "") == "t-free"; }).is_null(), "by unsubscribing");
    check(codex.call("thread/archive", [](const json &p) { return p.value("threadId", "") == "t-free"; }, 300).is_null(),
          "not by archiving");

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
    hook("wterm", {{"hook_event_name", "PermissionRequest"}, {"tool_name", "Bash"},
                   {"tool_input", {{"command", "git push"}, {"description", "Push the branch"}}}});
    json asked = a.next("attention");
    check(asked.value("kind", "") == "permission" && asked.value("text", "") == "Bash: git push",
          "PermissionRequest asks the owner: " + asked.dump());
    hook("wterm", {{"hook_event_name", "Notification"}, {"notification_type", "permission_prompt"}, {"message", "Claude needs your permission"}});
    check(!waitSession(a, "wterm", [](const json &s) {
              return s.value("dialog", "") == "permission"
                  && s.value("dialogInfo", json()) == json({{"tool", "Bash"}, {"text", "git push"}, {"description", "Push the branch"},
                                                             {"message", "Claude needs your permission"}});
          }).empty(), "the Notification after it adds its words to dialogInfo");
    check(a.next("attention", {}, 500).empty(), "and asks no second time");
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

    // A dialog no hook reports, read off the screen by the wrapper.
    const std::string teach = "Teach auto mode about your environment?\n\xE2\x9D\xAF 1. Yes\n  2. No\n\n"
                              "\xE2\x86\x90/\xE2\x86\x92 to change \xC2\xB7 Enter to continue \xC2\xB7 Esc to cancel";
    wrapper.send({{"type", "screen"}, {"dialog", teach}});
    const json asks = a.next("attention");
    check(asks.value("kind", "") == "screen" && asks.value("text", "") == "Teach auto mode about your environment?",
          "a screen dialog asks the owner with its first line: " + asks.dump());
    term = waitSession(a, "wterm", [](const json &s) { return s.value("dialog", json()) == "screen"; });
    check(term.value("state", "") == "waiting" && sub(term, "dialogInfo").value("text", "") == teach,
          "a screen dialog at the prompt: waiting, its text in dialogInfo: " + term.dump());
    a.send({{"type", "prompt"}, {"ref", 15}, {"session", "wterm"}, {"text", "three"}});
    check(a.next("accepted", [](const json &m) { return m.value("ref", 0) == 15; }).value("delivery", "") == "queued",
          "a prompt waits while it is open");
    a.send({{"type", "answer"}, {"ref", 16}, {"session", "wterm"}, {"choice", "key:down"}});
    check(wrapper.next("keys").value("data", "") == "\x1b[B", "key:down is the arrow");
    a.send({{"type", "answer"}, {"ref", 17}, {"session", "wterm"}, {"choice", "key:2"}});
    check(wrapper.next("keys").value("data", "") == "2", "key:2 the digit");
    a.send({{"type", "answer"}, {"ref", 18}, {"session", "wterm"}, {"choice", "key:f12"}});
    check(a.next("rejected").value("reason", "") == "bad-choice", "an unknown key is a bad choice");
    a.send({{"type", "answer"}, {"ref", 19}, {"session", "wterm"}, {"choice", "allow"}});
    check(wrapper.next("keys").value("data", "") == "\r", "allow is Return");
    check(waitSession(a, "wterm", [](const json &s) { return s.value("dialog", json()) != "screen"; }, 600).empty(),
          "and the dialog stays until the screen says it is gone");
    wrapper.send({{"type", "screen"}, {"dialog", nullptr}});
    check(wrapper.next("type").value("text", "") == "three", "then it is idle and the queued prompt goes in");
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "three"}});
    hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "Ok."}});
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle again");

    // A Claude Code started from a shell command of this one inherits pvai's
    // environment: its hooks and status line are not this session's.
    hook("wterm", {{"hook_event_name", "SessionStart"}, {"session_id", "nested-9"}, {"source", "startup"}});
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "nested-9"}, {"prompt", "x"}});
    {
        Client status;
        status.send({{"type", "status"}, {"id", "wterm"}, {"status", {{"session_id", "nested-9"}, {"version", "9.9.9"}}}});
    }
    check(waitSession(a, "wterm", [](const json &s) {
              return s.value("state", "") != "idle" || s.value("claudeSession", "") != "running-1"
                  || sub(s, "usage").value("version", "") == "9.9.9";
          }, 800).empty(), "a nested Claude Code's hooks and status line change nothing");

    // A wrapper that verifies (0.5.15): a prompt counts once Claude Code
    // confirms it; one that did not go in waits in front of the queue.
    {
        Client ver;
        ver.send({{"type", "register"}, {"id", "wver"}, {"cwd", project}, {"label", "ver"}, {"restart", true}, {"verify", true}});
        hook("wver", {{"hook_event_name", "SessionStart"}, {"session_id", "v-1"}, {"source", "startup"}});
        check(!waitSession(a, "wver", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a verifying wrapper's session");
        a.send({{"type", "prompt"}, {"ref", 30}, {"session", "wver"}, {"text", "eins"}});
        check(a.next("accepted", [](const json &m) { return m.value("ref", 0) == 30; }).value("delivery", "") == "queued",
              "accepted says queued until Claude Code confirms");
        json typing = ver.next("type");
        check(typing.value("text", "") == "eins" && typing.value("verify", false), "it is typed with verify");
        ver.send({{"type", "typed"}, {"ok", false}, {"screen", "Something new\n\xE2\x9D\xAF 1. Yes"}});
        check(a.next("attention").value("text", "") == "Something new", "the owner is asked with the first line");
        json blocked = waitSession(a, "wver", [](const json &s) { return s.value("dialog", json()) == "screen"; });
        check(blocked.value("state", "") == "waiting" && sub(blocked, "dialogInfo").value("text", "") == "Something new\n\xE2\x9D\xAF 1. Yes"
                  && blocked.value("queued", 0) == 1,
              "not typed: waiting on the screen, the prompt queued: " + blocked.dump());
        ver.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        typing = ver.next("type");
        check(typing.value("text", "") == "eins" && typing.value("verify", false), "a free prompt box: it is typed again");
        ver.send({{"type", "typed"}, {"ok", true}});
        check(a.next("delivered", {}, 500).empty(), "typed is not delivered yet");
        hook("wver", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "v-1"}, {"prompt", "eins"}});
        const json delivered = a.next("delivered");
        check(delivered.value("session", "") == "wver" && delivered.value("ref", 0) == 30, "UserPromptSubmit delivers it: " + delivered.dump());
        hook("wver", {{"hook_event_name", "Stop"}, {"session_id", "v-1"}, {"last_assistant_message", "Eins."}});
        check(!waitSession(a, "wver", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "the turn ends");

        // Typed, but no turn (no login, an unknown dialog took the Return).
        a.send({{"type", "prompt"}, {"ref", 31}, {"session", "wver"}, {"text", "zwei"}});
        check(ver.next("type").value("text", "") == "zwei", "the next prompt is typed");
        ver.send({{"type", "typed"}, {"ok", true}});
        check(!ver.next("snapshot", {}, 17000).empty(), "no UserPromptSubmit in time: the daemon asks for the screen");
        ver.send({{"type", "snapshot"}, {"screen", "zwei\nNot logged in"}});
        blocked = waitSession(a, "wver", [](const json &s) { return s.value("dialog", json()) == "screen"; });
        check(sub(blocked, "dialogInfo").value("text", "") == "zwei\nNot logged in" && blocked.value("queued", 0) == 1,
              "the owner sees the screen, the prompt waits: " + blocked.dump());
        ver.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        check(ver.next("type", {}, 800).empty(), "a free prompt alone does not type it again");
        a.send({{"type", "answer"}, {"ref", 32}, {"session", "wver"}, {"choice", "key:esc"}});
        check(ver.next("keys").value("data", "") == "\x1b", "the owner answers");
        typing = ver.next("type");
        check(typing.value("text", "") == "zwei" && typing.value("verify", false), "then it goes in again");
        ver.send({{"type", "typed"}, {"ok", false}, {"screen", "?"}});
        check(!waitSession(a, "wver", [](const json &s) { return s.value("dialog", json()) == "screen"; }).empty(), "blocked once more");
        a.send({{"type", "answer"}, {"ref", 33}, {"session", "wver"}, {"choice", "send"}});
        typing = ver.next("type");
        check(typing.value("text", "") == "zwei" && !typing.value("verify", true), "answer send types it without the check");
        hook("wver", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "v-1"}, {"prompt", "zwei"}});
        check(a.next("delivered").value("ref", 0) == 31, "and UserPromptSubmit delivers it");
        hook("wver", {{"hook_event_name", "Stop"}, {"session_id", "v-1"}, {"last_assistant_message", "Zwei."}});
        // While its wrapper is still there: closing `ver` ends the session.
        a.send({{"type", "clearInput"}, {"ref", 40}, {"session", "wver"}});
        const json noClear = a.next("rejected", [](const json &m) { return m.value("ref", 0) == 40; });
        check(noClear.value("reason", "") == "not-supported", "an older wrapper cannot clear: " + noClear.dump());
    }

    // clear (0.5.30): empties the prompt box, a draft or a command left in
    // shell mode, and the queue goes on.
    {
        Client clr;
        clr.send({{"type", "register"}, {"id", "wclr"}, {"cwd", project}, {"label", "clr"}, {"restart", true}, {"verify", true}, {"clear", true}});
        hook("wclr", {{"hook_event_name", "SessionStart"}, {"session_id", "c-1"}, {"source", "startup"}});
        json idle = waitSession(a, "wclr", [](const json &s) { return s.value("state", "") == "idle"; });
        check(idle.value("inputClearable", false), "an idle session under a new wrapper is clearable: " + idle.dump());
        clr.send({{"type", "draft"}, {"dirty", true}}); // the box in shell mode
        check(!waitSession(a, "wclr", [](const json &s) { return s.value("draft", false); }).empty(), "a shell box counts as a draft");
        a.send({{"type", "prompt"}, {"ref", 41}, {"session", "wclr"}, {"text", "drei"}});
        check(clr.next("type", {}, 800).empty(), "nothing is typed into it");
        a.send({{"type", "clearInput"}, {"ref", 42}, {"session", "wclr"}});
        check(!a.next("inputCleared", [](const json &m) { return m.value("ref", 0) == 42; }).empty(), "clear is acknowledged");
        check(!clr.next("clear").empty(), "the wrapper is asked to clear the box");
        clr.send({{"type", "draft"}, {"dirty", false}});
        json typing = clr.next("type");
        check(typing.value("text", "") == "drei", "an empty box: the prompt goes in");
        a.send({{"type", "clearInput"}, {"ref", 43}, {"session", "wclr"}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 43; }).value("reason", "") == "busy",
              "not while a prompt is going in");
        clr.send({{"type", "typed"}, {"ok", false}, {"screen", "!  echo old"}});
        json blocked = waitSession(a, "wclr", [](const json &s) { return s.value("dialog", json()) == "screen"; });
        check(blocked.value("inputClearable", false), "blocked by the box: clearable: " + blocked.dump());
        a.send({{"type", "clearInput"}, {"ref", 44}, {"session", "wclr"}});
        check(!clr.next("clear").empty(), "clear while blocked");
        clr.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        check(clr.next("type").value("text", "") == "drei", "the box is free again: it goes in");
        clr.send({{"type", "typed"}, {"ok", true}});
        hook("wclr", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "c-1"}, {"prompt", "drei"}});
        check(a.next("delivered").value("ref", 0) == 41, "and is delivered");
        hook("wclr", {{"hook_event_name", "Stop"}, {"session_id", "c-1"}, {"last_assistant_message", "Drei."}});
    }

    // AskUserQuestion (0.5.33): the questions with their choices; option,
    // next and text answer them, the review comes last.
    {
        Client qw;
        qw.send({{"type", "register"}, {"id", "wq"}, {"cwd", project}, {"label", "q"}, {"restart", true}, {"verify", true}, {"clear", true}, {"paced", true}});
        hook("wq", {{"hook_event_name", "SessionStart"}, {"session_id", "q-1"}, {"source", "startup"}});
        check(!waitSession(a, "wq", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a question session");
        hook("wq", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "q-1"}, {"prompt", "frag"}});
        const json questions = json::array(
            {{{"question", "Welche Früchte?"}, {"header", "Früchte"}, {"multiSelect", true},
              {"options", json::array({{{"label", "Apfel"}, {"description", "rot"}}, {{"label", "Kiwi"}, {"description", "grün"}}})}},
             {{"question", "Welche Stadt?"}, {"header", "Stadt"}, {"multiSelect", false},
              {"options", json::array({{{"label", "Bern"}, {"description", ""}}, {{"label", "Zürich"}, {"description", ""}}})}}});
        hook("wq", {{"hook_event_name", "PermissionRequest"}, {"session_id", "q-1"}, {"tool_name", "AskUserQuestion"},
                    {"tool_input", {{"questions", questions}}}});
        json asked = waitSession(a, "wq", [](const json &s) { return s.value("dialog", json()) == "question"; });
        const json info = sub(asked, "dialogInfo");
        check(info.value("current", -1) == 0 && info["questions"].size() == 2 && info["questions"][0]["options"][1].value("label", "") == "Kiwi"
                  && info["questions"][0]["selected"] == json::array() && info["questions"][1].value("multiSelect", true) == false,
              "dialogInfo.questions with their choices: " + info.dump());
        auto answer = [&](int ref, const std::string &choice) {
            a.send({{"type", "answer"}, {"ref", ref}, {"session", "wq"}, {"choice", choice}});
            for (int i = 0; i < 50; ++i) {
                const json m = a.next(2000);
                if (m.empty())
                    break;
                if (m.is_object() && m.value("ref", json()) == json(ref))
                    return m.value("type", "") == "rejected" ? m.value("reason", "") : m.value("type", "");
            }
            return std::string("none");
        };
        auto wrapperKeys = [](Client &w) { return w.next("keys").value("data", ""); };
        check(answer(60, "option:2") == "answered" && wrapperKeys(qw) == "2", "option:2 is the digit");
        check(!waitSession(a, "wq", [](const json &s) { return sub(s, "dialogInfo")["questions"][0]["selected"] == json::array({2}); }).empty(),
              "a multi-select choice is toggled on");
        check(answer(61, "text:frei") == "bad-choice", "no typed answer to a multi-select question");
        check(answer(62, "option:3") == "bad-choice", "no third choice");
        check(answer(63, "next") == "answered" && wrapperKeys(qw) == "\t", "next is Tab");
        check(!waitSession(a, "wq", [](const json &s) { return sub(s, "dialogInfo").value("current", -1) == 1; }).empty(), "on to the second question");
        check(answer(64, "text:Luzern am See") == "answered", "a typed answer");
        const json paced = qw.next("paced");
        check(paced["keys"] == json::array({"3", "\x1b[200~Luzern am See\x1b[201~", "\r"}), "paced: the field, the words, Return: " + paced.dump());
        check(!waitSession(a, "wq", [](const json &s) { return sub(s, "dialogInfo").value("current", -1) == 2 && s.value("state", "") == "waiting"; }).empty(),
              "then the review");
        check(answer(65, "option:1") == "bad-choice", "the review takes allow or deny");
        check(answer(66, "allow") == "answered" && wrapperKeys(qw) == "\r", "allow submits");
        hook("wq", {{"hook_event_name", "Stop"}, {"session_id", "q-1"}, {"last_assistant_message", "Ok."}});
        check(!waitSession(a, "wq", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "the turn ends");
    }

    // A wrapper that registers again after a daemon restart: busy when its
    // transcript has a turn going on, idle when the last turn ended.
    {
        const std::string runFile = home + "/elsewhere/mid-turn.jsonl", doneFile = home + "/elsewhere/done.jsonl";
        const std::string prompt = R"({"type":"user","message":{"role":"user","content":"do it"}})";
        const std::string answer = R"({"type":"assistant","message":{"content":[{"type":"text","text":"On it."}]}})";
        const std::string end = R"({"type":"system","subtype":"turn_duration","durationMs":5})";
        writeFile(runFile, end + "\n" + prompt + "\n" + answer + "\n");
        writeFile(doneFile, prompt + "\n" + answer + "\n" + end + "\n");
        writeFile(runtime + "/pvai/wmid.claude", json({{"claudeSession", "mid-1"}, {"transcript", runFile}}).dump() + "\n");
        writeFile(runtime + "/pvai/wdone.claude", json({{"claudeSession", "done-1"}, {"transcript", doneFile}}).dump() + "\n");
        Client mid, done;
        mid.send({{"type", "register"}, {"id", "wmid"}, {"cwd", project}, {"label", "mid"}, {"running", true}, {"restart", true}});
        done.send({{"type", "register"}, {"id", "wdone"}, {"cwd", project}, {"label", "done"}, {"running", true}, {"restart", true}});
        check(!waitSession(a, "wmid", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
              "mid-turn after a daemon restart: busy");
        check(!waitSession(a, "wdone", [](const json &s) { return s.value("state", "") == "idle"; }).empty(),
              "after the turn's end: idle");
    }

    // The permission mode: from the hooks and the footer; a change is
    // Shift+Tab until the footer shows it, once round the cycle at most.
    {
        Client mw;
        mw.send({{"type", "register"}, {"id", "wmode"}, {"cwd", project}, {"label", "mode"}, {"restart", true}, {"verify", true},
                 {"modes", true}});
        hook("wmode", {{"hook_event_name", "SessionStart"}, {"session_id", "m-1"}, {"source", "startup"}, {"permission_mode", "default"}});
        check(!waitSession(a, "wmode", [](const json &s) { return s.value("mode", json()) == "default"; }).empty(),
              "the mode from the hook");
        a.send({{"type", "mode"}, {"ref", 50}, {"session", "wmode"}, {"mode", "plan"}});
        check(mw.next("keys").value("data", "") == "\x1b[Z", "a change presses Shift+Tab");
        mw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"mode", "acceptEdits"}});
        check(mw.next("keys").value("data", "") == "\x1b[Z", "not there yet: again");
        mw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"mode", "plan"}});
        json event = a.next("mode", [](const json &m) { return m.value("ref", 0) == 50; });
        check(event.value("state", "") == "done" && event.value("mode", "") == "plan" && event.value("session", "") == "wmode",
              "then done: " + event.dump());
        a.send({{"type", "mode"}, {"ref", 51}, {"session", "wmode"}, {"mode", "bypassPermissions"}});
        for (const char *m : {"default", "acceptEdits", "plan"}) {
            check(mw.next("keys").value("data", "") == "\x1b[Z", std::string("pressing on towards bypass, at ") + m);
            mw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"mode", m}});
        }
        event = a.next("mode", [](const json &m) { return m.value("ref", 0) == 51; });
        check(event.value("state", "") == "failed" && event.value("error", "") == "unavailable",
              "a mode not in the cycle: unavailable after one round: " + event.dump());
        a.send({{"type", "mode"}, {"ref", 52}, {"session", "wmode"}, {"mode", "fast"}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 52; }).value("reason", "") == "unknown-mode",
              "an unknown mode is refused");
        Client old;
        old.send({{"type", "register"}, {"id", "wold"}, {"cwd", project}, {"label", "old"}, {"restart", true}, {"verify", true}});
        hook("wold", {{"hook_event_name", "SessionStart"}, {"session_id", "o-1"}, {"source", "startup"}});
        check(!waitSession(a, "wold", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "an older wrapper");
        a.send({{"type", "mode"}, {"ref", 53}, {"session", "wold"}, {"mode", "plan"}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 53; }).value("reason", "") == "not-supported",
              "a wrapper that does not report the footer: not-supported");
    }

    // Claude Code not on this host: start is refused, welcome says so.
    fs::rename(bin + "/claude", bin + "/claude.away");
    a.send({{"type", "start"}, {"ref", 34}, {"assistant", "claude"}, {"cwd", project}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 34; }).value("reason", "") == "not-installed",
          "start without claude installed: not-installed");
    {
        Client b;
        b.send(app());
        const json list = b.next("welcome").value("assistants", json());
        check(!list.empty() && list[0].value("name", "") == "claude" && !list[0].value("installed", true),
              "welcome lists claude with installed false: " + list.dump());
    }
    // Installed from the app: the install's steps, then the new state.
    a.send({{"type", "install"}, {"ref", 36}, {"assistant", "claude"}});
    json step = a.next("install", [](const json &m) { return m.value("step", json()) == "download"; });
    check(step.value("assistant", "") == "claude" && step.value("ref", 0) == 36 && step.value("state", "") == "running",
          "an install reports its steps: " + step.dump());
    const json installed = a.next("install", [](const json &m) { return m.value("state", "") != "running"; }, 5000);
    check(installed.value("state", "") == "done", "and ends done: " + installed.dump());
    const json nowInstalled = a.next("assistants", [](const json &m) {
        const json list = m.value("assistants", json::array());
        return !list.empty() && list[0].value("installed", false);
    });
    check(!nowInstalled.empty(), "assistants says claude is installed now");
    a.send({{"type", "install"}, {"ref", 37}, {"assistant", "claude"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 37; }).value("reason", "") == "already-installed",
          "a second install: already-installed");

    // Logins: Codex's device code; Claude Code's link and the code back.
    a.send({{"type", "login"}, {"ref", 38}, {"assistant", "codex"}});
    json login = a.next("login", [](const json &m) { return m.value("assistant", "") == "codex" && !m.value("code", json()).is_null(); });
    check(login.value("url", "") == "https://auth.openai.com/codex/device" && login.value("code", "") == "ABCD-12345"
              && login.value("state", "") == "waiting",
          "codex: the device link and code: " + login.dump());
    login = a.next("login", [](const json &m) { return m.value("assistant", "") == "codex" && m.value("state", "") != "waiting"; }, 5000);
    check(login.value("state", "") == "done", "and done once it went through: " + login.dump());
    check(!a.next("assistants", [](const json &m) {
              for (const auto &x : m.value("assistants", json::array()))
                  if (x.value("name", "") == "codex")
                      return x.value("loggedIn", json()) == true;
              return false;
          }).empty(),
          "codex loggedIn true from codex login status");
    a.send({{"type", "login"}, {"ref", 39}, {"assistant", "claude"}});
    login = a.next("login", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "code-needed"; });
    check(login.value("url", "").rfind("https://claude.com/cai/oauth/authorize?", 0) == 0, "claude: the link, and it wants the code: " + login.dump());
    a.send({{"type", "login-code"}, {"ref", 40}, {"assistant", "claude"}, {"code", "good#s1"}});
    login = a.next("login", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "done"; }, 5000);
    check(!login.empty(), "the code goes in, the login is done");
    check(!a.next("assistants", [](const json &m) {
              const json list = m.value("assistants", json::array());
              return !list.empty() && list[0].value("loggedIn", json()) == true;
          }).empty(),
          "claude loggedIn true from claude auth status");
    a.send({{"type", "login-code"}, {"ref", 41}, {"assistant", "claude"}, {"code", "x"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 41; }).value("reason", "") == "no-login",
          "a code without a login: no-login");

    // A session the app started that ends at once: the app hears why.
    {
        Client early;
        early.send({{"type", "register"}, {"id", "wearly"}, {"cwd", project}, {"label", "early"}, {"detached", true}, {"restart", true}});
        check(!waitSession(a, "wearly", [](const json &) { return true; }).empty(), "a detached session registers");
        early.send({{"type", "exited"}, {"status", 1}, {"screen", "line 1\nNot logged in \xC2\xB7 Please run /login"}});
        const json ended = a.next("attention", [](const json &m) { return m.value("session", "") == "wearly"; });
        check(ended.value("kind", "") == "ended" && ended.value("text", "") == "line 1\nNot logged in \xC2\xB7 Please run /login",
              "an early end is an attention with the last lines: " + ended.dump());
    }
    {
        Client stopped;
        stopped.send({{"type", "register"}, {"id", "wstop"}, {"cwd", project}, {"label", "stop"}, {"detached", true}, {"restart", true}});
        check(!waitSession(a, "wstop", [](const json &) { return true; }).empty(), "another detached session");
        a.send({{"type", "stop"}, {"ref", 35}, {"session", "wstop"}});
        check(stopped.next("terminate").value("type", "") == "terminate", "stop terminates it");
        stopped.send({{"type", "exited"}, {"status", 129}, {"screen", "bye"}});
        check(a.next("attention", [](const json &m) { return m.value("session", "") == "wstop"; }, 600).empty(),
              "a stopped session's end says nothing");
    }

    // Subagents: running while their meta file has no `finished`; a task
    // notification starts a turn without UserPromptSubmit.
    const std::string run2 = home + "/elsewhere/run-2.jsonl";
    writeFile(run2, "");
    hook("wterm", {{"hook_event_name", "Notification"}, {"notification_type", "idle_prompt"}, {"transcript_path", run2}});
    const std::string agentMeta = home + "/elsewhere/run-2/subagents/agent-a1b2.meta.json";
    writeFile(home + "/elsewhere/run-2/subagents/agent-a1b2.jsonl", "{}\n");
    writeFile(agentMeta, R"({"agentType":"build-runner","description":"Build it","requestShape":"background","spawnDepth":1})");
    term = waitSession(a, "wterm", [&](const json &s) { return agents(s).size() == 1; }, 4000);
    const json agent = term.empty() ? json::object() : agents(term)[0];
    check(agent.value("id", "") == "a1b2" && agent.value("type", "") == "build-runner" && agent.value("description", "") == "Build it"
              && agent.value("background", false) && agent.value("startedAt", 0LL) > 1700000000,
          "a running subagent is listed: " + term.dump());
    auto append = [&](const std::string &line) { std::ofstream(run2, std::ios::app) << line << "\n"; };
    const std::string notification =
        R"({"type":"attachment","isSidechain":false,"attachment":{"type":"queued_command","prompt":"<task-notification>\n<status>completed</status>\n</task-notification>"}})";
    const std::string answerLine =
        R"({"type":"assistant","uuid":"u-a1","timestamp":"2026-10-31T12:00:05.250Z","message":{"content":[{"type":"text","text":"Noted."}],)"
        R"("usage":{"cache_creation":{"ephemeral_1h_input_tokens":343,"ephemeral_5m_input_tokens":0}}}})";
    append(notification);
    check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "busy"; }, 1200).empty(),
          "a task notification alone starts no turn");
    append(answerLine);
    term = waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "busy"; }, 3000);
    check(!term.empty(), "Claude answering it does: the idle session is busy");
    check(sub(term, "usage").value("cache", json()) == json({{"ttl", 3600}, {"at", 1793448005}}),
          "usage.cache from the last answer: " + term.dump());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeFile(agentMeta, R"({"agentType":"build-runner","description":"Build it","requestShape":"background","finished":true})");
    check(!waitSession(a, "wterm", [&](const json &s) { return agents(s) == json::array(); }, 4000).empty(),
          "a finished subagent leaves the list");
    hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "Stop ends that turn");
    // A notification and its answer inside a turn, read only after its Stop:
    // no turn of its own.
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "go"}});
    append(notification);
    append(answerLine);
    hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle after Stop");
    check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "busy"; }, 1200).empty(),
          "and stays idle: the notification belonged to the turn");
    // The same with Claude's answer written only after the Stop arrived.
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "again"}});
    append(notification);
    hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
    append(answerLine);
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle after that Stop");
    check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "busy"; }, 1200).empty(),
          "an answer that lands after the Stop starts no turn either");
    // An older Claude Code without PermissionRequest: the last tool call in
    // the transcript is what the permission is about.
    {
        std::ofstream(run2, std::ios::app)
            << R"({"type":"assistant","uuid":"u-t1","message":{"content":[{"type":"tool_use","id":"tu1","name":"Bash","input":{"command":"make"}}]}})" << "\n";
    }
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "build"}});
    hook("wterm", {{"hook_event_name", "Notification"}, {"notification_type", "permission_prompt"}, {"message", "Claude needs your permission to use Bash"}});
    check(!waitSession(a, "wterm", [](const json &s) {
              return s.value("dialogInfo", json()) == json({{"tool", "Bash"}, {"text", "make"}, {"message", "Claude needs your permission to use Bash"}});
          }).empty(), "without PermissionRequest, dialogInfo comes from the transcript");
    hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
    check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle" && s.value("dialogInfo", json()).is_null(); }).empty(),
          "dialogInfo goes with the dialog");

    // Ended Claude conversations: titles, no command-only ones, not the
    // running one.
    a.send({{"type", "conversations"}, {"assistant", "claude"}, {"cwd", project}});
    list = a.next("conversations");
    const json found = list.value("conversations", json());
    check(found.size() == 1 && found[0].value("id", "") == "c0ffee00-1111-2222-3333-444455556666"
              && found[0].value("title", "") == "Erste Frage hier" && found[0].value("at", 0LL) > 0,
          "claude conversations: " + found.dump());
    check(welcome.value("forget", false) && welcome.value("model", false), "welcome.forget and welcome.model");
    // forget: deletes an ended conversation and what lies beside it.
    writeFile(transcripts + "/feedface-2222-3333-4444-555566667777.jsonl", userLine("Weg damit"));
    writeFile(transcripts + "/feedface-2222-3333-4444-555566667777/subagents/agent-1.jsonl", userLine("sub"));
    a.send({{"type", "forget"}, {"ref", 50}, {"assistant", "claude"}, {"cwd", project}, {"id", "feedface-2222-3333-4444-555566667777"}});
    json forgotten = a.next("forgotten");
    check(forgotten.value("ref", 0) == 50 && forgotten.value("id", "") == "feedface-2222-3333-4444-555566667777", "forgotten: " + forgotten.dump());
    check(!fs::exists(transcripts + "/feedface-2222-3333-4444-555566667777.jsonl") && !fs::exists(transcripts + "/feedface-2222-3333-4444-555566667777"),
          "the transcript and its folder are gone");
    a.send({{"type", "forget"}, {"ref", 51}, {"assistant", "claude"}, {"cwd", project}, {"id", "running-1"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 51; }).value("reason", "") == "running", "not a running one");
    check(fs::exists(transcripts + "/running-1.jsonl"), "which stays");
    a.send({{"type", "forget"}, {"ref", 52}, {"assistant", "claude"}, {"cwd", project}, {"id", "../../etc/passwd"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 52; }).value("reason", "") == "unknown-conversation", "a bad id");
    a.send({{"type", "forget"}, {"ref", 53}, {"assistant", "codex"}, {"cwd", project}, {"id", "t-arch"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 53; }).value("reason", "") == "not-supported", "Codex: not yet");
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
    // The fake claude must have written its arguments before the restart
    // ends it (macOS starts it slower than the hook below arrives).
    for (int i = 0; i < 50 && readFile(bin + "/claude.args").empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    check(updated.value("installed", json()) == json({{"claude", "2.0.0"}, {"codex", "5.5.5"}, {"kimi", "2.4.1"}}),
          "installed after the update");
    a.send({{"type", "start"}, {"ref", 21}, {"assistant", "gemini"}, {"cwd", project}});
    check(a.next("rejected").value("reason", "") == "unknown-assistant", "an unknown assistant is refused");
    a.send({{"type", "conversations"}, {"assistant", "gemini"}, {"cwd", project}});
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
    check(written.is_object() && written.value("model", "") == "opus" && sub(written, "hooks").size() == 5
              && sub(written, "statusLine").value("command", "") == g_pvai + " statusline --chain 'my-line'",
          "the rest of the file stays, the own status line is chained: " + written.dump());
    out = shell(g_pvai + " run -- claude --version");
    check(out.find("hooks and status line") == std::string::npos, "a second run leaves the file alone: " + out);
    check(shell(g_pvai + " install-hooks").find("in place") != std::string::npos, "install-hooks says they are in place");
    out = shell(g_pvai + " uninstall-hooks");
    const json removed = json::parse(readFile(settings), nullptr, false);
    check(removed == json::parse(R"({"model": "opus", "statusLine": {"type": "command", "command": "my-line"}})"),
          "uninstall-hooks removes them and restores the chained status line: " + out + removed.dump());
    check(shell(g_pvai + " uninstall-hooks").find("no pvai hooks") != std::string::npos,
          "a second uninstall-hooks leaves the file alone");

    ::kill(daemon, SIGTERM);
    ::waitpid(daemon, nullptr, 0);
    fs::remove_all(home);
    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
