// The session lifecycle against a real daemon: Claude sessions through a
// fake wrapper and fake hooks, Codex sessions through a fake app server, and
// for both versions, updates, ended conversations and resume. `claude` and
// `codex` are small scripts here, so nothing real starts.
//
// usage: pvai-session-test /path/to/pvai
#include "support.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

using namespace pvai;
using namespace pvai::test;
namespace fs = std::filesystem;

namespace {

std::string g_pvai;

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

json app()
{
    return {{"type", "hello"}, {"client", "test-app"}, {"version", "0"}};
}

void hook(const std::string &id, json event)
{
    LineClient c;
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
json waitSession(LineClient &client, const std::string &id, const std::function<bool(const json &)> &pred, int ms = 3000)
{
    const json found = client.next("sessions", [&](const json &m) {
        const json s = sessionIn(m, id);
        return !s.empty() && pred(s);
    }, ms);
    return sessionIn(found, id);
}

// Everything the app hears for `ms`: whether the session was ever listed as
// waiting or with a dialog, and the kinds of attention raised for it.
struct Observed {
    bool shown = false;
    std::vector<std::string> attention;
    bool has(const std::string &kind) const { return std::find(attention.begin(), attention.end(), kind) != attention.end(); }
};
Observed observe(LineClient &client, const std::string &id, int ms)
{
    Observed seen;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        const json m = client.next(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()));
        if (!m.is_object() || m.empty())
            break;
        if (m.value("type", "") == "attention" && m.value("session", "") == id)
            seen.attention.push_back(m.value("kind", ""));
        const json entry = sessionIn(m, id);
        if (!entry.empty() && (entry.value("state", "") == "waiting" || !entry.value("dialog", json()).is_null()))
            seen.shown = true;
    }
    return seen;
}

// ---- A fake Codex app server: one WebSocket client on its control socket.

class FakeCodex : public FakeServer {
public:
    explicit FakeCodex(const std::string &path) : FakeServer(path) { start(); }
    ~FakeCodex() override { stop(); }

    // The first message from the daemon with that method (or a response with
    // that id) since the start, waiting up to 3 s; null when none came.
    json received(const std::function<bool(const json &)> &pred, int ms = 3000) { return take(pred, ms, false); }
    json call(const std::string &method, const std::function<bool(const json &)> &params = {}, int ms = 3000)
    {
        return received([&](const json &m) { return m.value("method", "") == method && (!params || params(m.value("params", json()))); }, ms);
    }

    std::atomic<int> threadListCalls{0};
    std::atomic<int> rejectTurn{0}; // turn/start answers an error: 1 a model the account may not use, 2 a login problem
    std::atomic<bool> rejectStart{false}; // thread/start answers an error
    std::atomic<bool> hasAccount{false};  // account/read answers a ChatGPT account
    std::atomic<bool> kidActive{false}; // the subagent thread t-kid of t-new (found by the first rescan: idle until a test says so)
    json kid() const
    {
        return {{"id", "t-kid"}, {"parentThreadId", "t-new"}, {"agentRole", "explorer"}, {"agentNickname", "Hume"},
                {"preview", "Look around the repo"}, {"createdAt", 1793000000},
                {"status", {{"type", kidActive ? "active" : "idle"}, {"activeFlags", json::array()}}}};
    }

protected:
    // The WebSocket upgrade.
    void onAccept(int fd) override
    {
        std::string request;
        char c;
        while (request.find("\r\n\r\n") == std::string::npos && ::read(fd, &c, 1) == 1)
            request += c;
        const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        (void)::write(fd, reply.data(), reply.size());
    }

    std::string encode(const json &message) override
    {
        const std::string payload = message.dump();
        std::string f(1, static_cast<char>(0x81));
        if (payload.size() < 126) {
            f += static_cast<char>(payload.size());
        } else {
            f += static_cast<char>(126);
            f += static_cast<char>((payload.size() >> 8) & 0xff);
            f += static_cast<char>(payload.size() & 0xff);
        }
        f += payload;
        return f;
    }

    void onData(const std::string &chunk) override
    {
        in_raw_.append(chunk);
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
            push(message);
            onMessage(message);
        }
    }

    void reply(const json &id, const json &result) { writeNow({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }

    // What the app server would say to each call.
    void onMessage(const json &m) override
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
        else if (method == "account/read")
            reply(id, hasAccount ? json{{"account", {{"type", "chatgpt"}, {"email", "codex.user@example.com"}, {"planType", "plus"}}}, {"requiresOpenaiAuth", true}}
                                 : json{{"account", nullptr}, {"requiresOpenaiAuth", true}});
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
                writeNow({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", "no such thread"}}}});
        } else if (method == "thread/items/list") {
            json data = json::array();
            if (params.value("threadId", "") == "t-tui")
                data.push_back({{"item", {{"type", "agentMessage"}, {"id", "i1"}, {"text", "earlier answer"}}}, {"completedAtMs", 1000}});
            reply(id, {{"data", data}});
        } else if (method == "turn/start" && rejectTurn > 0) {
            if (rejectTurn == 1)
                writeNow({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600},
                          {"message", "The 'gpt-6.1-sol' model is not supported when using Codex with a ChatGPT account."}}}});
            else
                writeNow({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", "unexpected status 401 Unauthorized: Missing bearer"},
                          {"data", {{"codexErrorInfo", {{"httpConnectionFailed", {{"httpStatusCode", 401}}}}}}}}}});
        } else if (method == "thread/start" && rejectStart)
            writeNow({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", "thread/start refused"}}}});
        else if (method == "thread/start")
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

    std::string in_raw_;
};

} // namespace

// Kimi Code's `kimi acp`: the daemon starts bin/kimi, which runs this test
// binary as a relay (--fake-kimi) between its stdio and this socket; the
// test plays the agent, one JSON-RPC message per line.
class FakeKimi : public FakeServer {
public:
    explicit FakeKimi(const std::string &path) : FakeServer(path, true) { start(); }
    ~FakeKimi() override { stop(); }

    void update(const std::string &session, const json &update)
    {
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", {{"sessionId", session}, {"update", update}}}});
    }

    json call(const std::string &method, int ms = 3000)
    {
        return take([&](const json &m) { return m.value("method", "") == method; }, ms);
    }
    json answerTo(const json &id, int ms = 3000)
    {
        return take([&](const json &m) { return !m.contains("method") && m.value("id", json()) == id; }, ms);
    }
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
    const TestHome testHome;
    const std::string home = testHome.path;
    const std::string configHome = home + "/.config";
    const std::string runtime = testHome.runtime;
    const std::string bin = home + "/bin";
    // claude: a version that `update` raises, and a session that records
    // its arguments and waits. codex: an update that fails.
    writeFile(bin + "/claude.version", "1.0.0\n");
    writeFile(bin + "/claude", R"SH(#!/bin/sh
d=$(dirname "$0")
t="${XDG_CONFIG_HOME:-$HOME/.config}/pvai/claude-token"
if [ -e "$t" ]; then echo "after $*"; else echo "before $*"; fi >> "$d/claude.calls"
case "$1" in
--version) echo "$(cat "$d/claude.version") (Claude Code)";;
update) echo 2.0.0 > "$d/claude.version"; echo updated;;
setup-token)
  stty size > "$d/setup-token.size" 2>/dev/null
  m=$(cat "$d/setup-token.mode"); u=$(cat "$d/setup-token.url"); k=$(cat "$d/setup-token.tok")
  echo 'Opening browser to sign in'
  if [ "$m" = wrapped ]; then printf 'If the browser did not open, visit: %s\n' "$u" | fold -w 80
  else printf 'If the browser did not open, visit: %s\n' "$u"; fi
  printf 'Paste code here if prompted > '; read code; echo
  case "$m" in
  ok) echo 'Your OAuth token (valid for 1 year):'; echo "$k"; echo 'Store this token securely.';;
  wrapped) echo 'Your OAuth token (valid for 1 year):'; echo "$k" | fold -w 80; echo; echo 'Store this token securely.';;
  fail) echo "$k"; exit 1;;
  *) echo 'Long-lived token created.';;
  esac;;
auth) if [ "$2" = status ]; then
    echo "${CLAUDE_CODE_OAUTH_TOKEN-unset}" >> "$d/claude.statusenv"
    if [ -n "$CLAUDE_CODE_OAUTH_TOKEN" ]; then echo '{"loggedIn": true, "authMethod": "oauth_token"}'
    elif [ -e "$d/claude.in" ]; then echo '{"loggedIn": true, "authMethod": "claude.ai", "email": "claude.user@example.com"}'
    else echo '{"loggedIn": false}'; fi
  elif [ "$2" = logout ]; then
    if [ -e "$d/logout.fail" ]; then echo 'logout failed'; exit 1; fi
    sleep 1; rm -f "$d/claude.in"; echo 'Logged out'
  elif [ -e "$d/login.linkonly" ]; then echo 'If the browser did not open, visit: https://claude.com/cai/oauth/authorize?code=true&state=linkonly'; sleep 2; exit 1
  else echo 'Opening browser to sign in'; echo 'If the browser did not open, visit: https://claude.com/cai/oauth/authorize?code=true&state=s1'
    printf 'Paste code here if prompted > '; read code; [ "$code" = good#s1 ] || { echo 'Invalid code'; exit 1; }
    touch "$d/claude.in"; echo 'Login successful'; fi;;
*) echo "$@" >> "$d/claude.args"; echo "${CLAUDE_CODE_OAUTH_TOKEN-unset}" >> "$d/claude.env"; exec sleep 30;;
esac
)SH",
              true);
    writeFile(bin + "/codex",
              "#!/bin/sh\nd=$(dirname \"$0\")\ncase \"$1\" in\n--version) echo codex-cli 5.5.5;;\n"
              "update) echo checking; echo 'npm ERR! needs root' >&2; exit 1;;\n"
              "app-server) echo \"$*\" >> \"$d/codex.calls\";;\n"
              "login) if [ \"$2\" = status ]; then [ -e \"$d/codex.in\" ] && { echo 'Logged in using ChatGPT'; exit 0; }; echo 'Not logged in'; exit 1; fi\n"
              "  echo '1. Open this link in your browser and sign in to your account'; echo '   https://auth.openai.com/codex/device';\n"
              "  echo '2. Enter this one-time code (expires in 15 minutes)'; echo '   ABCD-12345'; sleep 1; touch \"$d/codex.in\";;\n"
              "logout) rm -f \"$d/codex.in\"; echo 'Logged out';;\nesac\n",
              true);
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
    ::setenv("PVAI_QUIET_DIV", "40", 1); // the screen-quiet times of the daemon: 10 s -> 0.25 s ...
    ::unsetenv(kEnvClaudeToken); // the daemon must not inherit a login from the machine running this test
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

    DaemonProcess daemon;
    // The daemon logs to stderr: into a file for the token checks below.
    const std::string daemonLog = home + "/daemon.log";
    {
        const int saved = ::dup(STDERR_FILENO);
        const int logFd = ::open(daemonLog.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (logFd >= 0)
            ::dup2(logFd, STDERR_FILENO);
        ::close(logFd);
        daemon.start(g_pvai, appPort, 1200, true);
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
    }

    // A field of the wrong JSON type is one peer's error, not the daemon's end.
    {
        LineClient bad;
        bad.send({{"type", 1}});
        const json answer = bad.next("error");
        check(answer.value("code", "") == "bad-message", "a non-string type is answered with bad-message: " + answer.dump());
        LineClient typed;
        typed.send({{"type", "hello"}, {"client", 5}});
        check(typed.next("error").value("code", "") == "bad-message", "a wrong-typed field in a known message too");
        LineClient ok;
        ok.send(app());
        check(ok.next("welcome").value("type", "") == "welcome", "the daemon still answers after wrong-typed messages");
    }
    LineClient a;
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
            LineClient b;
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

        // An error with a null code and message still ends the turn.
        a.send({{"type", "prompt"}, {"ref", 48}, {"session", "k-1"}, {"text", "Nochmal"}});
        const json again = kimi.call("session/prompt");
        check(!again.is_null(), "a second Kimi prompt");
        kimi.send({{"jsonrpc", "2.0"}, {"id", again["id"]}, {"error", {{"code", nullptr}, {"message", nullptr}}}});
        const json failedSay = a.next("say", [](const json &m) { return m.value("session", "") == "k-1"; });
        check(failedSay.value("text", "") == "Kimi Code: the turn failed", "null code and message: the failure is said as for Codex: " + failedSay.dump());
        check(!waitSession(a, "k-1", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "and the session is idle again");
        // A cancelled turn: the interrupted entry's id is stable (named after the prompt's).
        a.send({{"type", "prompt"}, {"ref", 49}, {"session", "k-1"}, {"text", "Abbrechen"}});
        kimi.respond(kimi.call("session/prompt"), {{"stopReason", "cancelled"}});
        check(!waitSession(a, "k-1", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "kimi: idle after a cancelled turn");
        for (int i = 0; i < 2; ++i) { // twice: the same ids both times
            a.send({{"type", "subscribe"}, {"session", "k-1"}});
            const json again2 = a.next("history", [](const json &m) { return m.value("session", "") == "k-1"; });
            std::string promptId, interruptedId;
            for (const auto &e : again2.value("entries", json::array())) {
                if (e.value("kind", "") == "prompt" && e.value("text", "") == "Abbrechen")
                    promptId = e.value("id", "");
                if (e.value("kind", "") == "interrupted")
                    interruptedId = e.value("id", "");
            }
            check(!promptId.empty() && interruptedId == promptId + ":interrupted", "kimi: the interrupted entry's id is the prompt's: " + interruptedId);
        }
        a.drain();

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

        // A rename is kept for the assistant's own sessions and wins when one is rebuilt.
        a.send({{"type", "rename"}, {"ref", 90}, {"session", "k-old"}, {"label", "  Mein Name  "}});
        const json kimiRenamed = a.next("renamed", [](const json &m) { return m.value("ref", 0) == 90; });
        check(kimiRenamed.value("session", "") == "k-old" && kimiRenamed.value("label", "") == "Mein Name", "kimi: renamed, trimmed: " + kimiRenamed.dump());
        check(json::parse(readFile(configHome + "/pvai/labels.json"), nullptr, false).value("kimi/k-old", "") == "Mein Name",
              "kimi: the rename is in labels.json");
        a.send({{"type", "stop"}, {"ref", 91}, {"session", "k-old"}});
        check(kimi.call("session/close").value("params", json()).value("sessionId", "") == "k-old", "kimi: closed");
        a.send({{"type", "start"}, {"ref", 92}, {"assistant", "kimi"}, {"cwd", project}, {"resume", "k-old"}, {"label", "Anders"}});
        call = kimi.call("session/load");
        kimi.respond(call, {{"configOptions", {modelOption}}});
        check(a.next("started").value("session", "") == "k-old", "kimi: played back again");
        check(!waitSession(a, "k-old", [](const json &s) { return s.value("label", "") == "Mein Name"; }).empty(),
              "kimi: the kept rename wins over the start request's label");
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

    // Not logged in (`codex login status` said so): the start is refused before anything goes to the app server;
    // after the login it goes through, and the account's name (account/read, an email) is in assistants.
    {
        auto codexEntry = [](const json &message) {
            for (const auto &x : message.value("assistants", json::array()))
                if (x.value("name", "") == "codex")
                    return x;
            return json::object();
        };
        for (int i = 0; i < 40; ++i) {
            LineClient probe;
            probe.send(app());
            if (codexEntry(probe.next("welcome")).value("loggedIn", json()) == false)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        a.send({{"type", "start"}, {"ref", 1100}, {"assistant", "codex"}, {"cwd", project}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 1100; }).value("reason", "") == "not-logged-in",
              "codex without a login: rejected not-logged-in");
        check(codex.call("thread/start", {}, 300).is_null(), "and no thread/start went to the app server");
        codex.hasAccount = true;
        a.send({{"type", "login"}, {"ref", 1101}, {"assistant", "codex"}});
        check(!a.next("login", [](const json &m) { return m.value("assistant", "") == "codex" && m.value("state", "") == "done"; }, 6000).empty(),
              "the codex login goes through");
        const json named = a.next("assistants", [&](const json &m) {
            const json c = codexEntry(m);
            return c.value("loggedIn", json()) == true && c.value("account", json()) == "codex.user@example.com";
        }, 6000);
        check(!named.empty(), "assistants: codex loggedIn true, account the email from account/read");
        check(!codexEntry(named).dump().empty() && codexEntry(named).dump().find("token") == std::string::npos, "and no token in it");
    }

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
    check(say.value("text", "") == "Spoken." && say.value("voice", false), "the turn's 🔊 line is said, voice=true though the prompt was typed: " + say.dump());

    // Failed turns are visible: a text entry, `say`, and for a login problem the "login" attention, once per incident.
    {
        a.drain();
        const std::string modelText = "Codex: The 'gpt-6.1-sol' model is not supported when using Codex with a ChatGPT account.";
        auto textsOf = [&]() {
            a.send({{"type", "subscribe"}, {"session", "t-new"}});
            const json h = a.next("history", [](const json &m) { return m.value("session", "") == "t-new"; });
            std::vector<std::string> out;
            for (const auto &e : h.value("entries", json::array()))
                if (e.value("kind", "") == "text" || e.value("kind", "") == "prompt")
                    out.push_back(e.value("kind", "") + ":" + e.value("text", ""));
            return out;
        };
        auto count = [&](const std::vector<std::string> &all, const std::string &what) { return std::count(all.begin(), all.end(), what); };
        auto prompt = [&](int ref, const std::string &text) {
            a.send({{"type", "prompt"}, {"ref", ref}, {"session", "t-new"}, {"text", text}});
            return !a.next("accepted", [&](const json &m) { return m.value("ref", 0) == ref; }).empty();
        };
        // Everything the app hears about t-new for `ms`: the says and the login attentions among it.
        struct Heard {
            std::vector<std::string> says;
            int logins = 0;
            std::string state; // the last state of the session in a `sessions` list
        };
        auto gather = [&](int ms) {
            Heard heard;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            for (;;) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
                if (left <= 0)
                    return heard;
                const json m = a.next(static_cast<int>(left));
                if (m.empty())
                    return heard;
                if (m.value("type", "") == "sessions") {
                    if (const json s = sessionIn(m, "t-new"); !s.empty())
                        heard.state = s.value("state", "");
                    continue;
                }
                if (m.value("session", "") != "t-new")
                    continue;
                if (m.value("type", "") == "say")
                    heard.says.push_back(m.value("text", ""));
                else if (m.value("type", "") == "attention" && m.value("kind", "") == "login")
                    ++heard.logins;
            }
        };
        const std::string loginText = "Codex is not logged in.";

        // A turn/start the app server refuses: the prompt and the reason are in the history, an ordinary error.
        codex.rejectTurn = 1;
        check(prompt(300, "model refused"), "a prompt goes to turn/start");
        Heard heard = gather(1200);
        check(heard.says == std::vector<std::string>{modelText}, "a rejected turn/start is said, whole: " + (heard.says.empty() ? "" : heard.says[0]));
        check(heard.logins == 0, "a model the account may not use is no login problem");
        check(heard.state == "idle", "and the session is idle again, not stuck: " + heard.state);
        auto texts = textsOf();
        check(count(texts, "prompt:model refused") == 1 && count(texts, "text:" + modelText) == 1, "prompt and reason are in the history");
        codex.rejectTurn = 0;

        // turn/completed with an error and no answer: the same, one entry.
        check(prompt(301, "fails at the end"), "next prompt");
        codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u20"}}}}}});
        codex.send({{"method", "turn/completed"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u20"}, {"status", "failed"},
                    {"error", {{"message", "The 'gpt-6.1-sol' model is not supported when using Codex with a ChatGPT account."},
                               {"codexErrorInfo", "modelNotFound"}}}}}}}});
        heard = gather(1200);
        check(heard.says == std::vector<std::string>{modelText} && heard.logins == 0, "turn/completed with an error: said, not a login problem");
        check(heard.state == "idle", "and idle: " + heard.state);
        texts = textsOf();
        check(count(texts, "text:" + modelText) == 2, "one more entry in the history");

        // An `error` that is a retry says nothing; the last one ends the turn (no turn/completed follows), a
        // login problem: entry, say, attention. The turn/completed that may follow adds nothing.
        const json authInfo = {{"httpConnectionFailed", {{"httpStatusCode", 401}}}};
        check(prompt(302, "login lost"), "next prompt");
        codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u21"}}}}}});
        codex.send({{"method", "error"}, {"params", {{"threadId", "t-new"}, {"turnId", "u21"}, {"willRetry", true},
                    {"error", {{"message", "Reconnecting... 2/5"}, {"codexErrorInfo", {{"responseStreamDisconnected", {{"httpStatusCode", 401}}}}}}}}}});
        heard = gather(500);
        check(heard.says.empty() && heard.logins == 0, "a retrying error says nothing");
        codex.send({{"method", "error"}, {"params", {{"threadId", "t-new"}, {"turnId", "u21"}, {"willRetry", false},
                    {"error", {{"message", "unexpected status 401 Unauthorized: Missing bearer"}, {"codexErrorInfo", authInfo}}}}}});
        heard = gather(1000);
        check(heard.says == std::vector<std::string>{loginText} && heard.logins == 1, "the last error is said, and it is the login attention (a 401)");
        check(heard.state == "idle", "the turn is over without a turn/completed: " + heard.state);
        codex.send({{"method", "turn/completed"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u21"}, {"status", "failed"},
                    {"error", {{"message", "unexpected status 401 Unauthorized: Missing bearer"}, {"codexErrorInfo", authInfo}}}}}}}});
        heard = gather(600);
        check(heard.says.empty() && heard.logins == 0, "the turn/completed after it adds no second say");
        texts = textsOf();
        check(count(texts, "text:" + loginText) == 1, "nor a second entry");

        // The same incident again: no second attention. A turn that works re-arms it.
        check(prompt(303, "still lost"), "again");
        codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u22"}}}}}});
        codex.send({{"method", "turn/completed"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u22"}, {"status", "failed"},
                    {"error", {{"message", "401"}, {"codexErrorInfo", "unauthorized"}}}}}}}});
        heard = gather(1000);
        check(heard.says == std::vector<std::string>{loginText} && heard.logins == 0, "the same incident: said again, no second attention");
        check(heard.state == "idle", "idle: " + heard.state);
        check(prompt(304, "works"), "a working turn");
        codex.send({{"method", "turn/started"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u23"}}}}}});
        codex.send({{"method", "item/completed"}, {"params", {{"threadId", "t-new"}, {"item", {{"type", "agentMessage"}, {"id", "i23"}, {"text", "Fine."}}}}}});
        codex.send({{"method", "turn/completed"}, {"params", {{"threadId", "t-new"}, {"turn", {{"id", "u23"}, {"status", "completed"}}}}}});
        heard = gather(800);
        check(heard.says == std::vector<std::string>{"Fine."} && heard.state == "idle", "it is said as usual: " + heard.state);
        codex.rejectTurn = 2;
        check(prompt(305, "lost again"), "and the login is lost again");
        heard = gather(1000);
        check(heard.logins == 1 && heard.says == std::vector<std::string>{loginText}, "a rejected turn/start with a 401: login attention again, after the working turn");
        check(heard.state == "idle", "idle: " + heard.state);
        codex.rejectTurn = 0;
        a.drain();
    }

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
    // Several approvals at once: shown one at a time, none lost; a request
    // nobody here can answer gets an error reply at once.
    codex.send({{"id", 97}, {"method", "item/commandExecution/requestApproval"}, {"params", {{"threadId", "t-new"}, {"command", "pwd"}}}});
    check(a.next("attention").value("text", "") == "pwd", "a second approval round: the first is shown");
    codex.send({{"id", 96}, {"method", "item/commandExecution/requestApproval"}, {"params", {{"threadId", "t-new"}, {"command", "whoami"}}}});
    codex.send({{"id", 95}, {"method", "item/commandExecution/requestApproval"}, {"params", {{"threadId", "t-new"}, {"command", "date"}}}});
    codex.send({{"id", 94}, {"method", "item/tool/call"}, {"params", {{"threadId", "t-new"}, {"tool", "x"}}}});
    check(!codex.received([](const json &m) { return m.value("id", json()) == 94 && m.value("error", json()).value("code", 0) == -32601; }).is_null(),
          "a request of an unknown method is answered with -32601");
    check(codex.received([](const json &m) { return m.value("id", json()) == 96 && m.contains("result"); }, 400).is_null(),
          "a waiting approval is not answered yet");
    a.send({{"type", "answer"}, {"ref", 40}, {"session", "t-new"}, {"choice", "allow"}});
    check(a.next("attention").value("text", "") == "whoami", "the next waiting one comes up when the first is answered");
    check(!codex.received([](const json &m) { return m.value("id", json()) == 97 && m.value("result", json()).value("decision", "") == "accept"; }).is_null(),
          "the first is answered accept");
    codex.send({{"method", "serverRequest/resolved"}, {"params", {{"threadId", "t-new"}, {"requestId", 95}}}});
    a.send({{"type", "answer"}, {"ref", 42}, {"session", "t-new"}, {"choice", "allow"}});
    check(!codex.received([](const json &m) { return m.value("id", json()) == 96 && m.value("result", json()).value("decision", "") == "accept"; }).is_null(),
          "the second is answered accept");
    check(!waitSession(a, "t-new", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
          "one resolved elsewhere while it waited is gone: the session is busy again");
    a.drain();
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
        LineClient tui;
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
    a.send({{"type", "rename"}, {"ref", 93}, {"session", "t-free"}, {"label", "Frei"}});
    check(a.next("renamed", [](const json &m) { return m.value("ref", 0) == 93; }).value("label", "") == "Frei", "codex: renamed");
    a.send({{"type", "stop"}, {"ref", 9}, {"session", "t-free"}});
    check(!a.next("stopping").empty(), "and stops");
    check(!codex.call("thread/unsubscribe", [](const json &p) { return p.value("threadId", "") == "t-free"; }).is_null(), "by unsubscribing");
    check(codex.call("thread/archive", [](const json &p) { return p.value("threadId", "") == "t-free"; }, 300).is_null(),
          "not by archiving");
    // The thread comes back (a rebuilt session): the kept name is on it again.
    a.next("sessions", [](const json &m) { return sessionIn(m, "t-free").empty(); });
    a.send({{"type", "start"}, {"ref", 94}, {"assistant", "codex"}, {"cwd", project}, {"resume", "t-free"}, {"label", "Anders"}});
    check(a.next("started", [](const json &m) { return m.value("ref", 0) == 94; }).value("session", "") == "t-free", "codex: resumed again");
    check(!waitSession(a, "t-free", [](const json &s) { return s.value("label", "") == "Frei"; }).empty(),
          "codex: a recreated session carries the kept rename");
    check(json::parse(readFile(configHome + "/pvai/labels.json"), nullptr, false).value("codex/t-free", "") == "Frei",
          "codex: the rename is in labels.json");

    // ---- Claude in a terminal: a fake wrapper and hooks.
    LineClient wrapper;
    wrapper.send({{"type", "register"}, {"version", kVersion}, {"id", "wterm"}, {"cwd", project}, {"label", "term"}});
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
    check(say.value("text", "") == "Fertig." && say.value("voice", false), "a typed turn with a 🔊 line: it is said, voice=true");
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
        LineClient status;
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
    check(asks.value("kind", "") == "screen" && asks.value("text", "").rfind("Teach auto mode about your environment?\n", 0) == 0,
          "a screen dialog asks the owner with its first lines: " + asks.dump());
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

    // A model switch waits for what Claude Code answers to /model.
    {
        const std::string running = transcripts + "/running-1.jsonl";
        auto append = [&](const std::string &text) { std::ofstream(running, std::ios::app) << text; };
        hook("wterm", {{"hook_event_name", "SessionStart"}, {"session_id", "running-1"}, {"source", "resume"}, {"transcript_path", running}});
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle with a transcript");
        a.send({{"type", "model"}, {"ref", 70}, {"session", "wterm"}, {"model", "fable"}});
        check(wrapper.next("type").value("text", "") == "/model fable", "a Claude Code model switch types /model");
        a.send({{"type", "model"}, {"ref", 71}, {"session", "wterm"}, {"model", "opus"}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 71; }).value("reason", "") == "busy",
              "a second switch while one is pending is busy");
        check(a.next("model", [](const json &m) { return m.value("ref", 0) == 70; }, 700).empty(), "no answer before Claude Code answers");
        append(userLine("<command-name>/model</command-name>") + userLine("<local-command-stdout>Kept model as \x1b[1mOpus 5.5\x1b[22m</local-command-stdout>"));
        const json kept = a.next("model", [](const json &m) { return m.value("ref", 0) == 70; }, 3000);
        check(kept.value("state", "") == "failed" && kept.value("error", "") == "Kept model as Opus 5.5" && kept.value("model", "") == "fable",
              "Kept model as ... is failed with Claude Code's words: " + kept.dump());
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle after the command");
        a.send({{"type", "model"}, {"ref", 72}, {"session", "wterm"}, {"model", "opus"}});
        check(wrapper.next("type").value("text", "") == "/model opus", "the next switch goes in");
        append(userLine("<command-name>/model</command-name>") + userLine("<local-command-stdout>Set model to Opus 5.5</local-command-stdout>"));
        const json set = a.next("model", [](const json &m) { return m.value("ref", 0) == 72; }, 3000);
        check(set.value("state", "") == "done" && set.value("model", "") == "opus", "Set model to ... is done: " + set.dump());
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle again after the switch");
        // Queued behind a running turn: that turn's Stop does not end the request.
        a.send({{"type", "prompt"}, {"ref", 73}, {"session", "wterm"}, {"text", "long"}});
        check(wrapper.next("type").value("text", "") == "long", "a prompt is typed");
        hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "long"}});
        a.send({{"type", "model"}, {"ref", 74}, {"session", "wterm"}, {"model", "sonnet"}});
        check(a.next("model", [](const json &m) { return m.value("ref", 0) == 74; }, 500).empty(), "no answer while it waits in the queue");
        hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "Ok."}});
        check(wrapper.next("type").value("text", "") == "/model sonnet", "after the Stop the /model goes in");
        check(a.next("model", [](const json &m) { return m.value("ref", 0) == 74; }, 500).empty(), "and the Stop did not fail it");
        append(userLine("<command-name>/model</command-name>") + userLine("<local-command-stdout>Set model to Sonnet</local-command-stdout>"));
        const json later = a.next("model", [](const json &m) { return m.value("ref", 0) == 74; }, 3000);
        check(later.value("state", "") == "done" && later.value("model", "") == "sonnet", "a queued switch is done by its output: " + later.dump());
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle after the queued switch");
    }

    // A /model that shows no output and no dialog fails after 30 s, and the session is free.
    {
        a.send({{"type", "model"}, {"ref", 75}, {"session", "wterm"}, {"model", "opus"}});
        check(wrapper.next("type").value("text", "") == "/model opus", "a /model goes in for the timeout test");
        const json timedOut = a.next("model", [](const json &m) { return m.value("ref", 0) == 75; }, 33000);
        check(timedOut.value("state", "") == "failed" && timedOut.value("error", "") == "no answer",
              "a /model with no output fails after 30 s: " + timedOut.dump());
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle after the timeout");
    }

    // A typed "/model <name>" ends like the app's switch: on Claude Code's output.
    {
        a.send({{"type", "prompt"}, {"ref", 77}, {"session", "wterm"}, {"text", "/model nonesuch"}});
        check(wrapper.next("type").value("text", "") == "/model nonesuch", "a typed /model goes in");
        std::ofstream(transcripts + "/running-1.jsonl", std::ios::app)
            << userLine("<command-name>/model</command-name>") + userLine("<local-command-stdout>Model 'nonesuch' not found</local-command-stdout>");
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 3000).empty(),
              "idle after a typed /model's output");
    }

    // A wrapper that reports no version (before 0.5.52) takes no model switch: it is stale.
    {
        LineClient old;
        old.send({{"type", "register"}, {"id", "wold"}, {"cwd", project}, {"label", "old"}});
        check(!waitSession(a, "wold", [](const json &) { return true; }).empty(), "an unversioned wrapper is a session");
        a.send({{"type", "model"}, {"ref", 76}, {"session", "wold"}, {"model", "opus"}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 76; }).value("reason", "") == "stale-wrapper",
              "a model switch through an unversioned wrapper is stale-wrapper");
    }

    // A Claude Code started from a shell command of this one inherits pvai's
    // environment: its hooks and status line are not this session's.
    hook("wterm", {{"hook_event_name", "SessionStart"}, {"session_id", "nested-9"}, {"source", "startup"}});
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "nested-9"}, {"prompt", "x"}});
    {
        LineClient status;
        status.send({{"type", "status"}, {"id", "wterm"}, {"status", {{"session_id", "nested-9"}, {"version", "9.9.9"}}}});
    }
    check(waitSession(a, "wterm", [](const json &s) {
              return s.value("state", "") != "idle" || s.value("claudeSession", "") != "running-1"
                  || sub(s, "usage").value("version", "") == "9.9.9";
          }, 800).empty(), "a nested Claude Code's hooks and status line change nothing");

    // A wrapper that verifies (0.5.15): a prompt counts once Claude Code
    // confirms it; one that did not go in waits in front of the queue.
    {
        LineClient ver;
        ver.send({{"type", "register"}, {"version", kVersion}, {"id", "wver"}, {"cwd", project}, {"label", "ver"}, {"restart", true}, {"verify", true}});
        hook("wver", {{"hook_event_name", "SessionStart"}, {"session_id", "v-1"}, {"source", "startup"}});
        check(!waitSession(a, "wver", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a verifying wrapper's session");
        a.send({{"type", "prompt"}, {"ref", 30}, {"session", "wver"}, {"text", "eins"}});
        check(a.next("accepted", [](const json &m) { return m.value("ref", 0) == 30; }).value("delivery", "") == "queued",
              "accepted says queued until Claude Code confirms");
        json typing = ver.next("type");
        check(typing.value("text", "") == "eins" && typing.value("verify", false), "it is typed with verify");
        ver.send({{"type", "screen"}, {"dialog", "Something new\n\xE2\x9D\xAF 1. Yes"}, {"prompt", false}}); // a real dialog stands
        ver.send({{"type", "typed"}, {"ok", false}, {"screen", "Something new\n\xE2\x9D\xAF 1. Yes"}});
        check(a.next("attention").value("text", "") == "Something new\n\xE2\x9D\xAF 1. Yes", "the owner is asked with the screen's lines");
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
        ver.send({{"type", "screen"}, {"dialog", "?"}, {"prompt", false}});
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

    // 0.5.60: a paste the check did not see in time stays in the box; the
    // wrapper empties it before the retry, else the prompt goes in twice.
    // A fake Claude Code draws the box, shows the first paste only after
    // 3 s and writes what Return submits.
    {
        const std::string tui = bin + "/fake-tui.py";
        const std::string submitted = home + "/tui.submitted";
        writeFile(tui,
                  "import os, sys, time, tty\n"
                  "tty.setraw(0)\n"
                  "box = ''\n"
                  "pastes = 0\n"
                  "def draw():\n"
                  "    rule = '\\u2500' * 40\n"
                  "    os.write(1, ('\\x1b[2J\\x1b[30;1H' + rule + '\\r\\n\\u276f ' + box + '\\r\\n' + rule + '\\r\\n  ? for shortcuts').encode())\n"
                  "os.write(1, b'\\x1b[?2004h')\n"
                  "draw()\n"
                  "buf = b''\n"
                  "while True:\n"
                  "    data = os.read(0, 4096)\n"
                  "    if not data: break\n"
                  "    buf += data\n"
                  "    while buf:\n"
                  "        if buf.startswith(b'\\x1b[200~'):\n"
                  "            end = buf.find(b'\\x1b[201~')\n"
                  "            if end < 0: break\n"
                  "            box += buf[6:end].decode()\n"
                  "            buf = buf[end + 6:]\n"
                  "            pastes += 1\n"
                  "            if pastes == 1: time.sleep(3)\n"
                  "        elif buf[:1] == b'\\x1b':\n"
                  "            if len(buf) < 6: break\n"
                  "            buf = buf[6:]\n"
                  "        else:\n"
                  "            c = buf[:1]; buf = buf[1:]\n"
                  "            if c == b'\\r':\n"
                  "                with open('" + submitted + "', 'a') as f: f.write(box + '\\n')\n"
                  "                box = ''\n"
                  "            elif c == b'\\x15': box = ''\n"
                  "            elif c == b'\\x7f': box = box[:-1]\n"
                  "            elif c != b'\\x0b': box += c.decode()\n"
                  "        draw()\n",
                  false);
        const std::string pidFile = home + "/tui.pid";
        (void)std::system(("cd '" + project + "' && " + g_pvai + " run --detached --id wpaste -- python3 '" + tui
                           + "' >/dev/null 2>&1 & echo $! > '" + pidFile + "'").c_str());
        check(!waitSession(a, "wpaste", [](const json &) { return true; }, 3000).empty(), "paste: the wrapper registers");
        hook("wpaste", {{"hook_event_name", "SessionStart"}, {"session_id", "p-1"}, {"source", "startup"}});
        check(!waitSession(a, "wpaste", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "paste: idle");
        a.send({{"type", "prompt"}, {"ref", 70}, {"session", "wpaste"}, {"text", "eins"}});
        std::string got;
        for (int i = 0; i < 150 && got.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            got = readFile(submitted);
        }
        check(got == "eins\n", "paste: a slow first paste goes in once, not twice: [" + got + "]");
        hook("wpaste", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "p-1"}, {"prompt", "eins"}});
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 70; }).value("session", "") == "wpaste",
              "paste: delivered");
        (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
    }

    // 0.5.63: a Return Claude Code ignores is pressed again by the wrapper;
    // one it ignores for good empties the box and the daemon types the
    // prompt once more, then tells the owner. The fake Claude Code logs every
    // paste and every Return with the box it saw, and drops the first N Returns.
    {
        const std::string tui = bin + "/fake-tui-ignore.py";
        writeFile(tui,
                  "import os, sys, time, tty\n"
                  "tty.setraw(0)\n"
                  "log = sys.argv[1]\n"
                  "ignore = int(sys.argv[2])\n"
                  "newline = len(sys.argv) > 3\n"
                  "box = ''\n"
                  "def draw():\n"
                  "    rule = '\u2500' * 40\n"
                  "    rows = '\\r\\n  '.join(box.split('\\n'))\n"
                  "    os.write(1, ('\\x1b[2J\\x1b[28;1H' + rule + '\\r\\n\u276f ' + rows + '\\r\\n' + rule + '\\r\\n  ? for shortcuts').encode())\n"
                  "def note(s):\n"
                  "    with open(log, 'a') as f: f.write(s.replace('\\n', '|') + '\\n')\n"
                  "os.write(1, b'\\x1b[?2004h')\n"
                  "draw()\n"
                  "buf = b''\n"
                  "while True:\n"
                  "    data = os.read(0, 4096)\n"
                  "    if not data: break\n"
                  "    buf += data\n"
                  "    while buf:\n"
                  "        if buf.startswith(b'\\x1b[200~'):\n"
                  "            end = buf.find(b'\\x1b[201~')\n"
                  "            if end < 0: break\n"
                  "            box += buf[6:end].decode()\n"
                  "            buf = buf[end + 6:]\n"
                  "            note('P:' + box)\n"
                  "        elif buf[:1] == b'\\x1b':\n"
                  "            if len(buf) < 6: break\n"
                  "            buf = buf[6:]\n"
                  "        else:\n"
                  "            c = buf[:1]; buf = buf[1:]\n"
                  "            if c == b'\\r':\n"
                  "                if ignore > 0:\n"
                  "                    ignore -= 1; note('I:' + box)\n"
                  "                    if newline: box += '\\n'\n"
                  "                else:\n"
                  "                    note('S:' + box); box = ''\n"
                  "            elif c == b'\\x15': box = ''\n"
                  "            elif c == b'\\x7f': box = box[:-1]\n"
                  "            elif c != b'\\x0b': box += c.decode()\n"
                  "        draw()\n",
                  false);
        auto launch = [&](const std::string &id, const std::string &logFile, int ignore, bool newline = false) {
            const std::string pidFile = home + "/" + id + ".pid";
            (void)std::system(("cd '" + project + "' && " + g_pvai + " run --detached --id " + id + " -- python3 '" + tui + "' '" + logFile + "' "
                               + std::to_string(ignore) + (newline ? " newline" : "") + " >/dev/null 2>&1 & echo $! > '" + pidFile + "'").c_str());
            check(!waitSession(a, id, [](const json &) { return true; }, 3000).empty(), id + ": the wrapper registers");
            hook(id, {{"hook_event_name", "SessionStart"}, {"session_id", id + "-1"}, {"source", "startup"}});
            check(!waitSession(a, id, [](const json &s) { return s.value("state", "") == "idle"; }).empty(), id + ": idle");
            return pidFile;
        };
        const auto lines = [](const std::string &file) {
            std::vector<std::string> out;
            std::istringstream in(readFile(file));
            for (std::string l; std::getline(in, l);)
                out.push_back(l);
            return out;
        };
        // (a) the first Return is ignored, the second submits: one submission
        {
            const std::string log = home + "/ign1.log";
            const std::string pidFile = launch("wign1", log, 1);
            a.send({{"type", "prompt"}, {"ref", 90}, {"session", "wign1"}, {"text", "zwei"}});
            for (int i = 0; i < 100 && lines(log).size() < 3; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::this_thread::sleep_for(std::chrono::milliseconds(4000)); // no third Return, no retype
            check(lines(log) == std::vector<std::string>({"P:zwei", "I:zwei", "S:zwei"}), "ignored Return: pressed again, submitted once: [" + readFile(log) + "]");
            check(a.next("attention", [](const json &m) { return m.value("kind", "") == "stuck"; }, 100).empty(), "ignored Return: no stuck attention");
            hook("wign1", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "wign1-1"}, {"prompt", "zwei"}});
            check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 90; }).value("session", "") == "wign1", "ignored Return: delivered");
            (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
        }
        // (a2) the first Return becomes a new line in the box (0.5.68): the
        // box changed but still holds the prompt, so it is pressed again
        {
            const std::string log = home + "/ign3.log";
            const std::string pidFile = launch("wign3", log, 1, true);
            a.send({{"type", "prompt"}, {"ref", 92}, {"session", "wign3"}, {"text", "vier"}});
            for (int i = 0; i < 100 && lines(log).size() < 3; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            check(lines(log) == std::vector<std::string>({"P:vier", "I:vier", "S:vier|"}), "Return as a new line: pressed again: [" + readFile(log) + "]");
            hook("wign3", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "wign3-1"}, {"prompt", "vier"}});
            check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 92; }).value("session", "") == "wign3", "Return as a new line: delivered");
            (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
        }
        // (b) every Return is ignored: three presses, the box is emptied, the
        // owner is told; the prompt is not typed again by itself
        {
            const std::string log = home + "/ign2.log";
            const std::string pidFile = launch("wign2", log, 1000);
            a.send({{"type", "prompt"}, {"ref", 91}, {"session", "wign2"}, {"text", "drei"}});
            const json stuck = a.next("attention", [](const json &m) { return m.value("kind", "") == "stuck"; }, 40000);
            check(stuck.value("text", "") == "drei", "ignored Returns: the owner is told: " + stuck.dump());
            const auto got = lines(log);
            const std::vector<std::string> want = {"P:drei", "I:drei", "I:drei", "I:drei"};
            check(got == want, "ignored Returns: 3 presses, not typed again: [" + readFile(log) + "]");
            (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
        }
    }

    // 0.5.76: an Esc before any reply makes Claude Code put the unanswered
    // prompt back into its box (a real 2.1.319 does); the next paste would be
    // glued to it. After the app's interrupt the wrapper empties the box when
    // it holds the text the daemon typed. The fake puts the text back on a
    // lone Esc (mode restore) or does not (mode none).
    {
        const std::string tui = bin + "/fake-tui-esc.py";
        writeFile(tui,
                  "import os, sys, tty\n"
                  "tty.setraw(0)\n"
                  "log, mode = sys.argv[1], sys.argv[2]\n"
                  "box = ''\n"
                  "last = ''\n"
                  "def draw():\n"
                  "    rule = '\u2500' * 40\n"
                  "    os.write(1, ('\\x1b[2J\\x1b[28;1H' + rule + '\\r\\n\u276f ' + box + '\\r\\n' + rule + '\\r\\n  ? for shortcuts').encode())\n"
                  "def note(s):\n"
                  "    with open(log, 'a') as f: f.write(s + '\\n')\n"
                  "os.write(1, b'\\x1b[?2004h')\n"
                  "draw()\n"
                  "buf = b''\n"
                  "while True:\n"
                  "    data = os.read(0, 4096)\n"
                  "    if not data: break\n"
                  "    buf += data\n"
                  "    while buf:\n"
                  "        if buf.startswith(b'\\x1b[200~'):\n"
                  "            end = buf.find(b'\\x1b[201~')\n"
                  "            if end < 0: break\n"
                  "            box += buf[6:end].decode()\n"
                  "            buf = buf[end + 6:]\n"
                  "            note('P:' + box)\n"
                  "        elif buf == b'\\x1b':\n"
                  "            buf = b''\n"
                  "            if mode == 'restore' and last: box = last\n"
                  "            note('E:' + box)\n"
                  "        elif buf[:1] == b'\\x1b':\n"
                  "            if len(buf) < 6: break\n"
                  "            buf = buf[6:]\n"
                  "        else:\n"
                  "            c = buf[:1]; buf = buf[1:]\n"
                  "            if c == b'\\r':\n"
                  "                note('S:' + box); last = box; box = ''\n"
                  "            elif c == b'\\x15': box = ''\n"
                  "            elif c == b'\\x7f': box = box[:-1]\n"
                  "            elif c != b'\\x0b': box += c.decode()\n"
                  "        draw()\n",
                  false);
        const auto lines = [](const std::string &file) {
            std::vector<std::string> out;
            std::istringstream in(readFile(file));
            for (std::string l; std::getline(in, l);)
                out.push_back(l);
            return out;
        };
        for (const std::string mode : {"restore", "none"}) {
            const std::string id = "wesc-" + mode;
            const std::string log = home + "/" + id + ".log";
            const std::string pidFile = home + "/" + id + ".pid";
            (void)std::system(("cd '" + project + "' && " + g_pvai + " run --detached --id " + id + " -- python3 '" + tui + "' '" + log + "' " + mode
                               + " >/dev/null 2>&1 & echo $! > '" + pidFile + "'").c_str());
            check(!waitSession(a, id, [](const json &) { return true; }, 3000).empty(), id + ": the wrapper registers");
            hook(id, {{"hook_event_name", "SessionStart"}, {"session_id", id + "-1"}, {"source", "startup"}});
            check(!waitSession(a, id, [](const json &s) { return s.value("state", "") == "idle"; }).empty(), id + ": idle");
            a.send({{"type", "prompt"}, {"ref", 160}, {"session", id}, {"text", "write a story about a cat"}});
            for (int i = 0; i < 80 && lines(log).size() < 2; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            hook(id, {{"hook_event_name", "UserPromptSubmit"}, {"session_id", id + "-1"}, {"prompt", "write a story about a cat"}});
            // Quickly, within the wrapper's 1.5 s check that the Return took: the restored
            // prompt is no Return that did not take, no second Return for it.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            a.send({{"type", "interrupt"}, {"ref", 161}, {"session", id}});
            check(!a.next("interrupting").empty(), id + ": interrupting");
            check(!waitSession(a, id, [](const json &s) { return s.value("state", "") == "idle"; }, 8000).empty(), id + ": idle after the interrupt");
            a.send({{"type", "prompt"}, {"ref", 162}, {"session", id}, {"text", "/compact"}});
            for (int i = 0; i < 80 && lines(log).size() < 5; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::string restored = mode == "restore" ? "E:write a story about a cat" : "E:";
            check(lines(log) == std::vector<std::string>({"P:write a story about a cat", "S:write a story about a cat", restored, "P:/compact", "S:/compact"}),
                  id + ": the restored prompt is emptied before the next paste: [" + readFile(log) + "]");
            (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
        }
    }

    // 0.5.44: a typed ok:false without a screen is retried; trust answered in
    // the terminal; dialog keys; big hook events.
    {
        LineClient rw;
        rw.send({{"type", "register"}, {"version", kVersion}, {"id", "wretry"}, {"cwd", project}, {"label", "retry"}, {"restart", true}, {"verify", true}});
        hook("wretry", {{"hook_event_name", "SessionStart"}, {"session_id", "r-1"}, {"source", "startup"}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a retry wrapper's session");
        rw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        a.send({{"type", "prompt"}, {"ref", 60}, {"session", "wretry"}, {"text", "eins"}});
        check(rw.next("type").value("text", "") == "eins", "retry: typed");
        rw.send({{"type", "typed"}, {"ok", false}, {"screen", "busy box"}}); // no screen message after it
        const auto before = std::chrono::steady_clock::now();
        json again = rw.next("type", {}, 3000);
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count();
        check(again.value("text", "") == "eins" && again.value("verify", false) && waited >= 400,
              "retry: the prompt is typed again after a moment without a new screen (" + std::to_string(waited) + " ms)");
        rw.send({{"type", "typed"}, {"ok", true}});
        hook("wretry", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "r-1"}, {"prompt", "eins"}});
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 60; }).value("session", "") == "wretry", "retry: delivered");
        hook("wretry", {{"hook_event_name", "Stop"}, {"session_id", "r-1"}, {"last_assistant_message", "Eins."}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "retry: idle again");
        // A box that never shows the paste: three tries, then the owner decides
        // (unbounded, /compact went in 146 times in 3 minutes). The same
        // screen again waits 2 s, then 4 s, before the next try.
        a.send({{"type", "prompt"}, {"ref", 61}, {"session", "wretry"}, {"text", "/compact"}});
        for (int i = 0; i < 3; ++i) {
            check(rw.next("type", {}, 5000).value("text", "") == "/compact", "bounded retry: try " + std::to_string(i + 1));
            rw.send({{"type", "typed"}, {"ok", false}, {"screen", "busy box"}});
            if (i < 2) {
                rw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
                check(rw.next("type", {}, 1500).empty(), "same screen: not typed again within the pause (try " + std::to_string(i + 1) + ")");
            }
        }
        check(a.next("attention", [](const json &m) { return m.value("kind", "") == "stuck"; }, 2000).value("text", "") == "/compact",
              "bounded retry: the owner is told after three tries");
        rw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        check(rw.next("type", {}, 2500).empty(), "bounded retry: not typed a fourth time by itself");
        a.send({{"type", "answer"}, {"ref", 62}, {"session", "wretry"}, {"choice", "send"}});
        check(rw.next("type").value("text", "") == "/compact", "bounded retry: the owner's send types it");
        rw.send({{"type", "output"}, {"data", ""}});
        hook("wretry", {{"hook_event_name", "Stop"}, {"session_id", "r-1"}, {"last_assistant_message", "ok"}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "bounded retry: idle again");

        // A session survey over a free prompt box: a dialog; nothing is typed
        // into it (it takes single keys), only the owner's key goes through.
        {
            a.send({{"type", "prompt"}, {"ref", 70}, {"session", "wretry"}, {"text", "umfrage"}});
            check(rw.next("type").value("text", "") == "umfrage", "survey: typed");
            rw.send({{"type", "typed"}, {"ok", false}, {"screen", "\xE2\x97\x8F How is Claude doing this session? (optional)\n  1: Bad  2: Fine"}});
            const std::string q = "\xE2\x97\x8F Can Anthropic look at your session transcript to help us improve Claude Code?\n  y: Yes     n: No      d: Don't ask again";
            rw.send({{"type", "screen"}, {"dialog", q}, {"prompt", false}});
            json s2 = waitSession(a, "wretry", [&](const json &s) { return s.value("dialog", json()) == "screen" && sub(s, "dialogInfo").value("text", "") == q; });
            check(!s2.empty(), "survey: shown as a screen dialog");
            const json opts = sub(s2, "dialogInfo").value("options", json());
            check(opts == json::parse(R"([{"id":"y","label":"Yes"},{"id":"n","label":"No"},{"id":"d","label":"Don't ask again"}])"),
                  "survey: options from the screen: " + opts.dump());
            rw.send({{"type", "screen"}, {"dialog", q}, {"prompt", true}});
            check(rw.next("type", {}, 3500).empty(), "survey: nothing typed while it stands");
            a.send({{"type", "answer"}, {"ref", 71}, {"session", "wretry"}, {"choice", "key:y"}});
            check(rw.next("keys").value("data", "") == "y", "survey: the owner's key goes through as the single key");
            rw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
            check(rw.next("type", {}, 3000).value("text", "") == "umfrage", "survey gone: the prompt goes in");
            rw.send({{"type", "typed"}, {"ok", true}});
            hook("wretry", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "r-1"}, {"prompt", "umfrage"}});
            hook("wretry", {{"hook_event_name", "Stop"}, {"session_id", "r-1"}, {"last_assistant_message", "ok"}});
            check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "survey: idle again");
        }

        // Dialog keys: arrows and terminal replies do not answer a dialog;
        // Return does; an older wrapper (no key) counts every key.
        hook("wretry", {{"hook_event_name", "PermissionRequest"}, {"session_id", "r-1"}, {"tool_name", "Bash"}, {"tool_input", {{"command", "ls"}}}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "waiting"; }).empty(), "keys: a dialog waits");
        rw.send({{"type", "input"}, {"key", "other"}});
        check(waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "busy"; }, 800).empty(), "keys: an arrow key does not answer it");
        rw.send({{"type", "input"}, {"key", "return"}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "keys: Return answers it");
        hook("wretry", {{"hook_event_name", "PermissionRequest"}, {"session_id", "r-1"}, {"tool_name", "Bash"}, {"tool_input", {{"command", "ls"}}}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "waiting"; }).empty(), "keys: waits again");
        rw.send({{"type", "input"}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "keys: no key field counts as before");

        // A Stop with a 1 MB answer is not lost; a PermissionRequest from the
        // real hook is cut to 64 KiB.
        const std::string big(1024 * 1024, 'x');
        hook("wretry", {{"hook_event_name", "Stop"}, {"session_id", "r-1"}, {"last_assistant_message", big}});
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "big: a 1 MB Stop reaches the session");
        hook("wretry", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "r-1"}, {"prompt", "zwei"}});
        const std::string eventFile = home + "/bigevent.json";
        writeFile(eventFile, json({{"hook_event_name", "PermissionRequest"}, {"session_id", "r-1"}, {"tool_name", "Bash"},
                                   {"tool_input", {{"command", big}}}, {"transcript_path", ""}}).dump());
        (void)std::system(("cat " + eventFile + " | env PVAI_ID=wretry PVAI_SOCK=" + daemonSocket() + " " + g_pvai + " hook").c_str());
        check(!waitSession(a, "wretry", [](const json &s) { return s.value("state", "") == "waiting"; }).empty(),
              "big: the real hook's PermissionRequest reaches the session");
        json event = {{"hook_event_name", "Stop"}, {"last_assistant_message", std::string(100000, 'x') + "\xC3\xA4\xC3\xA4"},
                      {"tool_input", {{"command", big}, {"edits", json::array({{{"new_string", big}}})}, {"short", "ok"}}}};
        cutHookEvent(event);
        const std::string last = event["last_assistant_message"].get<std::string>();
        check(last.size() > kHookStringMax && last.size() < kHookStringMax + 100 && last.find("cut by pvai") != std::string::npos,
              "big: the last answer is cut to 64 KiB with a marker (" + std::to_string(last.size()) + ")");
        check(event["tool_input"]["command"].get<std::string>().size() < kHookStringMax + 100
                  && event["tool_input"]["edits"][0]["new_string"].get<std::string>().size() < kHookStringMax + 100
                  && event["tool_input"]["short"] == "ok",
              "big: tool input strings are cut, also nested, short ones stay");
        const std::string accent = std::string(kHookStringMax - 1, 'x') + "\xC3\xA4" + std::string(10, 'y');
        json umlaut = {{"last_assistant_message", accent}};
        cutHookEvent(umlaut);
        check(json::accept(umlaut.dump()) && umlaut["last_assistant_message"].get<std::string>().find("\xC3\xA4") == std::string::npos,
              "big: a cut never splits a UTF-8 character");

        LineClient tw;
        tw.send({{"type", "register"}, {"version", kVersion}, {"id", "wtrust"}, {"cwd", project}, {"label", "trust"}, {"restart", true}, {"verify", true}});
        tw.send({{"type", "dialog"}, {"kind", "trust"}});
        check(!waitSession(a, "wtrust", [](const json &s) { return s.value("dialog", "") == "trust"; }).empty(), "trust: the dialog is shown");
        hook("wtrust", {{"hook_event_name", "SessionStart"}, {"session_id", "t-1"}, {"source", "startup"}});
        check(!waitSession(a, "wtrust", [](const json &s) { return s.value("state", "") == "idle" && s.value("dialog", json()).is_null(); }).empty(),
              "trust: answered in the terminal, the next SessionStart makes it idle");
    }

    // clear (0.5.30): empties the prompt box, a draft or a command left in
    // shell mode, and the queue goes on.
    {
        LineClient clr;
        clr.send({{"type", "register"}, {"version", kVersion}, {"id", "wclr"}, {"cwd", project}, {"label", "clr"}, {"restart", true}, {"verify", true}, {"clear", true}});
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
        clr.send({{"type", "screen"}, {"dialog", "!  echo old"}, {"prompt", false}});
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

    // 0.5.71: "send anyway" never presses Return into something that is not
    // our text. A fake Claude Code logs every paste (P:) and every Return (R)
    // it gets; "offer" draws Claude Code's auto mode offer under the box,
    // "plain" a plain box that never shows a paste.
    {
        const std::string tui = bin + "/fake-tui-offer.py";
        writeFile(tui,
                  "import os, sys, tty\n"
                  "tty.setraw(0)\n"
                  "log, mode = sys.argv[1], sys.argv[2]\n"
                  "rule = '\\u2500' * 60\n"
                  "def note(s):\n"
                  "    with open(log, 'a') as f: f.write(s + '\\n')\n"
                  "text = '\\x1b[2J\\x1b[20;1H' + rule + '\\r\\n\\u276f \\r\\n' + rule + '\\r\\n'\n"
                  "if mode == 'offer':\n"
                  "    text += (rule + '\\r\\n Teach auto mode about your environment?\\r\\n Auto mode works better when it knows your environment.\\r\\n\\r\\n"
                  " \\u276f 1. Yes\\r\\n   2. Not now\\r\\n   3. Don\\'t show again\\r\\n\\r\\n Enter to confirm \\u00b7 Esc to cancel')\n"
                  "else:\n"
                  "    text += '  ? for shortcuts'\n"
                  "os.write(1, b'\\x1b[?2004h' + text.encode())\n"
                  "while True:\n"
                  "    data = os.read(0, 4096)\n"
                  "    if not data: break\n"
                  "    if b'\\x1b[200~' in data: note('P:' + data.decode(errors='replace'))\n"
                  "    if b'\\r' in data: note('R')\n",
                  false);
        auto launch = [&](const std::string &id, const std::string &logFile, const std::string &mode) {
            const std::string pidFile = home + "/" + id + ".pid";
            (void)std::system(("cd '" + project + "' && " + g_pvai + " run --detached --id " + id + " -- python3 '" + tui + "' '" + logFile + "' " + mode
                               + " >/dev/null 2>&1 & echo $! > '" + pidFile + "'").c_str());
            check(!waitSession(a, id, [](const json &) { return true; }, 3000).empty(), id + ": the wrapper registers");
            hook(id, {{"hook_event_name", "SessionStart"}, {"session_id", id + "-1"}, {"source", "startup"}});
            check(!waitSession(a, id, [](const json &s) { return s.value("state", "") == "idle"; }).empty(), id + ": idle");
            return pidFile;
        };
        {
            const std::string log = home + "/offer.log";
            const std::string pidFile = launch("woffer", log, "offer");
            const json shown = waitSession(a, "woffer", [](const json &s) { return s.value("dialog", json()) == "screen"; }, 5000);
            check(!shown.empty() && shown["dialogInfo"].value("options", json::array()).size() == 3,
                  "the offer is a screen dialog with its three keys: " + shown.dump());
            a.send({{"type", "prompt"}, {"ref", 120}, {"session", "woffer"}, {"text", "diktat"}});
            a.send({{"type", "answer"}, {"ref", 121}, {"session", "woffer"}, {"choice", "send"}});
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            check(readFile(log).empty(), "the offer stands: no paste, no Return: [" + readFile(log) + "]");
            (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
        }
        {
            const std::string log = home + "/plain.log";
            const std::string pidFile = launch("wplain", log, "plain");
            a.send({{"type", "prompt"}, {"ref", 122}, {"session", "wplain"}, {"text", "diktat"}});
            check(!a.next("attention", [](const json &m) { return m.value("session", "") == "wplain" && m.value("kind", "") == "screen"; }, 30000).empty(),
                  "the box never shows the paste: the owner sees the screen once the retries are over");
            std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // the wrapper has emptied the box
            a.send({{"type", "answer"}, {"ref", 123}, {"session", "wplain"}, {"choice", "send"}});
            check(!a.next("attention", [](const json &m) { return m.value("session", "") == "wplain" && m.value("kind", "") == "stuck"; }, 40000).empty(),
                  "sent anyway, still not in the box: stuck");
            check(readFile(log).find('R') == std::string::npos && readFile(log).find("P:") != std::string::npos,
                  "pasted, but no Return was pressed: [" + readFile(log) + "]");
            (void)std::system(("kill $(cat '" + pidFile + "') 2>/dev/null").c_str());
        }
    }

    // A fake wrapper: a prompt sent anyway that is not reported as typed
    // gives the screen back; a command Claude Code ran in its place is lost,
    // told to the owner with the whole text, and never typed again.
    {
        LineClient lw;
        lw.send({{"type", "register"}, {"version", kVersion}, {"id", "wlost"}, {"cwd", project}, {"label", "lost"}, {"restart", true}, {"verify", true},
                 {"clear", true}, {"submitCheck", true}, {"typedUnchecked", true}});
        hook("wlost", {{"hook_event_name", "SessionStart"}, {"session_id", "lost-1"}, {"source", "startup"}});
        check(!waitSession(a, "wlost", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a lost-prompt test session");
        a.send({{"type", "prompt"}, {"ref", 130}, {"session", "wlost"}, {"text", "ein langer diktierter Satz"}});
        check(lw.next("type").value("verify", false), "typed with the check");
        lw.send({{"type", "screen"}, {"dialog", "busy"}, {"prompt", false}});
        lw.send({{"type", "typed"}, {"ok", false}, {"screen", "busy"}});
        check(!waitSession(a, "wlost", [](const json &s) { return s.value("dialog", json()) == "screen"; }).empty(), "blocked");
        a.send({{"type", "answer"}, {"ref", 131}, {"session", "wlost"}, {"choice", "send"}});
        const json anyway = lw.next("type");
        check(anyway.value("text", "") == "ein langer diktierter Satz" && !anyway.value("verify", true), "answer send types it unchecked");
        lw.send({{"type", "typed"}, {"ok", false}, {"screen", "offer"}});
        const json stuck = a.next("attention", [](const json &m) { return m.value("session", "") == "wlost" && m.value("kind", "") == "stuck"; }, 3000);
        check(!stuck.empty(), "unchecked and not typed: the owner is told");
        check(lw.next("type", {}, 2500).empty(), "and it is not typed again by itself");
        a.send({{"type", "answer"}, {"ref", 132}, {"session", "wlost"}, {"choice", "send"}});
        check(lw.next("type").value("text", "") == "ein langer diktierter Satz", "sent anyway once more");
        lw.send({{"type", "typed"}, {"ok", true}});
        hook("wlost", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "lost-1"}, {"prompt", "/auto-mode-setup"}});
        const json dropped = a.next("dropped", [](const json &m) { return !m.value("prompts", json::array()).empty(); }, 3000);
        check(!dropped.empty() && dropped["prompts"][0].value("text", "") == "ein langer diktierter Satz" && dropped["prompts"][0].value("ref", 0) == 130
                  && dropped["prompts"][0].value("session", "") == "wlost",
              "the lost prompt comes back whole: " + dropped.dump());
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 130; }, 500).empty(), "it is not reported delivered");
    }

    // A Return Claude Code did not act on (the wrapper says `submitted`
    // ok:false): a slash command ends as stuck, a prompt is typed once more
    // and then shown to the owner. Any compaction ends a pending /compact.
    {
        LineClient sw;
        sw.send({{"type", "register"}, {"version", kVersion}, {"id", "wstuck"}, {"cwd", project}, {"label", "stuck"}, {"restart", true}, {"verify", true}, {"clear", true}, {"submitCheck", true}});
        const std::string stuckTr = home + "/stuck-tr/stuck-1.jsonl"; // outside the conversations list
        writeFile(stuckTr, userLine("start"));
        hook("wstuck", {{"hook_event_name", "SessionStart"}, {"session_id", "stuck-1"}, {"source", "startup"}, {"transcript_path", stuckTr}});
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a slash test session");
        // (a) a slash command whose Return did not take
        a.send({{"type", "prompt"}, {"ref", 80}, {"session", "wstuck"}, {"text", "/context"}});
        check(sw.next("type").value("text", "") == "/context", "the slash command is typed");
        sw.send({{"type", "typed"}, {"ok", true}});
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "busy while the command is pending");
        sw.send({{"type", "submitted"}, {"ok", false}, {"screen", "SECRET talk above\n\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n\xE2\x9D\xAF /context\n\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n  ? for shortcuts"}});
        const json stuck = a.next("attention", [](const json &m) { return m.value("kind", "") == "stuck"; }, 2000);
        check(stuck.value("text", "") == "/context", "the app is told: " + stuck.dump());
        {
            const std::string log = readFile(daemonLog);
            check(log.find("[screen: \xE2\x9D\xAF /context | ? for shortcuts]") != std::string::npos && log.find("SECRET talk") == std::string::npos,
                  "the log shows the box and footer of the stuck command, not the conversation above");
        }
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle after the stuck command");
        // (b) a prompt whose Return did not take: never typed again by
        // itself (Claude Code may have taken it after all); the owner sees it
        a.send({{"type", "prompt"}, {"ref", 81}, {"session", "wstuck"}, {"text", "eins"}});
        check(sw.next("type").value("text", "") == "eins", "the prompt is typed");
        sw.send({{"type", "typed"}, {"ok", true}});
        sw.send({{"type", "submitted"}, {"ok", true}}); // ignored
        sw.send({{"type", "submitted"}, {"ok", false}});
        const json stuck2 = a.next("attention", [](const json &m) { return m.value("kind", "") == "stuck"; }, 2000);
        check(stuck2.value("text", "") == "eins", "the failure tells the app: " + stuck2.dump());
        json shown = waitSession(a, "wstuck", [](const json &s) { return s.value("dialog", json()) == "screen"; });
        check(!shown.empty() && shown.value("queued", 0) == 1, "and the owner sees a screen dialog, the prompt waits: " + shown.dump());
        check(sw.next("type", {}, 3000).empty(), "it is not typed again by itself");
        a.send({{"type", "answer"}, {"ref", 84}, {"session", "wstuck"}, {"choice", "send"}});
        check(sw.next("type").value("text", "") == "eins", "answer send types it");
        hook("wstuck", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "stuck-1"}, {"prompt", "eins"}});
        hook("wstuck", {{"hook_event_name", "Stop"}, {"session_id", "stuck-1"}, {"last_assistant_message", "Eins."}});
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "idle again");
        a.send({{"type", "prompt"}, {"ref", 85}, {"session", "wstuck"}, {"text", "/context"}});
        check(sw.next("type").value("text", "") == "/context", "a command is typed");
        sw.send({{"type", "typed"}, {"ok", true}});
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "busy while it is pending");
        check(waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") != "busy"; }, 500).empty(), "a free box without output does not end it");
        // (c) an auto compact_boundary ends a pending /compact
        std::ofstream(stuckTr, std::ios::app) << userLine("<command-name>/context</command-name>") + userLine("<local-command-stdout>ctx</local-command-stdout>");
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "the output ends the /context");
        a.send({{"type", "prompt"}, {"ref", 82}, {"session", "wstuck"}, {"text", "/compact"}});
        check(sw.next("type").value("text", "") == "/compact", "/compact is typed");
        sw.send({{"type", "typed"}, {"ok", true}});
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "busy while /compact is pending");
        std::ofstream(stuckTr, std::ios::app) << R"({"type":"system","subtype":"compact_boundary","uuid":"cb1","compactMetadata":{"trigger":"auto"}})" "\n";
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle"; }, 5000).empty(), "an auto compaction ends a pending /compact");
        const std::string turnEnd = R"({"type":"system","subtype":"turn_duration","durationMs":5})" "\n";
        auto stamped = [](json line, std::time_t at) {
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&at));
            line["timestamp"] = buf;
            return line.dump() + "\n";
        };
        // (d0) an old line left the transcript at running (a slash command
        // as a plain user line writes no turn end): a prompt typed after it
        // that never came is not taken as delivered, the screen is asked for
        std::ofstream(stuckTr, std::ios::app) << turnEnd
            << stamped(json::parse(userLine("/compact")), std::time(nullptr) - 7200);
        const std::string vierText = "[Message p0000vier from M4/wx (x), released by the owner.]\nVier, lang genug.";
        a.send({{"type", "prompt"}, {"ref", 86}, {"session", "wstuck"}, {"text", vierText}});
        check(sw.next("type").value("text", "") == vierText, "a prompt is typed after an old running line");
        sw.send({{"type", "typed"}, {"ok", true}});
        check(!sw.next("snapshot", {}, 17000).empty(), "an old running line is not this prompt: the daemon asks for the screen");
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 86; }, 300).empty(), "and it is not taken as delivered");
        sw.send({{"type", "snapshot"}, {"screen", "vier"}});
        check(!sw.next("clear", {}, 2000).empty(), "the box is emptied: typed again, the prompt is not added to its copy");
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("dialog", json()) == "screen"; }).empty(), "the owner sees the screen");
        // The owner presses Return in the terminal on a box that held the
        // copy twice (before the clear above): it went out, not typed again
        hook("wstuck", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "stuck-1"}, {"prompt", vierText + vierText}});
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 86; }).value("ref", 0) == 86,
              "a prompt that holds the queued one twice delivers it");
        hook("wstuck", {{"hook_event_name", "Stop"}, {"session_id", "stuck-1"}, {"last_assistant_message", "Vier."}});
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle" && s.value("queued", -1) == 0; }).empty(),
              "idle after it, nothing queued");
        check(sw.next("type", {}, 1500).empty(), "and it is not typed a third time");
        // (d) typed, the turn runs in the transcript, but UserPromptSubmit
        // never comes: delivered, not typed a second time, no screen dialog
        std::ofstream(stuckTr, std::ios::app) << turnEnd;
        a.send({{"type", "prompt"}, {"ref", 83}, {"session", "wstuck"}, {"text", "drei"}});
        check(sw.next("type").value("text", "") == "drei", "a prompt is typed");
        sw.send({{"type", "typed"}, {"ok", true}});
        std::ofstream(stuckTr, std::ios::app) << stamped(json::parse(userLine("drei")), std::time(nullptr))
            << stamped(json::parse(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Drei."}]}})"), std::time(nullptr));
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 83; }, 18000).value("ref", 0) == 83,
              "no UserPromptSubmit, but the turn runs: delivered");
        check(sw.next("snapshot", {}, 300).empty() && sw.next("type", {}, 300).empty(), "no snapshot, not typed again");
        json running = waitSession(a, "wstuck", [](const json &s) { return true; });
        check(running.value("state", "") == "busy" && running.value("dialog", json()).is_null() && running.value("queued", -1) == 0,
              "busy, no dialog, nothing queued: " + running.dump());
        std::ofstream(stuckTr, std::ios::app) << turnEnd;
        check(!waitSession(a, "wstuck", [](const json &s) { return s.value("state", "") == "idle"; }, 8000).empty(), "the turn end in the transcript ends it");
    }

    // A prompt typed in the last moments of a turn (right on its Stop hook):
    // Claude Code puts it in its own queue, the box empties, and no
    // UserPromptSubmit comes. It stays in flight, the next prompt is not
    // typed on top of it; Claude Code's UserPromptSubmit delivers it, and a
    // queue it never takes up is reported.
    {
        LineClient qw;
        qw.send({{"type", "register"}, {"version", kVersion}, {"id", "wqueue"}, {"cwd", project}, {"label", "queue"}, {"restart", true}, {"verify", true}, {"clear", true}, {"submitCheck", true}});
        const std::string queueTr = home + "/queue-tr/queue-1.jsonl";
        writeFile(queueTr, userLine("start"));
        hook("wqueue", {{"hook_event_name", "SessionStart"}, {"session_id", "queue-1"}, {"source", "startup"}, {"transcript_path", queueTr}});
        check(!waitSession(a, "wqueue", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "a queue test session");
        const auto enqueue = [](const std::string &text) {
            return json({{"type", "queue-operation"}, {"operation", "enqueue"}, {"content", text}}).dump() + "\n";
        };
        // (a) the prompt stands in Claude Code's queue when the Stop hook comes
        a.send({{"type", "prompt"}, {"ref", 200}, {"session", "wqueue"}, {"text", "Titel bei 16"}});
        check(qw.next("type").value("text", "") == "Titel bei 16", "the first prompt is typed");
        qw.send({{"type", "typed"}, {"ok", true}});
        qw.send({{"type", "submitted"}, {"ok", true}, {"why", "box"}});
        std::ofstream(queueTr, std::ios::app) << enqueue("Titel bei 16");
        std::this_thread::sleep_for(std::chrono::milliseconds(900)); // the transcript is read
        hook("wqueue", {{"hook_event_name", "Stop"}, {"session_id", "queue-1"}, {"last_assistant_message", "Fertig."}});
        a.send({{"type", "prompt"}, {"ref", 201}, {"session", "wqueue"}, {"text", "Brauchen wir 4 Gradients"}});
        const json held = waitSession(a, "wqueue", [](const json &s) { return s.value("queued", 0) == 2; });
        check(!held.empty() && held.value("dialog", json()).is_null() && held.value("state", "") == "idle", "both count as queued, idle, no screen dialog: " + held.dump());
        check(qw.next("type", {}, 2500).empty(), "the next prompt is not typed on top of the queued one");
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 200; }, 300).empty(), "and the queued one is not reported delivered yet");
        // Claude Code takes its queue up after the turn: the prompt is delivered, then the next goes in
        std::ofstream(queueTr, std::ios::app) << json({{"type", "queue-operation"}, {"operation", "dequeue"}}).dump() << "\n";
        hook("wqueue", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "queue-1"}, {"prompt", "Titel bei 16"}});
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 200; }, 2000).value("ref", 0) == 200, "the queued prompt is delivered by its UserPromptSubmit");
        hook("wqueue", {{"hook_event_name", "Stop"}, {"session_id", "queue-1"}, {"last_assistant_message", "Ok."}});
        check(qw.next("type").value("text", "") == "Brauchen wir 4 Gradients", "then the next prompt is typed");
        qw.send({{"type", "typed"}, {"ok", true}});
        hook("wqueue", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "queue-1"}, {"prompt", "Brauchen wir 4 Gradients"}});
        hook("wqueue", {{"hook_event_name", "Stop"}, {"session_id", "queue-1"}, {"last_assistant_message", "Ok."}});
        check(!waitSession(a, "wqueue", [](const json &s) { return s.value("state", "") == "idle" && s.value("queued", -1) == 0; }, 5000).empty(), "idle, nothing queued");
        // (b) Claude Code never takes the queue up: reported, not typed again
        a.send({{"type", "prompt"}, {"ref", 202}, {"session", "wqueue"}, {"text", "drei Gradients"}});
        check(qw.next("type").value("text", "") == "drei Gradients", "another prompt is typed");
        qw.send({{"type", "typed"}, {"ok", true}});
        std::ofstream(queueTr, std::ios::app) << enqueue("drei Gradients");
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        hook("wqueue", {{"hook_event_name", "Stop"}, {"session_id", "queue-1"}, {"last_assistant_message", "Zu."}});
        a.send({{"type", "prompt"}, {"ref", 203}, {"session", "wqueue"}, {"text", "und vier"}});
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 202; }, 25000).value("ref", 0) == 202, "it counts as delivered, it goes in with the next one");
        const json gone = a.next("attention", [](const json &m) { return m.value("session", "") == "wqueue" && m.value("kind", "") == "stuck"; }, 25000);
        check(!gone.empty() && gone.value("text", "") == "In Claude Code's queue: drei Gradients", "a queue Claude Code never takes up is reported: " + gone.dump());
        check(a.next("dropped", {}, 300).empty(), "and is not dropped (a resend would arrive twice)");
        check(qw.next("type", {}, 3000).value("text", "") == "und vier", "and the prompt behind it goes in then");
        qw.send({{"type", "typed"}, {"ok", true}});
        hook("wqueue", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "queue-1"}, {"prompt", "drei Gradients\nund vier"}});
        hook("wqueue", {{"hook_event_name", "Stop"}, {"session_id", "queue-1"}, {"last_assistant_message", "Ok."}});
        check(!waitSession(a, "wqueue", [](const json &s) { return s.value("state", "") == "idle" && s.value("queued", -1) == 0; }, 5000).empty(), "idle, nothing queued");
        // (c) Claude Code absorbs the queued prompt into the running turn: no UserPromptSubmit, delivered, not typed again
        a.send({{"type", "prompt"}, {"ref", 204}, {"session", "wqueue"}, {"text", "Bis jetzt auf 63."}, {"voice", true}});
        check(!qw.next("type").empty(), "the voice prompt is typed");
        qw.send({{"type", "typed"}, {"ok", true}});
        std::ofstream(queueTr, std::ios::app) << enqueue("\xF0\x9F\x8E\x99 Bis jetzt auf 63.")
            << json({{"type", "queue-operation"}, {"operation", "remove"}, {"content", "\xF0\x9F\x8E\x99 Bis jetzt auf 63."}, {"reason", "absorbed_mid_turn"}}).dump() << "\n";
        check(a.next("delivered", [](const json &m) { return m.value("ref", 0) == 204; }, 3000).value("ref", 0) == 204, "an absorbed prompt is delivered without UserPromptSubmit");
        check(qw.next("snapshot", {}, 600).empty() && qw.next("type", {}, 300).empty(), "no snapshot, not typed again");
    }

    // AskUserQuestion (0.5.33): the questions with their choices; option,
    // next and text answer them, the review comes last.
    {
        LineClient qw;
        qw.send({{"type", "register"}, {"version", kVersion}, {"id", "wq"}, {"cwd", project}, {"label", "q"}, {"restart", true}, {"verify", true}, {"clear", true}, {"paced", true}});
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
        auto wrapperKeys = [](LineClient &w) { return w.next("keys").value("data", ""); };
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
        LineClient mid, done;
        mid.send({{"type", "register"}, {"version", kVersion}, {"id", "wmid"}, {"cwd", project}, {"label", "mid"}, {"running", true}, {"restart", true}});
        done.send({{"type", "register"}, {"version", kVersion}, {"id", "wdone"}, {"cwd", project}, {"label", "done"}, {"running", true}, {"restart", true}});
        check(!waitSession(a, "wmid", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
              "mid-turn after a daemon restart: busy");
        check(!waitSession(a, "wdone", [](const json &s) { return s.value("state", "") == "idle"; }).empty(),
              "after the turn's end: idle");
        // A turn a task notification started, taken over while it runs.
        const std::string wakeFile = home + "/elsewhere/wake.jsonl";
        const std::string notice =
            R"({"type":"attachment","isSidechain":false,"attachment":{"type":"queued_command","prompt":"<task-notification>\n<status>completed</status>\n</task-notification>"}})";
        const std::string tool = R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{}}]}})";
        writeFile(wakeFile, prompt + "\n" + answer + "\n" + end + "\n" + notice + "\n" + tool + "\n");
        writeFile(runtime + "/pvai/wwake.claude", json({{"claudeSession", "wake-1"}, {"transcript", wakeFile}}).dump() + "\n");
        char stamp[32];
        const std::time_t soon = std::time(nullptr) - 30;
        std::tm tm{};
        gmtime_r(&soon, &tm);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        const std::string freshFile = home + "/elsewhere/fresh.jsonl";
        json freshNotice = json::parse(notice);
        freshNotice["timestamp"] = stamp;
        writeFile(freshFile, prompt + "\n" + answer + "\n" + end + "\n" + freshNotice.dump() + "\n");
        writeFile(runtime + "/pvai/wfresh.claude", json({{"claudeSession", "fresh-1"}, {"transcript", freshFile}}).dump() + "\n");
        LineClient fresh;
        fresh.send({{"type", "register"}, {"version", kVersion}, {"id", "wfresh"}, {"cwd", project}, {"label", "fresh"}, {"running", true}, {"restart", true}});
        check(!waitSession(a, "wfresh", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
              "taken over with a fresh unanswered task notification: busy");
        {
            char stampMac[32];
            const std::time_t t = std::time(nullptr) - 30;
            std::tm tmMac{};
            gmtime_r(&t, &tmMac);
            std::strftime(stampMac, sizeof stampMac, "%Y-%m-%dT%H:%M:%S.000Z", &tmMac);
            const json meta = {{"type", "user"}, {"isMeta", true}, {"timestamp", stampMac},
                               {"message", {{"role", "user"}, {"content", "Another Claude session sent a message: <agent-message from=\"x\">hi</agent-message>"}}}};
            const std::string macFile = home + "/elsewhere/mac.jsonl";
            writeFile(macFile, meta.dump() + "\n" + tool + "\n" + R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})" "\n");
            writeFile(runtime + "/pvai/wmac.claude", json({{"claudeSession", "mac-1"}, {"transcript", macFile}}).dump() + "\n");
            LineClient macClient;
            macClient.send({{"type", "register"}, {"version", kVersion}, {"id", "wmac"}, {"cwd", project}, {"label", "mac"}, {"running", true}, {"restart", true}});
            check(!waitSession(a, "wmac", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
                  "taken over in a turn a meta hand-back started, no turn end in the file: busy");
        }
        LineClient wake;
        wake.send({{"type", "register"}, {"version", kVersion}, {"id", "wwake"}, {"cwd", project}, {"label", "wake"}, {"running", true}, {"restart", true}});
        check(!waitSession(a, "wwake", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
              "taken over in a turn a task notification started: busy");
        // A spoken turn taken over while it runs is still spoken at its Stop.
        const std::string spokenFile = home + "/elsewhere/spoken.jsonl";
        const std::string spokenPrompt = json({{"type", "user"}, {"message", {{"role", "user"}, {"content", "\xF0\x9F\x8E\x99 wie spaet ist es"}}}}).dump();
        writeFile(spokenFile, end + "\n" + spokenPrompt + "\n" + answer + "\n");
        writeFile(runtime + "/pvai/wspoken.claude", json({{"claudeSession", "spoken-1"}, {"transcript", spokenFile}}).dump() + "\n");
        LineClient spoken;
        spoken.send({{"type", "register"}, {"version", kVersion}, {"id", "wspoken"}, {"cwd", project}, {"label", "spoken"}, {"running", true}, {"restart", true}});
        check(!waitSession(a, "wspoken", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "a spoken turn taken over: busy");
        hook("wspoken", {{"hook_event_name", "Stop"}, {"session_id", "spoken-1"}, {"last_assistant_message", "Kurz vor drei."}});
        const json spokenSay = a.next("say", [](const json &m) { return m.value("session", "") == "wspoken"; });
        check(!spokenSay.empty() && spokenSay.value("voice", false), "its Stop says voice=true: " + spokenSay.dump());
    }

    // Claude Code's login problems: the footer under the prompt box ("Not logged in") and an authentication
    // failure the transcript records, each an attention of kind "login", once per incident. Words that only appear in
    // an answer are none.
    {
        const std::string authFile = home + "/elsewhere/auth.jsonl";
        const std::string endLine = R"({"type":"system","subtype":"turn_duration","isSidechain":false})";
        writeFile(authFile, endLine + "\n");
        writeFile(runtime + "/pvai/wauth.claude", json({{"claudeSession", "auth-1"}, {"transcript", authFile}}).dump() + "\n");
        LineClient wrapperAuth;
        wrapperAuth.send({{"type", "register"}, {"version", kVersion}, {"id", "wauth"}, {"cwd", project}, {"label", "auth"}, {"running", true}, {"restart", true}});
        check(!waitSession(a, "wauth", [](const json &) { return true; }).empty(), "a session for the login checks");
        a.drain();
        auto loginAttention = [&](int ms = 4000) {
            return a.next("attention", [](const json &m) { return m.value("session", "") == "wauth" && m.value("kind", "") == "login"; }, ms);
        };
        auto screen = [&](bool login) {
            wrapperAuth.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"mode", "default"}, {"login", login}});
        };
        auto nowIso = [](int secondsAgo = 0) {
            std::time_t t = std::time(nullptr) - secondsAgo;
            char buffer[40];
            std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&t));
            return std::string(buffer);
        };
        auto line = [](const std::string &uuid, const std::string &at, const std::string &text, const std::string &extra = {}) {
            return std::string(R"({"type":"assistant","uuid":")") + uuid + R"(","timestamp":")" + at + R"(","message":{"content":[{"type":"text","text":")" + text + R"("}]})"
                + extra + "}\n";
        };
        const std::string authFields = R"(,"error":"authentication_failed","isApiErrorMessage":true,"apiErrorStatus":401)";
        auto appendAuth = [&](const std::string &text) { std::ofstream(authFile, std::ios::app) << text; };

        screen(false);
        check(loginAttention(500).empty(), "no login problem on the footer: no attention");
        screen(true);
        const json first = loginAttention();
        check(!first.empty() && first.value("text", "") == "Claude Code is not logged in.", "the footer says it: attention login: " + first.dump());
        screen(true);
        check(loginAttention(500).empty(), "still shown: once per incident");
        screen(false);
        screen(true);
        check(!loginAttention().empty(), "gone and back (a login, then lost again): news again");
        screen(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        a.drain();

        // The transcript: only the structured fields count, and only a line written now.
        appendAuth(line("x1", nowIso(), "If it says Please run /login, run it. Not logged in", ""));
        check(loginAttention(1500).empty(), "an answer that says Please run /login is no login problem");
        appendAuth(line("x2", nowIso(86400), "Failed to authenticate. API Error: 401", authFields));
        check(loginAttention(1500).empty(), "an old authentication failure (a copied history) is none");
        appendAuth(line("x3", nowIso(), "Failed to authenticate. API Error: 401 OAuth access token is invalid.", authFields));
        const json fromFile = loginAttention();
        check(!fromFile.empty() && fromFile.value("text", "") == "Claude Code is not logged in, or its login has expired.",
              "an authentication failure in the transcript: attention login: " + fromFile.dump());
        appendAuth(line("x4", nowIso(), "Failed to authenticate. API Error: 401 again", authFields));
        check(loginAttention(1500).empty(), "the same incident: not again");
        appendAuth(line("x5", nowIso(), "A working answer."));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        appendAuth(line("x6", nowIso(), "Failed to authenticate. API Error: 401 once more", authFields));
        check(!loginAttention().empty(), "a reply in between re-arms it");
        a.drain();
        hook("wauth", {{"hook_event_name", "Stop"}, {"session_id", "auth-1"}, {"last_assistant_message", "x"}});
        a.drain();
    }

    // The permission mode: from the hooks and the footer; a change is
    // Shift+Tab until the footer shows it, once round the cycle at most.
    {
        LineClient mw;
        mw.send({{"type", "register"}, {"version", kVersion}, {"id", "wmode"}, {"cwd", project}, {"label", "mode"}, {"restart", true}, {"verify", true},
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
        LineClient old;
        old.send({{"type", "register"}, {"version", kVersion}, {"id", "wold"}, {"cwd", project}, {"label", "old"}, {"restart", true}, {"verify", true}});
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
        LineClient b;
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
    // The app server holds the account in memory: after a login over an account it knew, it is restarted once
    // no thread works (`codex app-server daemon restart`).
    auto restarts = [&]() {
        for (int i = 0; i < 80; ++i) {
            if (readFile(bin + "/codex.calls").find("app-server daemon restart") != std::string::npos)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    };
    check(restarts(), "a login over a known account restarts the shared app server");
    a.send({{"type", "login"}, {"ref", 39}, {"assistant", "claude"}});
    login = a.next("login", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "code-needed"; });
    check(login.value("url", "").rfind("https://claude.com/cai/oauth/authorize?", 0) == 0, "claude: the link, and it wants the code: " + login.dump());
    a.send({{"type", "login-code"}, {"ref", 40}, {"assistant", "claude"}, {"code", "good#s1"}});
    login = a.next("login", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "done"; }, 5000);
    check(!login.empty(), "the code goes in, the login is done");
    check(!a.next("assistants", [](const json &m) {
              const json list = m.value("assistants", json::array());
              return !list.empty() && list[0].value("loggedIn", json()) == true && list[0].value("account", json()) == "claude.user@example.com";
          }).empty(),
          "claude loggedIn true from claude auth status, account the email it reports");
    a.send({{"type", "login-code"}, {"ref", 41}, {"assistant", "claude"}, {"code", "x"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 41; }).value("reason", "") == "no-login",
          "a code without a login: no-login");

    // Logout: Claude Code's goes through, a second one meanwhile is busy.
    a.send({{"type", "logout"}, {"ref", 42}, {"assistant", "claude"}});
    json logout = a.next("logout", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "running"; });
    check(!logout.empty(), "logout: running: " + logout.dump());
    a.send({{"type", "logout"}, {"ref", 43}, {"assistant", "claude"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 43; }).value("reason", "") == "busy", "a second logout meanwhile: busy");
    logout = a.next("logout", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") != "running"; }, 5000);
    check(logout.value("state", "") == "done" && logout.value("text", "") == "Logged out" && logout.value("ref", 0) == 42,
          "logout: done with the last line: " + logout.dump());
    check(!a.next("assistants", [](const json &m) {
              const json list = m.value("assistants", json::array());
              return !list.empty() && list[0].value("loggedIn", json()) == false;
          }).empty(),
          "claude loggedIn false after the logout");
    a.send({{"type", "logout"}, {"ref", 44}, {"assistant", "claude"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 44; }).value("reason", "") == "not-logged-in",
          "logout without a login: not-logged-in");
    a.send({{"type", "logout"}, {"ref", 45}, {"assistant", "nobody"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 45; }).value("reason", "") == "unknown-assistant",
          "logout of an unknown assistant: unknown-assistant");
    a.send({{"type", "logout"}, {"ref", 46}, {"assistant", "kimi"}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 46; }).value("reason", "") == "not-supported",
          "logout of Kimi: not-supported");
    // Codex's logout: the account goes, and the app server (which keeps it in memory) is restarted.
    fs::remove(bin + "/codex.calls");
    a.send({{"type", "logout"}, {"ref", 47}, {"assistant", "codex"}});
    check(a.next("logout", [](const json &m) { return m.value("assistant", "") == "codex" && m.value("state", "") == "done"; }, 6000).value("state", "") == "done",
          "codex logout done");
    check(restarts(), "and the shared app server is restarted");
    codex.hasAccount = false;
    check(!a.next("assistants", [](const json &m) {
              for (const auto &x : m.value("assistants", json::array()))
                  if (x.value("name", "") == "codex")
                      return x.value("loggedIn", json()) == false && x.value("account", json("?")).is_null();
              return false;
          }, 6000).empty(),
          "assistants: codex loggedIn false, account null");
    a.send({{"type", "login"}, {"ref", 48}, {"assistant", "codex"}});
    check(!a.next("login", [](const json &m) { return m.value("assistant", "") == "codex" && m.value("state", "") == "done"; }, 6000).empty(), "logged in again");

    // ---- Claude token login: the token is a fallback. The decision (account
    // login or not) comes from `auth status` without it; the marker file
    // tells `pvai run`; an inherited token is never touched. The tokens are
    // fake; no message below prints one.
    {
        const std::string tokenFile = configHome + "/pvai/claude-token";
        const std::string markerFile = configHome + "/pvai/claude-token-active";
        const std::string pattern = "abcdefghijklmnopqrstuvwxyz0123456789-_ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        auto mk = [&](const std::string &tag) { return "sk-ant-oat01-" + tag + pattern + pattern.substr(0, 20); };
        const std::string tokSet = mk("SET"), tokNew = mk("NEW"), tokStandby = mk("STANDBY"), tokOk = mk("OK"), tokWrapped = mk("WRAP"),
                          tokFail = mk("FAIL");
        const std::string secretCode = "secretcode#s2";
        const std::string inherited = "sk-ant-oat01-inherited-from-the-environment";
        const std::string url = "https://claude.com/cai/oauth/authorize?code=true&client_id=00000000-1111-2222-3333-444444444444"
                                "&redirect_uri=https%3A%2F%2Fexample.invalid%2Foauth%2Fcode%2Fcallback&scope=user%3Ainference&state=s2";

        LineClient t;
        t.send(app());
        std::string seen; // everything the client heard, to prove what never came
        auto get = [&](const std::string &type, const std::function<bool(const json &)> &pred = {}, int ms = 5000) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            for (;;) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
                if (left <= 0)
                    return json::object();
                const json m = t.next(static_cast<int>(left));
                if (m.empty())
                    return json::object();
                seen += m.dump();
                if (m.value("type", "") == type && (!pred || pred(m)))
                    return m;
            }
        };
        auto refIs = [](int ref) { return [ref](const json &m) { return m.value("ref", 0) == ref; }; };
        auto lines = [&](const std::string &path) {
            std::vector<std::string> out;
            std::istringstream in(readFile(path));
            for (std::string line; std::getline(in, line);)
                out.push_back(line);
            return out;
        };
        auto waitLines = [&](const std::string &path, std::size_t count) {
            for (int i = 0; i < 60 && lines(path).size() < count; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return lines(path);
        };
        auto claudeEntry = [](const json &message) {
            const json list = message.value("assistants", json::array());
            return list.empty() ? json::object() : list[0];
        };
        // The next `assistants` in which claude has that method and `loggedIn`.
        auto method = [&](const std::string &wanted, const json &loggedIn) {
            return get("assistants", [&](const json &m) {
                const json c = claudeEntry(m);
                return c.value("loginMethod", "") == wanted && c.value("loggedIn", json("?")) == loggedIn;
            });
        };
        auto gone = [&](const std::string &id) {
            t.send({{"type", "stop"}, {"ref", 299}, {"session", id}});
            return !get("sessions", [&](const json &m) { return sessionIn(m, id).empty(); }, 8000).empty();
        };
        // `pvai run` as the owner starts it in a terminal, with an inherited login token; what claude saw.
        // `kind`: "claude" a program named claude, "other" a plain `sh`, "binary" a program with
        // another name that PVAI_CLAUDE names (the daemon's way of running it).
        writeFile(home + "/alt/claude", "#!/bin/sh\necho \"${CLAUDE_CODE_OAUTH_TOKEN-unset}\" > \"$1\"; exec sleep 20\n", true);
        writeFile(home + "/alt/agent", "#!/bin/sh\necho \"${CLAUDE_CODE_OAUTH_TOKEN-unset}\" > \"$1\"; exec sleep 20\n", true);
        auto wrapperSaw = [&](const std::string &id, const std::string &inheritedToken, const std::string &kind = "claude") {
            const std::string out = home + "/" + id + ".env";
            fs::remove(out);
            const std::string program = kind == "claude" ? "'" + home + "/alt/claude' '" + out + "'"
                                        : kind == "binary" ? "'" + home + "/alt/agent' '" + out + "'"
                                                           : "sh -c 'echo \"${CLAUDE_CODE_OAUTH_TOKEN-unset}\" > " + out + "; exec sleep 20'";
            (void)std::system(("cd '" + project + "' && " + (kind == "binary" ? "PVAI_CLAUDE='" + home + "/alt/agent' " : std::string())
                               + (inheritedToken.empty() ? "" : "CLAUDE_CODE_OAUTH_TOKEN='" + inheritedToken + "' ") + g_pvai + " run --detached --id " + id
                               + " -- " + program + " >/dev/null 2>&1 &")
                                  .c_str());
            const auto seenLines = waitLines(out, 1);
            (void)gone(id);
            return seenLines.empty() ? std::string("(nothing)") : seenLines[0];
        };
        auto mode = [&](const std::string &path) {
            struct stat st {};
            return ::stat(path.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 0777) : -1;
        };
        auto startedSaw = [&](int ref) {
            fs::remove(bin + "/claude.env");
            t.send({{"type", "start"}, {"ref", ref}, {"assistant", "claude"}, {"cwd", project}});
            const std::string id = get("started", refIs(ref)).value("session", "");
            const auto env = waitLines(bin + "/claude.env", 1);
            return std::make_pair(id, env.empty() ? std::string("(nothing)") : env[0]);
        };

        // F: welcome. The account is not logged in and no token is stored.
        const json hello = get("welcome");
        check(hello.value("tokenLogin", false), "welcome.tokenLogin is true");
        check(claudeEntry(hello).value("loginMethod", "") == "none" && claudeEntry(hello).value("tokenStored", true) == false
                  && !hello["assistants"][1].contains("loginMethod"),
              "claude: loginMethod none, tokenStored false (no account login, no token)");

        // set-token: only the syntax is checked.
        int ref = 200;
        const std::vector<json> bad = {"", "has space", "tab\there", "line\nbreak", std::string(2000, 'x'), "bell\x07", 7, nullptr};
        bool allBad = true;
        for (const json &value : bad) {
            t.send({{"type", "set-token"}, {"ref", ++ref}, {"assistant", "claude"}, {"token", value}});
            allBad = get("rejected", refIs(ref)).value("reason", "") == "bad-token" && allBad;
        }
        t.send({{"type", "set-token"}, {"ref", ++ref}, {"assistant", "claude"}});
        allBad = get("rejected", refIs(ref)).value("reason", "") == "bad-token" && allBad;
        check(allBad && !fs::exists(tokenFile), "set-token rejects empty, spaces, control characters, too long, non-strings and none: bad-token, nothing stored");
        t.send({{"type", "set-token"}, {"ref", ++ref}, {"assistant", "codex"}, {"token", tokSet}});
        check(get("rejected", refIs(ref)).value("reason", "") == "not-supported", "set-token for Codex: not-supported");
        t.send({{"type", "set-token"}, {"ref", ++ref}, {"assistant", "nobody"}, {"token", tokSet}});
        check(get("rejected", refIs(ref)).value("reason", "") == "unknown-assistant", "set-token for an unknown assistant: unknown-assistant");

        // D, account missing: the token is stored, becomes the fallback, and
        // the status runs with it only; claude is not started before the file is.
        fs::remove(bin + "/claude.calls");
        fs::remove(bin + "/claude.statusenv");
        t.send({{"type", "set-token"}, {"ref", 210}, {"assistant", "claude"}, {"token", tokSet}});
        const json stored = get("token-set", refIs(210));
        check(stored.value("assistant", "") == "claude" && stored.size() == 3, "set-token answers token-set with the assistant and nothing else");
        check(mode(tokenFile) == 0600 && readFile(tokenFile) == tokSet + "\n", "the token file is claude-token, mode 0600, with the token");
        std::size_t leftovers = 0;
        for (const auto &entry : fs::directory_iterator(configHome + "/pvai"))
            if (entry.path().filename().string().rfind("claude-token.", 0) == 0)
                ++leftovers;
        check(leftovers == 0, "no temp file stays behind");
        const json entry = claudeEntry(method("token", true));
        check(!entry.empty() && entry.value("tokenStored", false), "account missing: the fallback is active (loginMethod token, loggedIn true, tokenStored)");
        check(entry.contains("account") && entry["account"].is_null(), "and with the token in effect there is no account name");
        check(mode(markerFile) == 0600 && readFile(markerFile).empty(), "the marker claude-token-active exists, empty, mode 0600");
        const auto statusEnv = waitLines(bin + "/claude.statusenv", 1);
        check(statusEnv.size() == 1 && statusEnv[0] == tokSet, "set-token ran the status once, and only with the token (no status without it)");
        const std::string calls = readFile(bin + "/claude.calls");
        check(!calls.empty() && calls.find("before") == std::string::npos,
              "set-token started no claude before the token was stored: every claude run after it saw the file");
        check(calls.find(tokSet) == std::string::npos, "the token is on no command line");

        // B, C: a session the app starts and one in a terminal get it while the fallback is active; a restart reads it again.
        auto [tokSession, started] = startedSaw(211);
        check(!tokSession.empty() && started == tokSet, "fallback active: a started session gets the token in its environment");
        hook(tokSession, {{"hook_event_name", "SessionStart"}, {"session_id", "tok-1"}, {"source", "startup"}});
        check(!waitSession(t, tokSession, [](const json &s) { return s.value("restartable", false) && s.value("state", "") == "idle"; }).empty(),
              "the session is restartable");
        t.send({{"type", "set-token"}, {"ref", 212}, {"assistant", "claude"}, {"token", tokNew}});
        check(!get("token-set", refIs(212)).empty(), "a second set-token replaces the file");
        check(readFile(tokenFile) == tokNew + "\n" && mode(tokenFile) == 0600, "with mode 0600 again");
        t.send({{"type", "restart"}, {"ref", 213}, {"session", tokSession}});
        check(!get("restarting", refIs(213)).empty(), "restarting");
        auto env = waitLines(bin + "/claude.env", 2);
        check(env.size() >= 2 && env[1] == tokNew, "the wrapper's relaunch reads the file again");
        check(gone(tokSession), "the session ends");
        check(wrapperSaw("wtok1", "") == tokNew, "pvai run in a terminal: claude gets the token (marker and file exist)");
        check(wrapperSaw("wtok1b", "", "binary") == tokNew, "pvai run: the program PVAI_CLAUDE names gets the token whatever its name");
        check(wrapperSaw("wtok1c", "", "other") == "unset", "pvai run -- another command: no token, though marker and file exist");
        // A: an inherited token is never replaced.
        check(wrapperSaw("wtok2", inherited) == inherited, "pvai run: an inherited CLAUDE_CODE_OAUTH_TOKEN is left alone, the file does not override it");
        // C: no marker, no token.
        fs::remove(markerFile);
        check(wrapperSaw("wtok3", "") == "unset", "pvai run without the marker: no token, though the file exists");
        writeFile(markerFile, "");
        fs::permissions(markerFile, fs::perms::owner_read | fs::perms::owner_write);

        // A plain account login that prints its link and then waits with no more output reports the link anyway.
        writeFile(bin + "/login.linkonly", "");
        t.send({{"type", "login"}, {"ref", 2130}, {"assistant", "claude"}});
        const json linkOnly = get("login", [](const json &m) { return m.value("ref", 0) == 2130 && !m.value("url", json()).is_null(); });
        check(linkOnly.value("url", "") == "https://claude.com/cai/oauth/authorize?code=true&state=linkonly",
              "account login that prints its link and then waits: the link is reported");
        get("login", [](const json &m) { return m.value("ref", 0) == 2130 && m.value("state", "") == "failed"; }, 8000);
        fs::remove(bin + "/login.linkonly");

        // A login cancelled while it waits for its code: the task ends as cancelled once the
        // process has gone (the daemon never waits for it), and the next login starts at once.
        t.send({{"type", "login"}, {"ref", 2140}, {"assistant", "claude"}});
        get("login", [](const json &m) { return m.value("ref", 0) == 2140 && m.value("state", "") == "code-needed"; });
        t.send({{"type", "login-cancel"}, {"ref", 2141}, {"assistant", "claude"}});
        const json cancelled = get("login", [](const json &m) { return m.value("ref", 0) == 2140 && m.value("state", "") == "failed"; }, 5000);
        check(cancelled.value("error", "") == "cancelled", "a cancelled login ends as cancelled: " + cancelled.dump());
        t.send({{"type", "login"}, {"ref", 2142}, {"assistant", "claude"}});
        check(!get("login", [](const json &m) { return m.value("ref", 0) == 2142 && m.value("state", "") == "code-needed"; }).empty(),
              "and the next login starts");
        t.send({{"type", "login-cancel"}, {"ref", 2143}, {"assistant", "claude"}});
        get("login", [](const json &m) { return m.value("ref", 0) == 2142 && m.value("state", "") == "failed"; }, 5000);

        // The account logs in (a Claude login ends with a status without the token): the fallback goes inactive.
        t.send({{"type", "login"}, {"ref", 214}, {"assistant", "claude"}});
        get("login", [](const json &m) { return m.value("state", "") == "code-needed"; });
        t.send({{"type", "login-code"}, {"ref", 215}, {"assistant", "claude"}, {"code", "good#s1"}});
        check(!get("login", [](const json &m) { return m.value("state", "") == "done"; }, 8000).empty(), "an account login goes through");
        const json standby = claudeEntry(method("account", true));
        check(!standby.empty() && standby.value("tokenStored", false), "account logged in: loginMethod account, the token only stored (standby)");
        check(standby.value("account", json()) == "claude.user@example.com", "the account's name is shown while the token is on standby");
        check(!fs::exists(markerFile), "and the marker is gone");
        const auto afterLogin = lines(bin + "/claude.statusenv");
        check(!afterLogin.empty() && afterLogin.back() == "unset", "the check after the login ran without the token");
        const auto plain = startedSaw(216);
        check(plain.second == "unset", "account logged in: a started session gets no token");
        check(gone(plain.first), "it ends");
        check(wrapperSaw("wtok4", "") == "unset", "pvai run: no token without the marker, the standby token is not used");

        // D, account logged in: stored as standby, nothing runs.
        fs::remove(bin + "/claude.calls");
        fs::remove(bin + "/claude.statusenv");
        t.send({{"type", "set-token"}, {"ref", 217}, {"assistant", "claude"}, {"token", tokStandby}});
        check(!get("token-set", refIs(217)).empty(), "set-token with the account logged in");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        check(readFile(tokenFile) == tokStandby + "\n" && !fs::exists(markerFile) && !fs::exists(bin + "/claude.calls") && !fs::exists(bin + "/claude.statusenv"),
              "stored on standby: no marker, and claude was not started at all");

        // The account logs out again; the check after a setup-token run turns the fallback on.
        fs::remove(bin + "/claude.in");
        writeFile(bin + "/setup-token.url", url + "\n");
        auto runSetup = [&](const std::string &mode, const std::string &tok, int loginRef) {
            writeFile(bin + "/setup-token.mode", mode + "\n");
            writeFile(bin + "/setup-token.tok", tok + "\n");
            t.send({{"type", "login"}, {"ref", loginRef}, {"assistant", "claude"}, {"method", "token"}});
            const json waiting = get("login", [&](const json &m) { return m.value("ref", 0) == loginRef && m.value("state", "") == "code-needed"; });
            check(waiting.value("url", "") == url, "setup-token (" + mode + "): the link comes whole, and it wants the code");
            t.send({{"type", "login-code"}, {"ref", loginRef + 1}, {"assistant", "claude"}, {"code", secretCode}});
            return get("login", [&](const json &m) { return m.value("ref", 0) == loginRef && (m.value("state", "") == "done" || m.value("state", "") == "failed"); }, 8000);
        };
        fs::remove(bin + "/claude.statusenv");
        json ended = runSetup("ok", tokOk, 220);
        check(ended.value("state", "") == "done", "setup-token with a printed token: done");
        check(readFile(tokenFile) == tokOk + "\n" && mode(tokenFile) == 0600, "the recognized token is stored, mode 0600");
        const std::string size = readFile(bin + "/setup-token.size");
        check(size.size() >= 5 && size.find(" 1000") != std::string::npos, "its terminal is very wide: " + size);
        check(!claudeEntry(method("token", true)).empty() && fs::exists(markerFile), "account missing after the login: the fallback is active, the marker is back");
        const auto decided = waitLines(bin + "/claude.statusenv", 2);
        check(decided.size() >= 2 && decided[0] == "unset" && decided[1] == tokOk, "that check ran without the token, then with it");
        ended = runSetup("wrapped", tokWrapped, 230);
        check(ended.value("state", "") == "done" && readFile(tokenFile) == tokWrapped + "\n", "a link and a token wrapped at 80 columns are put together");
        ended = runSetup("none", tokFail, 240);
        check(ended.value("state", "") == "failed" && ended.value("error", "") == "token-not-recognized",
              "setup-token without a recognizable token: failed token-not-recognized");
        check(readFile(tokenFile) == tokWrapped + "\n", "and the stored token stays as it was");
        ended = runSetup("fail", tokFail, 250);
        check(ended.value("state", "") == "failed" && ended.value("error", "") == "setup-token failed", "a failing setup-token: failed, whatever it printed");
        check(readFile(tokenFile) == tokWrapped + "\n", "no token is taken from a failed run");
        t.send({{"type", "login"}, {"ref", 260}, {"assistant", "claude"}, {"method", "carrier-pigeon"}});
        check(get("rejected", refIs(260)).value("reason", "") == "bad-method", "an unknown login method: bad-method");

        // E: logout drops file, marker and decision even when `claude auth logout` fails.
        writeFile(bin + "/logout.fail", "");
        t.send({{"type", "logout"}, {"ref", 270}, {"assistant", "claude"}});
        ended = get("logout", [](const json &m) { return m.value("state", "") != "running"; });
        check(ended.value("state", "") == "failed", "the failing logout reports failed");
        check(!fs::exists(tokenFile) && !fs::exists(markerFile), "and the token file and the marker are gone");
        fs::remove(bin + "/logout.fail");
        const json none = claudeEntry(method("none", false));
        check(!none.empty() && none.value("tokenStored", true) == false, "assistants: none, loggedIn false, no token stored");
        check(wrapperSaw("wtok5", inherited) == inherited, "without a file an inherited token is left alone");
        check(wrapperSaw("wtok6", "") == "unset", "and without one claude gets none");

        // The token reached nothing but the file and claude's environment.
        std::string everything = seen + readFile(bin + "/claude.calls") + readFile(bin + "/claude.args") + readFile(daemonLog);
        bool leaked = false;
        for (const std::string &secret : {tokSet, tokNew, tokStandby, tokOk, tokWrapped, tokFail, secretCode, std::string("sk-ant-oat01-")})
            leaked = leaked || everything.find(secret) != std::string::npos;
        check(!leaked, "no token and no login code in any event, claude argument or the daemon's log");
        check(seen.find("Opening browser") == std::string::npos && seen.find("OAuth token") == std::string::npos && seen.find("Store this") == std::string::npos,
              "setup-token's raw output never went to an app");
        t.drain();
        a.drain();
        fs::remove(bin + "/claude.args"); // the sessions above are not the ones later tests count
        fs::remove(bin + "/claude.env");
    }

    // A session the app started that ends at once: the app hears why.
    {
        LineClient early;
        early.send({{"type", "register"}, {"version", kVersion}, {"id", "wearly"}, {"cwd", project}, {"label", "early"}, {"detached", true}, {"restart", true}});
        check(!waitSession(a, "wearly", [](const json &) { return true; }).empty(), "a detached session registers");
        early.send({{"type", "exited"}, {"status", 1}, {"screen", "line 1\nNot logged in \xC2\xB7 Please run /login"}});
        const json ended = a.next("attention", [](const json &m) { return m.value("session", "") == "wearly"; });
        check(ended.value("kind", "") == "ended" && ended.value("text", "") == "line 1\nNot logged in \xC2\xB7 Please run /login",
              "an early end is an attention with the last lines: " + ended.dump());
    }
    {
        LineClient stopped;
        stopped.send({{"type", "register"}, {"version", kVersion}, {"id", "wstop"}, {"cwd", project}, {"label", "stop"}, {"detached", true}, {"restart", true}});
        check(!waitSession(a, "wstop", [](const json &) { return true; }).empty(), "another detached session");
        a.send({{"type", "stop"}, {"ref", 35}, {"session", "wstop"}});
        check(stopped.next("terminate").value("type", "") == "terminate", "stop terminates it");
        stopped.send({{"type", "exited"}, {"status", 129}, {"screen", "bye"}});
        check(a.next("attention", [](const json &m) { return m.value("session", "") == "wstop"; }, 600).empty(),
              "a stopped session's end says nothing");
    }

    // 0.5.52: a session that ends with a prompt not taken tells every app,
    // with the whole text.
    {
        const std::string longText = std::string(300, 'x') + " the end";
        {
            LineClient dying;
            dying.send({{"type", "register"}, {"version", kVersion}, {"id", "wdrop"}, {"cwd", project}, {"label", "drop"}, {"restart", true}, {"verify", true}});
            check(!waitSession(a, "wdrop", [](const json &) { return true; }).empty(), "dropped: a session registers");
            a.send({{"type", "prompt"}, {"ref", 700}, {"session", "wdrop"}, {"text", longText}});
            check(!waitSession(a, "wdrop", [](const json &s) { return s.value("queued", 0) == 1; }).empty(), "dropped: one prompt waits");
        }
        const json dropped = a.next("dropped", [](const json &m) { return !m.value("prompts", json::array()).empty(); });
        const json first = dropped.value("prompts", json::array()).empty() ? json::object() : dropped["prompts"][0];
        check(first.value("session", "") == "wdrop" && first.value("ref", 0) == 700 && first.value("text", "") == longText
                  && !first.contains("kept"),
              "a session without a conversation id ends: dropped, full text, no kept: " + dropped.dump());
    }

    // 0.5.52: what a Claude session with a known conversation did not take is
    // kept; a session that goes on with that conversation queues it again.
    {
        auto droppedOf = [&](int ref) {
            return a.next("dropped", [&](const json &m) {
                for (const auto &p : m.value("prompts", json::array()))
                    if (p.value("ref", 0) == ref)
                        return true;
                return false;
            });
        };
        {
            LineClient first;
            first.send({{"type", "register"}, {"version", kVersion}, {"id", "wkeep1"}, {"cwd", project}, {"label", "keep1"}, {"restart", true}, {"verify", true}});
            hook("wkeep1", {{"hook_event_name", "SessionStart"}, {"session_id", "k-1"}, {"source", "startup"}});
            hook("wkeep1", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "k-1"}, {"prompt", "busy now"}});
            check(!waitSession(a, "wkeep1", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "kept: the session is busy");
            a.send({{"type", "prompt"}, {"ref", 710}, {"session", "wkeep1"}, {"text", "first later"}});
            a.send({{"type", "prompt"}, {"ref", 711}, {"session", "wkeep1"}, {"text", "second later"}, {"voice", true}});
            check(!waitSession(a, "wkeep1", [](const json &s) { return s.value("queued", 0) == 2; }).empty(), "kept: two prompts wait");
        }
        const json dropped = droppedOf(710);
        const json list = dropped.value("prompts", json::array());
        check(list.size() == 2 && list[0].value("ref", 0) == 710 && list[0].value("text", "") == "first later" && list[0].value("kept", false)
                  && list[1].value("ref", 0) == 711 && list[1].value("kept", false),
              "a session with a conversation id ends: dropped with kept: " + dropped.dump());
        LineClient second;
        second.send({{"type", "register"}, {"version", kVersion}, {"id", "wkeep2"}, {"cwd", project}, {"label", "keep2"}, {"restart", true}, {"verify", true}});
        check(!waitSession(a, "wkeep2", [](const json &) { return true; }).empty(), "kept: a new wrapper registers");
        hook("wkeep2", {{"hook_event_name", "SessionStart"}, {"session_id", "k-1"}, {"source", "resume"}});
        check(!waitSession(a, "wkeep2", [](const json &s) { return s.value("queued", 0) == 2; }).empty(),
              "resuming that conversation queues both again");
        json typing = second.next("type");
        check(typing.value("text", "") == "first later", "the first goes in first: " + typing.dump());
        second.send({{"type", "typed"}, {"ok", true}});
        hook("wkeep2", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "k-1"}, {"prompt", "first later"}});
        const json delivered = a.next("delivered", [](const json &m) { return m.value("session", "") == "wkeep2"; });
        check(delivered.value("ref", 0) == 710, "and is delivered with its old ref: " + delivered.dump());
        hook("wkeep2", {{"hook_event_name", "Stop"}, {"session_id", "k-1"}, {"last_assistant_message", "ok"}});
        typing = second.next("type");
        check(typing.value("text", "").find("second later") != std::string::npos, "then the second: " + typing.dump());
        second.send({{"type", "typed"}, {"ok", true}});
        hook("wkeep2", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "k-1"}, {"prompt", "second later"}});
        check(a.next("delivered", [](const json &m) { return m.value("session", "") == "wkeep2"; }).value("ref", 0) == 711, "the second too");

        // A forgotten conversation drops what was kept for it, for good.
        writeFile(transcripts + "/k-f.jsonl", userLine("x"));
        {
            LineClient third;
            third.send({{"type", "register"}, {"version", kVersion}, {"id", "wkeep3"}, {"cwd", project}, {"label", "keep3"}, {"restart", true}, {"verify", true}});
            hook("wkeep3", {{"hook_event_name", "SessionStart"}, {"session_id", "k-f"}, {"source", "startup"}});
            hook("wkeep3", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "k-f"}, {"prompt", "busy"}});
            a.send({{"type", "prompt"}, {"ref", 720}, {"session", "wkeep3"}, {"text", "lost soon"}});
            check(!waitSession(a, "wkeep3", [](const json &s) { return s.value("queued", 0) == 1; }).empty(), "forget: a prompt waits");
        }
        check(droppedOf(720).value("prompts", json::array())[0].value("kept", false), "forget: it is kept");
        a.send({{"type", "forget"}, {"ref", 721}, {"assistant", "claude"}, {"cwd", project}, {"id", "k-f"}});
        const json gone = droppedOf(720);
        const json goneList = gone.value("prompts", json::array());
        check(goneList.size() == 1 && goneList[0].value("text", "") == "lost soon" && !goneList[0].contains("kept"),
              "forgetting the conversation drops it for good, without kept: " + gone.dump());
    }

    // At most 16 prompts are kept per conversation: the oldest go first.
    {
        {
            LineClient cap;
            cap.send({{"type", "register"}, {"version", kVersion}, {"id", "wcap"}, {"cwd", project}, {"label", "cap"}, {"restart", true}, {"verify", true}});
            hook("wcap", {{"hook_event_name", "SessionStart"}, {"session_id", "k-cap"}, {"source", "startup"}});
            hook("wcap", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "k-cap"}, {"prompt", "busy"}});
            for (int i = 0; i < 17; ++i)
                a.send({{"type", "prompt"}, {"ref", 800 + i}, {"session", "wcap"}, {"text", "p" + std::to_string(i)}});
            check(!waitSession(a, "wcap", [](const json &s) { return s.value("queued", 0) == 17; }).empty(), "cap: 17 prompts wait");
        }
        const json dropped = a.next("dropped", [](const json &m) { return m.value("prompts", json::array()).size() == 17; });
        int kept = 0;
        for (const auto &p : dropped.value("prompts", json::array()))
            kept += p.value("kept", false) ? 1 : 0;
        check(kept == 16 && !dropped["prompts"][0].contains("kept") && dropped["prompts"][16].value("kept", false),
              "only 16 are kept, the oldest is not: " + std::to_string(kept));
    }

    // 0.5.52: the wrapper's version, and what an old wrapper cannot do.
    {
        LineClient none, old, now, ancient;
        none.send({{"type", "register"}, {"id", "wv-none"}, {"cwd", project}, {"label", "none"}, {"restart", true}, {"verify", true}});
        old.send({{"type", "register"}, {"version", "0.5.44"}, {"id", "wv-old"}, {"cwd", project}, {"label", "old"}, {"restart", true}, {"verify", true}});
        now.send({{"type", "register"}, {"version", kVersion}, {"id", "wv-now"}, {"cwd", project}, {"label", "now"}, {"restart", true}});
        const json all = a.next("sessions", [](const json &m) {
            return !sessionIn(m, "wv-none").empty() && !sessionIn(m, "wv-old").empty() && !sessionIn(m, "wv-now").empty();
        });
        const json noVersion = sessionIn(all, "wv-none");
        check(noVersion.contains("wrapperVersion") && noVersion["wrapperVersion"].is_null() && noVersion.value("stale", false),
              "a wrapper without a version: wrapperVersion null, stale: " + noVersion.dump());
        const json oldOne = sessionIn(all, "wv-old");
        check(oldOne.value("wrapperVersion", "") == "0.5.44" && oldOne.value("stale", false),
              "an older version is shown and stale: " + oldOne.dump());
        const json current = sessionIn(all, "wv-now");
        check(current.value("wrapperVersion", "") == kVersion && !current.contains("stale"),
              "the daemon's own version is not stale: " + current.dump());
        // A wrapper that reports a version before kMinWrapperVersion is refused; a missing one is served.
        hook("wv-none", {{"hook_event_name", "SessionStart"}, {"session_id", "wv-1"}, {"source", "startup"}});
        hook("wv-none", {{"hook_event_name", "PermissionRequest"}, {"session_id", "wv-1"}, {"tool_name", "Bash"}, {"tool_input", {{"command", "ls"}}}});
        check(!waitSession(a, "wv-none", [](const json &s) { return s.value("state", "") == "waiting"; }).empty(), "stale: a dialog waits");
        a.send({{"type", "answer"}, {"ref", 900}, {"session", "wv-none"}, {"choice", "allow"}});
        check(a.next("answered", [](const json &m) { return m.value("ref", 0) == 900; }).value("session", "") == "wv-none",
              "an answer to a wrapper without a version is served");
        ancient.send({{"type", "register"}, {"version", "0.5.43"}, {"id", "wv-43"}, {"cwd", project}, {"label", "old43"}, {"restart", true}, {"verify", true}});
        check(!waitSession(a, "wv-43", [](const json &s) { return s.value("stale", false); }).empty(), "0.5.43 registers, stale");
        for (const char *type : {"answer", "mode", "model", "restart", "clearInput"}) {
            a.send({{"type", type}, {"ref", 901}, {"session", "wv-43"}, {"mode", "plan"}, {"model", "opus"}, {"choice", "allow"}});
            check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 901; }).value("reason", "") == "stale-wrapper",
                  std::string(type) + " to a wrapper of 0.5.43 is rejected stale-wrapper");
        }
        a.send({{"type", "answer"}, {"ref", 902}, {"session", "wv-old"}, {"choice", "allow"}});
        check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 902; }).value("reason", "") == "no-dialog",
              "0.5.44 is new enough: the answer reaches the session");
        a.send({{"type", "prompt"}, {"ref", 903}, {"session", "wv-none"}, {"text", "still typed"}});
        check(a.next("accepted", [](const json &m) { return m.value("ref", 0) == 903; }).value("session", "") == "wv-none",
              "a prompt goes through to a stale wrapper");
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
    // A spoken prompt: what follows without the owner typing is spoken too.
    // (Each Stop with its own text: the same event twice counts once.)
    int turn = 0;
    // A Stop alone while idle: a turn the transcript read has not shown yet.
    // (Apart from the last Stop: the same hook twice within 500 ms is one.)
    auto saidUnseen = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok " + std::to_string(++turn)}});
        const json voice = a.next("say", [](const json &m) { return m.value("session", "") == "wterm"; }).value("voice", json());
        a.drain();
        return voice;
    };
    json lastSay; // the `say` of the last saidAnswer
    auto saidAnswer = [&](const std::string &prompt, const std::string &answer) {
        if (!prompt.empty())
            hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", prompt}});
        hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", answer}});
        lastSay = a.next("say", [](const json &m) { return m.value("session", "") == "wterm"; });
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "Stop ends the turn");
        a.drain(); // later checks read the states that follow, not these
        return lastSay.value("voice", json());
    };
    auto said = [&](const std::string &prompt) { return saidAnswer(prompt, "ok " + std::to_string(++turn)); };
    check(said("\xF0\x9F\x8E\x99 hör zu") == json(true), "a spoken turn is spoken");
    check(said("[Message p1 from M4/w1, released by the owner. Answer with: ...]\nhi") == json(true),
          "an agent message after it is spoken too");
    check(said("Another Claude session sent a message:\n<agent-message from=\"a1\">\ndone\n</agent-message>") == json(true),
          "a subagent's hand-back after it is spoken too");
    check(said("<task-notification>\n<status>completed</status>\n</task-notification>") == json(true),
          "a task notification through UserPromptSubmit is spoken too");
    check(said("<system-reminder>\nnote</system-reminder>") == json(true), "a machine tag through UserPromptSubmit leaves the voice state");
    check(saidUnseen() == json(true), "a turn whose Stop comes before the transcript shows it is spoken too");
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
    { const json v = said(""); check(v == json(true), "and so is the turn a task notification started: " + v.dump()); }
    check(said("typed") == json(false), "a typed prompt ends that");
    check(lastSay.value("text", "") == "ok " + std::to_string(turn), "a typed turn without a 🔊 line: voice=false, the first sentence as text");
    check(saidAnswer("typed again", "Done.\n\xF0\x9F\x94\x8A Fertig.") == json(true) && lastSay.value("text", "") == "Fertig.",
          "a typed turn with a 🔊 line: voice=true, the line as text");
    check(saidAnswer("\xF0\x9F\x8E\x99 spoken", "Done.\n\xF0\x9F\x94\x8A Fertig.") == json(true) && lastSay.value("text", "") == "Fertig.",
          "a spoken turn with a 🔊 line: voice=true as before");
    check(saidAnswer("typed once more", "Only an answer. Second sentence.") == json(false) && lastSay.value("text", "") == "Only an answer.",
          "a typed turn without one: voice=false, the first sentence");
    check(saidUnseen() == json(false), "also for a turn whose Stop comes before the transcript shows it");
    check(said("[Message p2 from M4/w1, released by the owner. Answer with: ...]\nhi") == json(false),
          "and an agent message after a typed prompt is not spoken");
    check(said("Another Claude session sent a message:\n<agent-message from=\"a2\">\ndone\n</agent-message>") == json(false),
          "nor a subagent's hand-back after a typed prompt");
    // Claude Code stores a long delivered message as a paste.
    const std::string frame = "\n\n<pasted_content id=\"8916\">\n[Message p690cecbc from M4/w3c, released by the owner. Answer with: ...]\nhi\n</pasted_content id=\"8916\">";
    check(said("\xF0\x9F\x8E\x99 voice again") == json(true), "a spoken prompt");
    a.send({{"type", "prompt"}, {"ref", 20}, {"session", "wterm"}, {"text", "hi"}, {"from", "M4/w3c"}, {"fromLabel", "x"}});
    check(wrapper.next("type").value("text", "").rfind("[Message ", 0) == 0, "a daemon-typed agent message goes in");
    hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", frame}});
    check(said("") == json(true), "daemon-typed, confirmed as a paste: the voice state is unchanged");
    check(said(frame) == json(true), "terminal-typed paste frame around [Message: unchanged (text fallback)");
    check(said("\n\n<pasted_content id=\"1\">\nplain words\n</pasted_content id=\"1\">") == json(false),
          "terminal-typed plain text in a frame ends it");
    check(said("typed again") == json(false), "plain terminal text is not spoken");
    // A subagent's hand-back written as a meta line, no UserPromptSubmit: it
    // starts a turn the daemon sees only in the transcript.
    const std::string handBack =
        R"({"type":"user","isMeta":true,"message":{"role":"user","content":"Another Claude session sent a message:\n<agent-message from=\"a3\">\ndone\n</agent-message>"}})";
    auto handBackTurn = [&](const std::string &uuid) {
        append(handBack);
        std::string line = answerLine;
        line.replace(line.find("u-a1"), 4, uuid);
        append(line);
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "busy"; }, 3000).empty(),
              "a meta hand-back and its answer make the idle session busy");
    };
    auto stopSaid = [&]() {
        // Claude Code writes the turn's end into the file before the Stop hook runs.
        append(R"({"type":"system","subtype":"turn_duration","isSidechain":false})");
        std::this_thread::sleep_for(std::chrono::milliseconds(600)); // apart from the last Stop
        hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok " + std::to_string(++turn)}});
        const json voice = a.next("say", [](const json &m) { return m.value("session", "") == "wterm"; }).value("voice", json());
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "Stop ends the turn");
        a.drain();
        return voice;
    };
    auto unpromptedPermission = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        hook("wterm", {{"hook_event_name", "PermissionRequest"}, {"tool_name", "Bash"}, {"tool_input", {{"command", "ls"}}}});
        a.next("attention");
        hook("wterm", {{"hook_event_name", "Notification"}, {"notification_type", "permission_prompt"}, {"message", "Claude needs your permission"}});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        a.drain();
    };
    check(said("typed") == json(false), "a typed prompt");
    handBackTurn("u-h0");
    { const json v = stopSaid(); check(v == json(false), "a meta hand-back turn after a typed prompt is not spoken: " + v.dump()); }
    unpromptedPermission();
    check(stopSaid() == json(false), "nor a turn that waited for a permission after a typed prompt");
    check(said("\xF0\x9F\x8E\x99 spoken") == json(true), "a spoken prompt");
    handBackTurn("u-h1");
    check(stopSaid() == json(true), "a meta hand-back turn after a spoken prompt is spoken");
    check(said("\xF0\x9F\x8E\x99 spoken again") == json(true), "a spoken prompt");
    unpromptedPermission();
    check(stopSaid() == json(true), "a turn that waited for a permission after a spoken prompt is spoken");
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

    // The usage limit: Claude Code writes the 429 as an assistant line and a
    // turn end, then goes on by itself at the reset (a dequeue line, no hook).
    {
        const long long resetsAt = static_cast<long long>(std::time(nullptr)) + 3600;
        const json limitLine = {{"type", "assistant"}, {"uuid", "u-lim"}, {"timestamp", "2026-11-02T19:21:52.424Z"},
                                {"message", {{"model", "<synthetic>"}, {"role", "assistant"},
                                             {"content", json::array({{{"type", "text"}, {"text", "You've hit your 5-hour limit \xC2\xB7 resets 11:20pm (Europe/Zurich)"}}})}}},
                                {"apiError", "usage_limit_reached"}, {"error", "rate_limit"}, {"isApiErrorMessage", true}, {"apiErrorStatus", 429},
                                {"quotaLimits", {{"status", "rejected"}, {"resetsAt", resetsAt}, {"rateLimitType", "five_hour"}}}};
        const std::string info = R"({"type":"system","subtype":"informational","content":"Usage limit reached"})";
        const std::string end = R"({"type":"system","subtype":"turn_duration","durationMs":1165})";
        const std::string dequeue = R"({"type":"queue-operation","operation":"dequeue","timestamp":"2026-11-02T22:21:22.796Z"})";
        auto isLimited = [](const json &s) { return sub(s, "limited").value("type", "") == "five_hour"; };
        a.drain();
        hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "long job"}});
        append(limitLine.dump());
        append(info);
        append(end);
        const json attention = a.next("attention", [](const json &m) { return m.value("kind", "") == "limited"; }, 3000);
        check(attention.value("session", "") == "wterm" && attention.value("text", "").find("5-hour limit") != std::string::npos,
              "attention limited: " + attention.dump());
        term = waitSession(a, "wterm", isLimited);
        check(!term.empty() && term.value("state", "") == "busy" && term["limited"].value("resetsAt", 0LL) == resetsAt
                  && term["limited"].value("text", "").find("resets 11:20pm") != std::string::npos,
              "the limit line: limited set, still busy: " + term.dump());
        check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 4500).empty(),
              "a turn end while limited does not end the busy state");
        append(dequeue);
        term = waitSession(a, "wterm", [](const json &s) { return s["limited"].is_null(); });
        check(!term.empty() && term.value("state", "") == "busy", "the dequeue: limited null, still busy: " + term.dump());
        hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "done"}});
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle" && s["limited"].is_null(); }).empty(),
              "then Stop ends the turn");
        // A Stop that arrives while the limit waits changes nothing.
        hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "again"}});
        append(limitLine.dump());
        check(!waitSession(a, "wterm", isLimited).empty(), "limited again");
        hook("wterm", {{"hook_event_name", "Stop"}, {"last_assistant_message", "x"}});
        check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 1200).empty(), "a Stop during the limit: still busy");
        hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "typed again"}});
        term = waitSession(a, "wterm", [](const json &s) { return s["limited"].is_null() && s.value("state", "") == "busy"; });
        check(!term.empty(), "a prompt of the owner's ends the wait");
        // A turn end and no Stop: idle after a moment.
        append(answerLine);
        append(end);
        check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 1200).empty(), "a turn end: not idle at once");
        check(!waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 5000).empty(),
              "but idle after 3 s without a Stop");
    }

    // A model's own quota (Fable's) in the running turn: idle at once, one
    // attention, and nothing held busy until the reset.
    {
        const long long resetsAt = static_cast<long long>(std::time(nullptr)) + 3600;
        char nowIso[32];
        const std::time_t nowT = std::time(nullptr);
        std::strftime(nowIso, sizeof nowIso, "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&nowT));
        auto fableAt = [&](const std::string &uuid, const std::string &stamp) {
            return json{{"type", "assistant"}, {"uuid", uuid}, {"timestamp", stamp},
                                {"message", {{"model", "<synthetic>"}, {"role", "assistant"},
                                             {"content", json::array({{{"type", "text"}, {"text", "You've reached your Fable limit. Run /usage-credits to continue or switch models with /model."}}})}}},
                                {"apiError", "model_requires_usage_credits"}, {"error", "rate_limit"}, {"isApiErrorMessage", true}, {"apiErrorStatus", 429},
                                {"quotaLimits", nullptr},
                                {"apiErrorParams", {{"rate_limit_info", {{"status", "rejected"}, {"resetsAt", resetsAt}, {"rateLimitType", "seven_day_overage_included"}}}}}};
            };
        const json fableLine = fableAt("u-fable", nowIso);
        a.drain();
        hook("wterm", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "fable job"}});
        // A copied history line from a fork is no news: no attention for it.
        append(fableAt("u-fable-old", "2020-01-01T00:00:00.000Z").dump());
        check(a.next("attention", [](const json &m) { return m.value("kind", "") == "limited"; }, 1500).empty(), "a copied old limit line is no attention");
        append(fableLine.dump());
        const json fable = a.next("attention", [](const json &m) { return m.value("kind", "") == "limited"; }, 3000);
        check(fable.value("session", "") == "wterm" && fable.value("text", "").find("Fable limit") != std::string::npos,
              "a Fable limit line in the running turn: attention limited: " + fable.dump());
        term = waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "idle"; }, 3000);
        check(!term.empty() && term["limited"].is_null(), "the session is idle at once, limited null: " + term.dump());
        check(a.next("attention", [](const json &m) { return m.value("kind", "") == "limited"; }, 2500).empty(), "attention limited comes once");
        check(waitSession(a, "wterm", [](const json &s) { return s.value("state", "") == "busy"; }, 1500).empty(),
              "nothing holds the session busy until the reset");
    }

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
    // No login on this host (the cached `claude auth status`), no token: the start is refused, and nothing is run for it.
    fs::remove(bin + "/claude.calls");
    a.send({{"type", "start"}, {"ref", 1200}, {"assistant", "claude"}, {"cwd", project}});
    check(a.next("rejected", [](const json &m) { return m.value("ref", 0) == 1200; }).value("reason", "") == "not-logged-in",
          "claude without a login: rejected not-logged-in");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(readFile(bin + "/claude.calls").empty(), "and no claude (not even a status run) was started for it");
    a.send({{"type", "login"}, {"ref", 1201}, {"assistant", "claude"}});
    a.next("login", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "code-needed"; }, 6000);
    a.send({{"type", "login-code"}, {"ref", 1202}, {"assistant", "claude"}, {"code", "good#s1"}});
    check(!a.next("login", [](const json &m) { return m.value("assistant", "") == "claude" && m.value("state", "") == "done"; }, 6000).empty(), "claude logs in");
    check(!a.next("assistants", [](const json &m) {
              const json list = m.value("assistants", json::array());
              return !list.empty() && list[0].value("loggedIn", json()) == true;
          }, 6000).empty(), "assistants: loggedIn true");
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

    // 0.5.61: a detached session under an older pvai run: the restart ends
    // that wrapper, and one of the installed pvai takes over the same id
    // and conversation.
    fs::remove(bin + "/claude.args");
    {
        LineClient old;
        old.send({{"type", "register"}, {"version", "0.5.56"}, {"id", "wold"}, {"cwd", project}, {"label", "old"},
                  {"detached", true}, {"restart", true}, {"verify", true}});
        hook("wold", {{"hook_event_name", "SessionStart"}, {"session_id", "c0ffee00-1111-2222-3333-444455556666"}, {"source", "startup"}});
        check(!waitSession(a, "wold", [](const json &s) {
                  return s.value("state", "") == "idle" && s.value("stale", false) && s.value("restartable", false);
              }).empty(), "stale: a detached session under pvai run 0.5.56");
        a.send({{"type", "restart"}, {"ref", 23}, {"session", "wold"}});
        check(a.next("restarting").value("session", "") == "wold", "stale: restarting");
        check(!old.next("terminate").empty(), "stale: the old wrapper is ended, not asked to restart claude");
    } // it goes
    json taken = waitSession(a, "wold", [](const json &s) { return s.value("wrapperVersion", "") == kVersion; }, 5000);
    check(taken.value("detached", false) && taken.value("label", "") == "old",
          "stale: a pvai run of this version takes the same id: " + taken.dump());
    std::string oldArgs;
    for (int i = 0; i < 50 && oldArgs.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        oldArgs = readFile(bin + "/claude.args");
    }
    check(oldArgs.find("--resume c0ffee00-1111-2222-3333-444455556666") != std::string::npos,
          "stale: with the same conversation: " + oldArgs);
    a.send({{"type", "stop"}, {"ref", 24}, {"session", "wold"}});
    check(!a.next("sessions", [&](const json &m) { return sessionIn(m, "wold").empty(); }, 5000).empty(), "stale: the new one stops");

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

    // ---- Renaming a Claude session: the wrapper learns the label and registers with it again.
    {
        const std::string longLabel(65, 'x');
        auto w1 = std::make_unique<LineClient>();
        LineClient w2;
        w1->send({{"type", "register"}, {"version", kVersion}, {"id", "wren"}, {"cwd", project}, {"label", "alpha"}});
        w2.send({{"type", "register"}, {"version", kVersion}, {"id", "wren2"}, {"cwd", project}, {"label", "beta"}});
        check(!waitSession(a, "wren2", [](const json &) { return true; }).empty(), "rename: two sessions");
        auto reason = [&](int ref, const std::string &session, const json &label) {
            a.send({{"type", "rename"}, {"ref", ref}, {"session", session}, {"label", label}});
            return a.next("rejected", [&](const json &m) { return m.value("ref", 0) == ref; }, 1000).value("reason", "");
        };
        check(reason(100, "nope", "x") == "unknown-session", "rename: unknown-session");
        check(reason(101, "wren", "") == "bad-label", "rename: empty is bad-label");
        check(reason(102, "wren", " \t\n ") == "bad-label", "rename: blank is bad-label");
        check(reason(103, "wren", longLabel) == "bad-label", "rename: 65 bytes is bad-label");
        check(reason(104, "wren", "a\nb") == "bad-label", "rename: a newline inside is bad-label");
        check(reason(105, "wren", "a\x01" "b") == "bad-label", "rename: a control char is bad-label");
        check(reason(106, "wren", "beta") == "taken", "rename: another session's label is taken");
        check(reason(107, "wren", json(7)) == "bad-label", "rename: a non-string label is bad-label");
        a.send({{"type", "rename"}, {"ref", 108}, {"session", "wren"}, {"label", "alpha"}});
        check(a.next("renamed", [](const json &m) { return m.value("ref", 0) == 108; }).value("label", "") == "alpha", "rename: its own label is a no-op success");
        a.send({{"type", "rename"}, {"ref", 109}, {"session", "wren"}, {"label", std::string(64, 'y')}});
        check(!a.next("renamed", [](const json &m) { return m.value("ref", 0) == 109; }).empty(), "rename: 64 bytes are fine");
        a.send({{"type", "rename"}, {"ref", 110}, {"session", "wren"}, {"label", " gamma "}});
        const json ok = a.next("renamed", [](const json &m) { return m.value("ref", 0) == 110; });
        check(ok.value("session", "") == "wren" && ok.value("label", "") == "gamma", "rename: renamed, trimmed: " + ok.dump());
        check(!waitSession(a, "wren", [](const json &s) { return s.value("label", "") == "gamma"; }).empty(), "rename: the sessions list has it");
        const json pushed = w1->next("label", [](const json &m) { return m.value("label", "") == "gamma"; });
        check(!pushed.empty(), "rename: the wrapper is told the new label");
        // The wrapper registers again after a daemon restart with what it was told.
        w1 = std::make_unique<LineClient>(); // the old connection closes
        w1->send({{"type", "register"}, {"version", kVersion}, {"id", "wren"}, {"cwd", project}, {"label", "gamma"}, {"running", true}});
        check(!waitSession(a, "wren", [](const json &s) { return s.value("label", "") == "gamma"; }).empty(), "rename: a re-registered wrapper keeps it");
        {
            LineClient b;
            b.send(app());
            check(sessionIn(b.next("sessions"), "wren2").value("label", "") == "beta", "rename: the other session is untouched");
        }
        const json labels = json::parse(readFile(configHome + "/pvai/labels.json"), nullptr, false);
        check(!labels.is_object() || !labels.contains("claude/wren"), "rename: Claude sessions are not in labels.json");
        LineClient fresh;
        fresh.send(app());
        check(fresh.next("welcome").value("rename", false), "welcome.rename");
    }

    // Null or wrongly typed fields in a hook, a transcript and a subagent's
    // meta file (written by other programs) end nothing: the state change
    // always happens and the daemon keeps running.
    {
        const std::string nullRun = home + "/elsewhere/nul-run.jsonl";
        writeFile(nullRun,
                  R"({"type":null,"isSidechain":null,"isMeta":null,"message":null})" "\n"
                  R"({"type":"user","isSidechain":"no","timestamp":null,"message":{"content":[{"type":"text","text":null}]}})" "\n");
        LineClient nw;
        nw.send({{"type", "register"}, {"version", kVersion}, {"id", "wnull"}, {"cwd", project}, {"label", "nulls"}});
        check(!waitSession(a, "wnull", [](const json &s) { return s.value("state", "") == "starting"; }).empty(), "null fields: registered");
        hook("wnull", {{"hook_event_name", "SessionStart"}, {"session_id", "nul-1"}, {"source", "startup"}, {"transcript_path", nullRun}});
        check(!waitSession(a, "wnull", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "null fields: SessionStart");
        hook("wnull", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "nul-1"}, {"prompt", nullptr}, {"permission_mode", 4},
                       {"transcript_path", nullptr}, {"source", nullptr}});
        check(!waitSession(a, "wnull", [](const json &s) { return s.value("state", "") == "busy"; }).empty(),
              "null fields: UserPromptSubmit still makes it busy");
        hook("wnull", {{"hook_event_name", "PermissionRequest"}, {"session_id", nullptr}, {"tool_name", nullptr}, {"tool_input", nullptr}});
        check(!waitSession(a, "wnull", [](const json &s) { return s.value("state", "") == "waiting"; }).empty(),
              "null fields: PermissionRequest still waits");
        hook("wnull", {{"hook_event_name", "Notification"}, {"notification_type", 3}, {"message", nullptr}});
        hook("wnull", {{"hook_event_name", "Stop"}, {"session_id", "nul-1"}, {"last_assistant_message", nullptr}});
        check(!waitSession(a, "wnull", [](const json &s) { return s.value("state", "") == "idle"; }).empty(),
              "null fields: Stop still ends the turn");
        writeFile(home + "/elsewhere/nul-run/subagents/agent-n1.jsonl", "{}\n");
        writeFile(home + "/elsewhere/nul-run/subagents/agent-n1.meta.json",
                  R"({"agentType":null,"description":5,"requestShape":null,"finished":null})");
        const json listed = waitSession(a, "wnull", [&](const json &s) { return agents(s).size() == 1; }, 4000);
        check(!listed.empty() && agents(listed)[0].value("id", "") == "n1" && agents(listed)[0].value("type", "x").empty(),
              "null fields in a subagent's meta: listed with empty fields: " + listed.dump());
        hook("wnull", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "nul-1"}, {"prompt", "still working"}});
        check(!waitSession(a, "wnull", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "the session works after all of it");
        LineClient after;
        after.send(app());
        check(after.next("welcome").value("type", "") == "welcome", "and the daemon answers");
    }

    // A slash or shell command that writes no output and fires no hook ends
    // when the screen shows a free prompt and no work for a while (the
    // times are divided by PVAI_QUIET_DIV); work on the screen keeps it
    // pending. A limit without a reset time ends the same way.
    {
        LineClient qw;
        qw.send({{"type", "register"}, {"version", kVersion}, {"id", "wquiet"}, {"cwd", project}, {"label", "quiet"}, {"restart", true}, {"verify", true},
                 {"clear", true}, {"submitCheck", true}});
        const std::string quietTr = home + "/quiet-tr/quiet-1.jsonl";
        writeFile(quietTr, userLine("start"));
        hook("wquiet", {{"hook_event_name", "SessionStart"}, {"session_id", "quiet-1"}, {"source", "startup"}, {"transcript_path", quietTr}});
        check(!waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "silent command: a session");
        auto screen = [&](bool working) { qw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"working", working}}); };
        a.send({{"type", "prompt"}, {"ref", 150}, {"session", "wquiet"}, {"text", "/foo"}});
        check(qw.next("type").value("text", "") == "/foo", "silent command: typed");
        qw.send({{"type", "typed"}, {"ok", true}});
        check(!waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "silent command: busy");
        screen(true);
        check(waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") != "busy"; }, 1500).empty(),
              "a screen that shows work keeps the command pending");
        screen(false);
        check(!waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") == "idle"; }, 3000).empty(),
              "a free prompt with no work ends a command that wrote nothing");
        // A shell command likewise; a wrapper that does not say `working` never ends it.
        a.send({{"type", "prompt"}, {"ref", 151}, {"session", "wquiet"}, {"text", "! true"}});
        check(qw.next("type").value("text", "") == "! true", "silent shell command: typed");
        qw.send({{"type", "typed"}, {"ok", true}});
        qw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        check(waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") != "busy"; }, 1500).empty(),
              "a screen message without `working` does not end it");
        screen(false);
        check(!waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") == "idle"; }, 3000).empty(), "a silent shell command ends too");
        // A limit without a reset time.
        std::ofstream(quietTr, std::ios::app)
            << json({{"type", "assistant"}, {"uuid", "lim1"}, {"isApiErrorMessage", true}, {"apiError", "usage_limit_reached"},
                     {"message", {{"content", {{{"type", "text"}, {"text", "Limit"}}}}}}}).dump() << "\n";
        check(!waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") == "busy" && s.value("limited", json()).is_object(); }, 3000).empty(),
              "a limit with no reset time holds the session busy");
        screen(true);
        check(waitSession(a, "wquiet", [](const json &s) { return !s.value("limited", json()).is_object(); }, 4000).empty(),
              "work on the screen keeps the limit");
        screen(false);
        check(!waitSession(a, "wquiet", [](const json &s) { return s.value("state", "") == "idle" && !s.value("limited", json()).is_object(); }, 6000).empty(),
              "a free prompt with no work gives the limit up");
    }

    // A failed typed check that pvai retries by itself on a free screen shows
    // the app no dialog and raises no attention; after three failures, or
    // with a real dialog on the screen, it does.
    {
        LineClient fw;
        fw.send({{"type", "register"}, {"version", kVersion}, {"id", "wflash"}, {"cwd", project}, {"label", "flash"}, {"restart", true}, {"verify", true}});
        hook("wflash", {{"hook_event_name", "SessionStart"}, {"session_id", "flash-1"}, {"source", "startup"}});
        check(!waitSession(a, "wflash", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "flash: a session");
        fw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        auto shownDialog = [&](const json &s) { return s.value("state", "") == "waiting" || !s.value("dialog", json()).is_null(); };
        // (c) free screen: no waiting/screen state, no attention, typed again
        a.send({{"type", "prompt"}, {"ref", 180}, {"session", "wflash"}, {"text", "eins"}});
        check(fw.next("type").value("text", "") == "eins", "flash (c): typed");
        fw.send({{"type", "typed"}, {"ok", false}, {"screen", "busy box"}});
        const Observed c = observe(a, "wflash", 1500);
        check(!c.shown && c.attention.empty(), "flash (c): no dialog and no attention during the retry");
        check(fw.next("type", {}, 5000).value("text", "") == "eins", "flash (c): typed again");
        fw.send({{"type", "typed"}, {"ok", true}});
        hook("wflash", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "flash-1"}, {"prompt", "eins"}});
        hook("wflash", {{"hook_event_name", "Stop"}, {"session_id", "flash-1"}, {"last_assistant_message", "ok"}});
        check(!waitSession(a, "wflash", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "flash (c): idle again");
        // (d) three failures: the dialog and the attention appear
        a.send({{"type", "prompt"}, {"ref", 181}, {"session", "wflash"}, {"text", "zwei"}});
        for (int i = 0; i < 3; ++i) {
            check(fw.next("type", {}, 6000).value("text", "") == "zwei", "flash (d): try " + std::to_string(i + 1));
            fw.send({{"type", "typed"}, {"ok", false}, {"screen", "busy box"}});
            if (i < 2) {
                fw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
                const Observed hidden = observe(a, "wflash", 400);
                check(!hidden.shown && hidden.attention.empty(), "flash (d): still hidden after failure " + std::to_string(i + 1));
            }
        }
        const Observed d = observe(a, "wflash", 1500);
        check(d.shown && d.has("stuck") && d.has("screen"), "flash (d): the dialog and the attention show after the last failure");
        a.send({{"type", "answer"}, {"ref", 182}, {"session", "wflash"}, {"choice", "send"}});
        check(fw.next("type", {}, 3000).value("text", "") == "zwei", "flash (d): the owner's send types it");
        fw.send({{"type", "typed"}, {"ok", true}});
        hook("wflash", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "flash-1"}, {"prompt", "zwei"}});
        hook("wflash", {{"hook_event_name", "Stop"}, {"session_id", "flash-1"}, {"last_assistant_message", "ok"}});
        check(!waitSession(a, "wflash", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "flash (d): idle again");
        // (e) a real dialog on the screen: shown at once
        a.send({{"type", "prompt"}, {"ref", 183}, {"session", "wflash"}, {"text", "drei"}});
        check(fw.next("type").value("text", "") == "drei", "flash (e): typed");
        const std::string q = "How is Claude doing this session? (optional)\n  1: Bad  2: Fine";
        fw.send({{"type", "screen"}, {"dialog", q}, {"prompt", false}});
        fw.send({{"type", "typed"}, {"ok", false}, {"screen", q}});
        const Observed e = observe(a, "wflash", 1500);
        check(e.shown && e.has("screen"), "flash (e): the dialog shows at once, with its attention");
        fw.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}});
        check(fw.next("type", {}, 4000).value("text", "") == "drei", "flash (e): typed once the dialog is gone");
        fw.send({{"type", "typed"}, {"ok", true}});
        hook("wflash", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "flash-1"}, {"prompt", "drei"}});
        hook("wflash", {{"hook_event_name", "Stop"}, {"session_id", "flash-1"}, {"last_assistant_message", "ok"}});
    }

    // The app interrupts a slash command (Claude Code puts it back into the
    // box): the wrapper is asked to empty the box if it holds that text, the
    // slash command is not typed again, and the next one goes in only after
    // the wrapper's draft report.
    {
        LineClient ew;
        ew.send({{"type", "register"}, {"version", kVersion}, {"id", "wslashesc"}, {"cwd", project}, {"label", "slashesc"}, {"restart", true}, {"verify", true},
                 {"clear", true}, {"submitCheck", true}, {"clearIf", true}});
        const std::string escTr = home + "/slashesc-tr/slashesc-1.jsonl";
        writeFile(escTr, userLine("start"));
        hook("wslashesc", {{"hook_event_name", "SessionStart"}, {"session_id", "slashesc-1"}, {"source", "startup"}, {"transcript_path", escTr}});
        check(!waitSession(a, "wslashesc", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "slash Esc: a session");
        ew.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"working", true}}); // a compaction shows work
        a.send({{"type", "prompt"}, {"ref", 170}, {"session", "wslashesc"}, {"text", "/compact"}});
        check(ew.next("type").value("text", "") == "/compact", "slash Esc: typed");
        ew.send({{"type", "typed"}, {"ok", true}});
        check(!waitSession(a, "wslashesc", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "slash Esc: busy");
        a.send({{"type", "interrupt"}, {"ref", 171}, {"session", "wslashesc"}});
        const json clear = ew.next("clear", {}, 8000);
        check(!clear.empty() && clear.value("if", "") == "/compact", "slash Esc: the box is emptied if it holds the command: " + clear.dump());
        a.send({{"type", "prompt"}, {"ref", 172}, {"session", "wslashesc"}, {"text", "/compact"}});
        check(ew.next("type", {}, 800).empty(), "slash Esc: nothing goes in before the wrapper's draft report");
        ew.send({{"type", "draft"}, {"dirty", false}});
        check(ew.next("type", {}, 3000).value("text", "") == "/compact", "slash Esc: the next command goes in alone after it");
        ew.send({{"type", "typed"}, {"ok", true}});
    }

    // A slash command typed after the screen has been free for longer than
    // the quiet time is not taken as done before the quiet time has passed
    // since it was typed.
    {
        LineClient qs;
        qs.send({{"type", "register"}, {"version", kVersion}, {"id", "wquietslash"}, {"cwd", project}, {"label", "quietslash"}, {"restart", true}, {"verify", true},
                 {"clear", true}, {"submitCheck", true}});
        const std::string qsTr = home + "/quietslash-tr/quietslash-1.jsonl";
        writeFile(qsTr, userLine("start"));
        hook("wquietslash", {{"hook_event_name", "SessionStart"}, {"session_id", "quietslash-1"}, {"source", "startup"}, {"transcript_path", qsTr}});
        check(!waitSession(a, "wquietslash", [](const json &s) { return s.value("state", "") == "idle"; }).empty(), "late slash: a session");
        qs.send({{"type", "screen"}, {"dialog", nullptr}, {"prompt", true}, {"working", false}});
        std::this_thread::sleep_for(std::chrono::milliseconds(900)); // well over the 0.25 s quiet time
        a.send({{"type", "prompt"}, {"ref", 173}, {"session", "wquietslash"}, {"text", "/foo"}});
        check(qs.next("type").value("text", "") == "/foo", "late slash: typed");
        qs.send({{"type", "typed"}, {"ok", true}});
        check(!waitSession(a, "wquietslash", [](const json &s) { return s.value("state", "") == "busy"; }).empty(), "late slash: busy");
        check(waitSession(a, "wquietslash", [](const json &s) { return s.value("state", "") != "busy"; }, 150).empty(),
              "late slash: not done before the quiet time since it was typed");
        check(!waitSession(a, "wquietslash", [](const json &s) { return s.value("state", "") == "idle"; }, 3000).empty(),
              "late slash: done afterwards");
    }

    // ---- Hooks: pvai run puts them in place, once.
    auto shell = [](const std::string &command) { return runCommand(command + " </dev/null", false).out; };
    const std::string settings = home + "/.claude/settings.json";
    writeFile(settings, R"({"model": "opus", "statusLine": {"type": "command", "command": "my-line"}})");
    const std::string claudeJson = home + "/.claude.json";
    writeFile(claudeJson, R"({"theme": "light", "projects": {"/x": {}}})");
    std::string out = shell(g_pvai + " run -- claude --version");
    const json onboarded = json::parse(readFile(claudeJson), nullptr, false);
    check(onboarded.is_object() && onboarded.value("hasCompletedOnboarding", false) && onboarded.value("theme", "") == "light"
              && onboarded.contains("projects"),
          "pvai run marks onboarding done and keeps the existing keys: " + onboarded.dump());
    fs::remove(claudeJson);
    shell(g_pvai + " run -- claude --version");
    const json created = json::parse(readFile(claudeJson), nullptr, false);
    check(created.is_object() && created.value("hasCompletedOnboarding", false) && created.value("theme", "") == "dark",
          "a missing .claude.json is created with onboarding done and theme dark: " + created.dump());
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

    daemon.stop();
    return finish();
}
