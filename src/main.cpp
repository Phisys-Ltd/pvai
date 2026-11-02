#include "codex.h"
#include "common.h"
#include "pairing.h"
#include "run.h"
#include "service.h"
#include "tls.h"

#include <qrcodegen.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ctime>
#include <functional>
#include <optional>
#include <set>
#include <thread>

namespace pvai {
int runDaemon(int port);
int runHook();
int runStatusline(const std::string &chain);
} // namespace pvai

namespace {

using namespace pvai;

const char *kUsage =
    "usage: pvai run [--name LABEL] [-- COMMAND...]   Claude Code (default: claude) reachable by voice\n"
    "       pvai codex [ARGS...]                       the Codex TUI on Codex's shared app server, reachable by voice\n"
    "       pvai daemon [--port N]                     the host's sessions (started by run on demand)\n"
    "       pvai daemon stop|restart [--force]         stop or restart it; refused while prompts are queued\n"
    "       pvai listen [ADDR[:PORT] | off]            direct access without ssh (TLS, port 7471), or show it\n"
    "       pvai pair [--name N] [--host H] [--link]   let one device pair within 10 minutes (link and QR code)\n"
    "       pvai devices                               list paired devices\n"
    "       pvai revoke NAME|FINGERPRINT               remove a paired device and close its connections\n"
    "       pvai hook                                  called by Claude Code's hooks\n"
    "       pvai statusline [--chain COMMAND]          called as Claude Code's status line\n"
    "       pvai install-hooks                         add the hooks and the status line to ~/.claude/settings.json\n"
    "                                                  (pvai run does that itself when they are missing)\n"
    "       pvai uninstall-hooks                       remove them again (a chained status line comes back)\n"
    "       pvai install-service                       run the daemon as a systemd user service, also after a reboot\n"
    "       pvai uninstall-service                     remove that service again (the running daemon stays)\n"
    "       pvai token                                 print the token the app needs (and start the daemon)\n"
    "       pvai start [--name LABEL] [DIR] [-- ARGS...] start Claude Code without a terminal\n"
    "       pvai attach SESSION                        a terminal onto such a session (Ctrl+\\ leaves it)\n"
    "       pvai sessions                              list this host's sessions\n"
    "       pvai answer SESSION allow|deny             answer the dialog a session waits on\n"
    "       pvai forget DIR ID                         delete an ended Claude Code conversation in DIR for good\n"
    "       pvai clear-input SESSION                   empty a session's prompt box (a draft, a shell command)\n"
    "       pvai stop SESSION                          end a session started with start\n"
    "       pvai send [--voice] SESSION TEXT           type TEXT into a session (for tests)\n"
    "       pvai propose [--reply ID] HOST[/SESSION] [TEXT]\n"
    "                                                  suggest a message to another session; the owner releases it\n"
    "       pvai peers                                 list the sessions propose can reach, also on other hosts\n"
    "       pvai install claude|codex|kimi             install an assistant for this user (as the app can)\n"
    "       pvai login claude|codex|kimi               log it in: shows a link and a code\n"
    "       pvai watch                                 print what the daemon tells apps\n"
    "       pvai --version | version                   print the version\n";

const char *kHookEvents[] = {"SessionStart", "UserPromptSubmit", "Stop", "Notification", "PermissionRequest"};

std::string shellQuote(const std::string &text)
{
    std::string out = "'";
    for (const char c : text)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string unquote(const std::string &quoted)
{
    if (quoted.size() < 2 || quoted.front() != '\'')
        return quoted;
    std::string out;
    for (std::size_t i = 1; i + 1 < quoted.size(); ++i) {
        if (quoted.compare(i, 4, "'\\''") == 0) {
            out += '\'';
            i += 3;
        } else {
            out += quoted[i];
        }
    }
    return out;
}

bool isPvaiHook(const json &hook)
{
    const std::string command = hook.is_object() ? hook.value("command", "") : "";
    const std::string suffix = "pvai hook";
    return command.size() >= suffix.size()
        && command.compare(command.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// `settings` with `<this binary> hook` on every event pvai needs and its
// status line, older pvai entries replaced, everything else left as it is.
json withHooks(json settings)
{
    const std::string command = selfExe() + " hook";
    json &hooks = settings["hooks"];
    if (!hooks.is_object())
        hooks = json::object();
    for (const char *event : kHookEvents) {
        json &groups = hooks[event];
        if (!groups.is_array())
            groups = json::array();
        for (auto &group : groups)
            if (group.is_object() && group.contains("hooks") && group["hooks"].is_array())
                std::erase_if(group["hooks"].get_ref<json::array_t &>(), isPvaiHook);
        std::erase_if(groups.get_ref<json::array_t &>(), [](const json &group) {
            return group.is_object() && group.contains("hooks") && group["hooks"].empty();
        });
        groups.push_back({{"hooks", json::array({{{"type", "command"}, {"command", command}, {"timeout", 5}}})}});
    }
    // The status line carries context and rate limits; an own one keeps
    // printing through --chain.
    const std::string statusCommand = selfExe() + " statusline";
    json &statusLine = settings["statusLine"];
    std::string own = statusLine.is_object() ? statusLine.value("command", "") : "";
    if (own.find("pvai statusline") != std::string::npos) {
        const auto chain = own.find(" --chain ");
        own = chain == std::string::npos ? "" : unquote(own.substr(chain + 9));
    }
    statusLine = {{"type", "command"}, {"command", own.empty() ? statusCommand : statusCommand + " --chain " + shellQuote(own)}};
    return settings;
}

// `settings` without what withHooks() put in: pvai's hook entries (groups
// and events they leave empty go too) and its status line, a chained one
// restored. Everything else stays as it is.
json withoutHooks(json settings)
{
    if (settings.contains("hooks") && settings["hooks"].is_object()) {
        json &hooks = settings["hooks"];
        for (auto it = hooks.begin(); it != hooks.end();) {
            json &groups = it.value();
            bool removed = false;
            if (groups.is_array()) {
                for (auto &group : groups)
                    if (group.is_object() && group.contains("hooks") && group["hooks"].is_array())
                        removed |= std::erase_if(group["hooks"].get_ref<json::array_t &>(), isPvaiHook) > 0;
                std::erase_if(groups.get_ref<json::array_t &>(), [](const json &group) {
                    return group.is_object() && group.contains("hooks") && group["hooks"].empty();
                });
            }
            if (removed && groups.empty())
                it = hooks.erase(it);
            else
                ++it;
        }
        if (hooks.empty())
            settings.erase("hooks");
    }
    if (settings.contains("statusLine") && settings["statusLine"].is_object()) {
        const std::string own = settings["statusLine"].value("command", "");
        if (own.find("pvai statusline") != std::string::npos) {
            const auto chain = own.find(" --chain ");
            if (chain == std::string::npos)
                settings.erase("statusLine");
            else
                settings["statusLine"] = {{"type", "command"}, {"command", unquote(own.substr(chain + 9))}};
        }
    }
    return settings;
}

// Rewrites ~/.claude/settings.json with `change` applied; a file the change
// leaves as it is is not touched. `quiet`: say only what changed or failed,
// on stderr (for `pvai run`, which checks on every start). 0 when the file
// is as wanted.
int changeSettings(const std::function<json(json)> &change, bool create, bool quiet,
                   const char *unchanged, const char *written)
{
    const std::string dir = homeDir() + "/.claude";
    const std::string path = dir + "/settings.json";
    json settings = json::object();
    struct stat st;
    const bool exists = ::stat(path.c_str(), &st) == 0;
    if (exists) {
        std::ifstream in(path);
        settings = json::parse(in, nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) {
            std::fprintf(stderr, "pvai: %s is not a JSON object, not touching it\n", path.c_str());
            return 1;
        }
    }
    const json wanted = change(settings);
    if ((exists || !create) && wanted == settings) {
        if (!quiet)
            std::printf("%s %s\n", unchanged, path.c_str());
        return 0;
    }
    // Written beside it and renamed, so that Claude Code never reads half a
    // file; the old one stays as .pvai-bak.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (exists)
        std::ofstream(path + ".pvai-bak") << settings.dump(2) << "\n";
    AtomicOptions options;
    options.mode = 0666; // as an ofstream creates it
    options.suffix = ".pvai-new";
    options.sync = false;
    if (exists)
        options.chmod = st.st_mode & 07777;
    switch (writeAtomically(path, wanted.dump(2) + "\n", options)) {
    case Written::WriteFailed:
        std::fprintf(stderr, "pvai: cannot write %s\n", (path + options.suffix).c_str());
        return 1;
    case Written::RenameFailed:
        std::fprintf(stderr, "pvai: cannot replace %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    case Written::Ok:
        break;
    }
    std::fprintf(quiet ? stderr : stdout, "pvai: %s %s (the old file is %s.pvai-bak)\n", written, path.c_str(), path.c_str());
    return 0;
}

// Puts the hooks and the status line into ~/.claude/settings.json when they
// are missing or name another pvai binary.
int installHooks(bool quiet)
{
    const std::string unchanged = "hooks and status line for " + selfExe() + " are in place in";
    const std::string written = "hooks and status line for " + selfExe() + " written to";
    return changeSettings(withHooks, true, quiet, unchanged.c_str(), written.c_str());
}

// Claude Code's own config (~/.claude.json, beside the ~/.claude directory
// that installHooks uses): marks the first-run onboarding as done when the
// keys are missing, so a fresh install starts without its pages. Keys that
// exist stay as they are.
void markOnboardingDone()
{
    const std::string path = homeDir() + "/.claude.json";
    json config = json::object();
    struct stat st;
    const bool exists = ::stat(path.c_str(), &st) == 0;
    if (exists) {
        std::ifstream in(path);
        config = json::parse(in, nullptr, false);
        if (config.is_discarded() || !config.is_object()) {
            std::fprintf(stderr, "pvai: %s is not a JSON object, not touching it\n", path.c_str());
            return;
        }
    }
    bool changed = false;
    for (const auto &[key, value] : {std::pair<const char *, json>{"hasCompletedOnboarding", true}, {"theme", "dark"}})
        if (!config.contains(key)) {
            config[key] = value;
            changed = true;
        }
    if (!changed)
        return;
    AtomicOptions options;
    options.suffix = ".tmp";
    options.sync = false;
    if (exists)
        options.chmod = st.st_mode & 07777;
    if (writeAtomically(path, config.dump(2) + "\n", options) != Written::Ok)
        std::fprintf(stderr, "pvai: cannot write %s: %s\n", path.c_str(), std::strerror(errno));
}

int uninstallHooks()
{
    return changeSettings(withoutHooks, false, false, "no pvai hooks or status line in",
                          "pvai's hooks and status line removed from");
}

int noDaemon()
{
    std::fprintf(stderr, "pvai: no daemon\n");
    return 1;
}

enum class Talk { Done, NoDaemon, Timeout, Closed };

// Connects to the daemon (starting it when asked), says hello (a client of
// the app's kind), sends `first` when it is a message, and hands every
// answer to `handler` until that returns true. Timeout: nothing came within
// `timeoutMs` (-1: no limit); Closed: the daemon went away.
// `retryInterrupted`: a signal during the wait is no timeout.
Talk talkToDaemon(bool startDaemon, bool hello, const json &first, int timeoutMs,
                  const std::function<bool(Conn &, const json &)> &handler, bool retryInterrupted = false)
{
    const int fd = openDaemon(startDaemon);
    if (fd < 0)
        return Talk::NoDaemon;
    Conn conn(fd);
    if (hello)
        conn.send({{"type", "hello"}, {"client", "pvai"}, {"version", kVersion}});
    if (!first.is_null())
        conn.send(first);
    for (;;) {
        pollfd p{conn.fd(), static_cast<short>(POLLIN | (conn.wantsWrite() ? POLLOUT : 0)), 0};
        const int ready = ::poll(&p, 1, timeoutMs);
        if (ready < 0 ? !(retryInterrupted && errno == EINTR) : ready == 0)
            return Talk::Timeout;
        std::vector<json> messages;
        const bool alive = conn.readMessages(messages) && conn.flush();
        for (const auto &message : messages)
            if (handler(conn, message))
                return Talk::Done;
        if (!alive)
            return Talk::Closed;
    }
}

int localClient(const json &first, bool follow, bool startDaemon = false)
{
    int status = 1;
    const Talk talk = talkToDaemon(startDaemon, true, first, follow ? -1 : 3000, [&](Conn &, const json &message) {
        std::cout << message.dump() << std::endl;
        const std::string type = message.is_object() ? message.value("type", "") : "";
        if (!follow && (type == "accepted" || type == "answered" || type == "inputCleared" || type == "forgotten" || type == "started" || type == "stopping")) {
            status = 0;
            return true;
        }
        if (!follow && (type == "rejected" || (type == "sessions" && first.is_null()))) {
            status = type == "rejected" ? 1 : 0;
            return true;
        }
        return false;
    });
    if (talk == Talk::NoDaemon)
        return noDaemon();
    if (talk == Talk::Timeout)
        return follow ? 1 : 2;
    return talk == Talk::Done ? status : 1;
}

// A request to the daemon that is no app's: no hello; `each` sees every
// answer and returns true when it was the last.
int adminRequest(const json &request, bool startDaemon, const std::function<bool(const json &)> &each)
{
    const Talk talk = talkToDaemon(startDaemon, false, request, -1,
                                   [&](Conn &, const json &message) { return message.is_object() && each(message); }, true);
    return talk == Talk::NoDaemon ? -1 : talk == Talk::Done ? 0 : 1;
}

// An agent message for the owner to release in the app; never waits for it.
int runPropose(const std::string &to, std::string text, bool fromStdin, const std::string &replyTo)
{
    if (fromStdin)
        text.assign(std::istreambuf_iterator<char>(std::cin), {});
    const char *from = std::getenv(kEnvId);
    json propose = {{"type", "propose"}, {"ref", "cli"}, {"to", to}, {"text", text}, {"from", from ? from : ""}};
    if (!replyTo.empty())
        propose["replyTo"] = replyTo;
    int status = 1;
    const Talk talk = talkToDaemon(false, true, propose, 3000, [&](Conn &, const json &message) {
        const std::string type = message.is_object() ? message.value("type", "") : "";
        if (type == "proposed") {
            const int apps = message.value("apps", 0);
            std::printf("%s to %s: ", message.value("id", "").c_str(), message.value("to", to).c_str());
            if (apps > 0)
                std::printf("waiting for the owner in the app (%d app%s connected)\n", apps, apps == 1 ? "" : "s");
            else
                std::printf("waiting for the owner in the app (no app connected now; it waits up to 24 hours)\n");
            status = 0;
            return true;
        }
        if (type == "rejected") {
            const std::string reason = message.value("reason", "");
            std::fprintf(stderr, "pvai: not proposed: %s%s\n", reason.c_str(),
                         reason == "ambiguous-target" ? " (several sessions have that label: use the id, see pvai peers)" : "");
            return true;
        }
        return false;
    });
    if (talk == Talk::NoDaemon)
        return noDaemon();
    return talk == Talk::Done ? status : 1;
}

// The sessions an agent can write to: this host's, then the other hosts'
// as the app last told the daemon. Each line starts with the target for
// `pvai propose`.
// pvai daemon stop|restart [--force]: refused while prompts are queued (they
// live in the daemon's memory), unless forced; then the dropped ones are
// listed. restart starts a new daemon once the old one has let go.
int runDaemonControl(bool restart, bool force)
{
    bool refused = false;
    const Talk talk = talkToDaemon(false, true, {{"type", "shutdown"}, {"ref", "cli"}, {"force", force}}, 5000,
                                   [&](Conn &, const json &message) {
        const std::string type = message.is_object() ? message.value("type", "") : "";
        if (type == "rejected" && message.value("reason", "") == "queued") {
            std::fprintf(stderr, "pvai: not stopped: prompts are waiting\n");
            for (const auto &w : message.value("waiting", json::array()))
                std::fprintf(stderr, "  %s \"%s\": %d queued\n", w.value("session", "").c_str(), w.value("label", "").c_str(),
                             w.value("queued", 0));
            std::fprintf(stderr, "pvai daemon %s --force drops them\n", restart ? "restart" : "stop");
            refused = true;
            return true;
        }
        if (type == "shutting-down") {
            for (const auto &d : message.value("dropped", json::array()))
                std::printf("dropped: %s ref %s: %s\n", d.value("session", "").c_str(), d.value("ref", json()).dump().c_str(),
                            d.value("text", "").c_str());
            return true;
        }
        return false;
    });
    if (talk == Talk::NoDaemon) {
        if (!restart) {
            std::printf("no daemon running\n");
            return 0;
        }
    } else {
        if (talk == Talk::Timeout) {
            std::fprintf(stderr, "pvai: the daemon did not answer\n");
            return 1;
        }
        if (talk == Talk::Closed) {
            std::fprintf(stderr, "pvai: the daemon closed the connection\n");
            return 1;
        }
        if (refused)
            return 1;
        // Gone once its socket stops answering.
        for (int i = 0; i < 50; ++i) {
            const int probe = connectUnix(daemonSocket());
            if (probe < 0)
                break;
            ::close(probe);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("daemon stopped\n");
    }
    if (!restart)
        return 0;
    const int fresh = openDaemon(true);
    if (fresh < 0) {
        std::fprintf(stderr, "pvai: no new daemon\n");
        return 1;
    }
    ::close(fresh);
    std::printf("daemon started\n");
    return 0;
}

// pvai install NAME, pvai login NAME: what the app does, from a terminal.
// Follows the task's events; a login that wants a code back reads it.
int runSetup(const std::string &verb, const std::string &assistant)
{
    std::string shown;
    int status = 1;
    const Talk talk = talkToDaemon(true, true, {{"type", verb}, {"ref", "cli"}, {"assistant", assistant}}, -1,
                                   [&](Conn &conn, const json &m) {
        const std::string type = m.is_object() ? m.value("type", "") : "";
        if (type == "rejected" && m.value("ref", json()) == "cli") {
            std::fprintf(stderr, "pvai: %s %s refused: %s\n", verb.c_str(), assistant.c_str(), m.value("reason", "").c_str());
            return true;
        }
        if (type != verb || m.value("assistant", "") != assistant)
            return false;
        auto field = [&](const char *key) { return m.contains(key) && m[key].is_string() ? m[key].get<std::string>() : std::string(); };
        const std::string state = field("state");
        std::string line = state;
        for (const char *key : {"step", "url", "code", "text", "error"})
            if (const std::string v = field(key); !v.empty())
                line += std::string("  ") + key + ": " + v;
        if (line != shown) {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
            shown = line;
        }
        if (state == "code-needed") {
            std::printf("code from the page: ");
            std::fflush(stdout);
            std::string code;
            if (!std::getline(std::cin, code))
                return true;
            conn.send({{"type", "login-code"}, {"ref", "cli"}, {"assistant", assistant}, {"code", code}});
        }
        if (state == "done") {
            status = 0;
            return true;
        }
        return state == "failed";
    });
    if (talk == Talk::NoDaemon)
        return noDaemon();
    return talk == Talk::Done ? status : 1;
}

int runPeers()
{
    const char *me = std::getenv(kEnvId);
    auto line = [&](const std::string &host, const json &s) {
        const std::string id = s.value("session", "");
        std::string text = "  " + host + "/" + id;
        for (const char *key : {"assistant", "state", "label", "cwd"})
            if (const std::string value = s.value(key, ""); !value.empty())
                text += std::string("  ") + (std::string(key) == "label" ? "\"" + value + "\"" : value);
        if (me && id == me)
            text += "  (this session)";
        std::printf("%s\n", text.c_str());
    };
    const Talk talk = talkToDaemon(false, true, {{"type", "listPeers"}}, 3000, [&](Conn &, const json &message) {
        if (!message.is_object() || message.value("type", "") != "peerList")
            return false;
        const std::string self = message.value("self", "");
        std::printf("this host (%s):\n", self.c_str());
        if (message["local"].empty())
            std::printf("  no sessions\n");
        for (const auto &s : message["local"])
            line(self, s);
        if (message["at"].is_null()) {
            std::printf("other hosts: not known here (no app has sent them since the daemon started)\n");
            return true;
        }
        const long long age = static_cast<long long>(std::time(nullptr)) - message["at"].get<long long>();
        std::printf("other hosts (from the app, %lld min ago):\n", age / 60);
        if (message["peers"].empty())
            std::printf("  none\n");
        for (const auto &s : message["peers"])
            line(s.value("host", ""), s);
        std::printf("write to one with: pvai propose HOST/SESSION \"TEXT\" (a label works instead of the id when only one session has it)\n");
        return true;
    });
    if (talk == Talk::NoDaemon)
        return noDaemon();
    return talk == Talk::Done ? 0 : 1;
}

int runListen(const std::vector<std::string> &args)
{
    const std::string path = configDir() + "/listen";
    if (args.empty()) {
        std::ifstream in(path);
        std::string spec;
        std::getline(in, spec);
        if (spec.empty()) {
            std::printf("direct access is off\n");
            return 0;
        }
        std::printf("direct access on %s\nfingerprint %s\n", spec.c_str(), serverIdentity().fingerprint.c_str());
        return 0;
    }
    if (args[0] == "off") {
        std::filesystem::remove(path);
    } else {
        std::string spec = args[0];
        // A bare address gets the default port; an IPv6 one needs brackets.
        if (spec.find(':') == std::string::npos)
            spec += ":7471";
        const Identity identity = serverIdentity();
        if (identity.fingerprint.empty()) {
            std::fprintf(stderr, "pvai: cannot make the server key in %s\n", configDir().c_str());
            return 1;
        }
        std::ofstream out(path, std::ios::trunc);
        out << spec << "\n";
        if (!out) {
            std::fprintf(stderr, "pvai: cannot write %s\n", path.c_str());
            return 1;
        }
    }
    bool ok = false;
    const int status = adminRequest({{"type", "listen"}}, args[0] != "off", [&](const json &answer) {
        ok = answer.value("ok", false);
        const std::string spec = answer.value("spec", "");
        if (spec.empty())
            std::printf("direct access is off\n");
        else if (ok)
            std::printf("direct access on %s\nfingerprint %s\n", spec.c_str(), answer.value("fingerprint", "").c_str());
        else
            std::fprintf(stderr, "pvai: the daemon cannot listen on %s (see %s/daemon.log)\n", spec.c_str(),
                         runtimeDir().c_str());
        return true;
    });
    if (status < 0) {
        std::printf("saved; the daemon picks it up when it starts\n");
        return 0;
    }
    return status == 0 && ok ? 0 : 1;
}

// A QR code for a terminal: two modules per character, dark on light
// whatever the terminal's colours.
void printQr(const std::string &text)
{
    std::optional<qrcodegen::QrCode> code;
    try {
        code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
    } catch (const std::exception &) {
        return; // too long for a QR code; the link is printed anyway
    }
    const int width = code->getSize(), quiet = 2, size = width + 2 * quiet;
    auto dark = [&](int x, int y) {
        x -= quiet;
        y -= quiet;
        return x >= 0 && y >= 0 && x < width && y < width && code->getModule(x, y);
    };
    for (int y = 0; y < size; y += 2) {
        std::string line = "\x1b[30;47m";
        for (int x = 0; x < size; ++x) {
            const bool top = dark(x, y), bottom = y + 1 < size && dark(x, y + 1);
            line += top && bottom ? "\u2588" : top ? "\u2580" : bottom ? "\u2584" : " ";
        }
        std::printf("%s\x1b[0m\n", line.c_str());
    }
}

int runPair(const std::vector<std::string> &args)
{
    json request = {{"type", "pair-open"}, {"wait", true}};
    bool linkOnly = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--link")
            linkOnly = true;
        else if (args[i] == "--name" && i + 1 < args.size())
            request["name"] = args[++i];
        else if (args[i] == "--host" && i + 1 < args.size())
            request["host"] = args[++i];
        else if (args[i] == "--seconds" && i + 1 < args.size())
            request["ttl"] = std::atoi(args[++i].c_str());
        else {
            std::fputs(kUsage, stderr);
            return 1;
        }
    }
    request["wait"] = !linkOnly;
    int result = 1;
    const int status = adminRequest(request, false, [&](const json &answer) {
        const std::string type = answer.value("type", "");
        if (type == "pair-window") {
            const std::string link = answer.value("link", "");
            if (linkOnly) {
                std::printf("%s\n", link.c_str());
                result = 0;
                return true;
            }
            std::printf("Open this on the device, or scan the code, within %d minutes:\n\n%s\n\n",
                        (answer.value("seconds", 600) + 59) / 60, link.c_str());
            printQr(link);
            std::printf("\nwaiting for the device...\n");
            std::fflush(stdout);
            return false;
        }
        if (type == "pair-done") {
            std::printf("paired: %s\n", answer.value("device", "").c_str());
            result = 0;
        } else if (type == "pair-closed") {
            std::printf("the window closed (%s)\n", answer.value("reason", "").c_str());
        } else {
            std::fprintf(stderr, "pvai: %s\n", answer.value("text", answer.dump()).c_str());
        }
        return true;
    });
    if (status < 0) {
        std::fprintf(stderr, "pvai: no daemon; pvai listen first\n");
        return 1;
    }
    return status == 0 ? result : 1;
}

std::string when(long long time)
{
    if (time <= 0)
        return "never";
    const std::time_t t = static_cast<std::time_t>(time);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &tm);
    return text;
}

int runDevices()
{
    const auto devices = Devices(devicesPath()).load();
    if (devices.empty()) {
        std::printf("no paired devices\n");
        return 0;
    }
    for (const auto &d : devices)
        std::printf("%-24s  added %s  last seen %s\n  %s\n", d.name.c_str(), when(d.added).c_str(),
                    when(d.lastSeen).c_str(), d.fingerprint.c_str());
    return 0;
}

int runRevoke(const std::string &which)
{
    std::vector<std::string> names;
    bool failed = false;
    const int status = adminRequest({{"type", "revoke"}, {"device", which}}, false, [&](const json &answer) {
        if (answer.value("type", "") == "error") {
            failed = true;
            return true;
        }
        for (const auto &name : answer.value("devices", json::array()))
            names.push_back(name.get<std::string>());
        return true;
    });
    if (status < 0) { // no daemon, so no connections either
        bool saved = true;
        for (const auto &device : Devices(devicesPath()).remove(which, &saved))
            names.push_back(device.name);
        failed = !saved;
    }
    if (failed) {
        std::fprintf(stderr, "pvai: could not write %s, nothing revoked\n", devicesPath().c_str());
        return 1;
    }
    if (names.empty()) {
        std::fprintf(stderr, "pvai: no device %s\n", which.c_str());
        return 1;
    }
    for (const auto &name : names)
        std::printf("revoked: %s\n", name.c_str());
    return 0;
}

// The Codex TUI, on Codex's shared app server whatever their versions, so
// the daemon sees its thread. No wrapper: the app server is the interface.
int runCodex(const std::vector<std::string> &args)
{
    const std::string codex = codexBinary();
    if (codex.empty()) {
        std::fprintf(stderr, "pvai: codex is not installed\n");
        return 1;
    }
    if (!std::filesystem::exists(codexSocket())) {
        ChildOptions start;
        start.argv = {"codex", "app-server", "daemon", "start"};
        start.exe = codex;
        start.dropEnv = false;
        start.out = Stdio::null();
        if (const pid_t pid = spawnChild(start); pid > 0)
            ::waitpid(pid, nullptr, 0);
    }
    if (const int fd = openDaemon(true); fd >= 0)
        ::close(fd);

    // Subcommands that are no TUI get nothing added; resume, fork and agents
    // take the options after their name.
    static const std::set<std::string> other = {
        "exec", "e", "review", "login", "logout", "mcp", "plugin", "app-server", "remote-control", "completion",
        "update", "doctor", "sandbox", "debug", "apply", "a", "queue", "archive", "delete", "migrate-rollouts",
        "unarchive", "cloud", "exec-server", "features", "help"};
    std::vector<std::string> argv = {"codex"};
    std::size_t at = 0;
    const bool tui = args.empty() || !other.count(args[0]);
    if (tui && !args.empty() && (args[0] == "resume" || args[0] == "fork" || args[0] == "agents"))
        argv.push_back(args[at++]);
    if (tui) {
        bool remote = false, cd = false;
        for (const auto &arg : args) {
            remote = remote || arg == "--remote" || arg.rfind("--remote=", 0) == 0;
            cd = cd || arg == "-C" || arg == "--cd" || arg.rfind("--cd=", 0) == 0;
        }
        if (!remote)
            argv.insert(argv.end(), {"--remote", "unix://"});
        if (!cd) {
            char cwd[PATH_MAX];
            if (::getcwd(cwd, sizeof cwd))
                argv.insert(argv.end(), {"-C", cwd});
        }
    }
    argv.insert(argv.end(), args.begin() + static_cast<long>(at), args.end());
    dropClaudeEnvironment();
    if (!tui) {
        execChild(codex, argv, false);
        std::perror("pvai: codex");
        return 127;
    }

    // A TUI: it runs as our child while we stay registered with the daemon,
    // so that its threads leave the session list when it ends.
    std::string cwd;
    for (std::size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "-C" || argv[i] == "--cd")
            cwd = argv[i + 1];
    for (const auto &arg : argv)
        if (arg.rfind("--cd=", 0) == 0)
            cwd = arg.substr(5);
    char resolved[PATH_MAX];
    if (::realpath(cwd.c_str(), resolved))
        cwd = resolved;
    std::string resume; // codex resume <id or name>
    if (!args.empty() && args[0] == "resume")
        for (std::size_t i = 1; i < args.size() && resume.empty(); ++i) {
            if (args[i].rfind("-", 0) != 0)
                resume = args[i];
            else if ((args[i] == "-C" || args[i] == "--cd" || args[i] == "-m" || args[i] == "--model" || args[i] == "-c"
                      || args[i] == "--config" || args[i] == "-p" || args[i] == "--profile")
                     && i + 1 < args.size())
                ++i;
        }
    ChildOptions tuiChild;
    tuiChild.argv = argv;
    tuiChild.exe = codex;
    tuiChild.dropEnv = false; // dropped above
    tuiChild.execFailure = "pvai: codex";
    const pid_t child = spawnChild(tuiChild);
    if (child < 0) {
        std::perror("pvai: fork");
        return 1;
    }
    // The terminal's keys and signals are the TUI's.
    ::signal(SIGINT, SIG_IGN);
    ::signal(SIGQUIT, SIG_IGN);
    ::signal(SIGTSTP, SIG_IGN);
    static pid_t s_child;
    s_child = child;
    for (int sig : {SIGTERM, SIGHUP})
        ::signal(sig, [](int signal) { ::kill(s_child, signal); });
    // The connection is the registration: it ends when we do.
    const int daemon = openDaemon(true);
    if (daemon >= 0) {
        setCloseOnExec(daemon);
        sendLine(daemon, {{"type", "attach"}, {"assistant", "codex"}, {"cwd", cwd}, {"resume", resume},
                          {"pid", static_cast<int>(child)}});
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (daemon >= 0)
        ::close(daemon);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

} // namespace

int main(int argc, char **argv)
{
    ::signal(SIGPIPE, SIG_IGN);
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string verb = args.empty() ? "" : args[0];
    try {
        if (verb == "run") {
            WrapperOptions options;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--name" && i + 1 < args.size()) {
                    options.label = args[++i];
                } else if (args[i] == "--id" && i + 1 < args.size()) {
                    options.id = args[++i];
                } else if (args[i] == "--detached") {
                    options.detached = true;
                } else if (args[i] == "--") {
                    options.command.assign(args.begin() + static_cast<long>(i) + 1, args.end());
                    break;
                } else {
                    options.command.assign(args.begin() + static_cast<long>(i), args.end());
                    break;
                }
            }
            if (options.command.empty())
                options.command = {"claude"};
            const auto program = std::filesystem::path(options.command[0]).filename();
            // A bare "claude" is looked up the way the daemon looks it up, so
            // a program the app installed under ~/.local/bin runs from a
            // shell or a daemon whose PATH does not hold that directory.
            if (program == "claude" && options.command[0] == "claude")
                if (const std::string found = claudeBinary(); !found.empty())
                    options.command[0] = found;
            if (program == "codex" && !options.detached)
                return runCodex({options.command.begin() + 1, options.command.end()});
            // Claude Code reports through the hooks and the status line; a
            // missing or stale entry (another pvai binary) is fixed here.
            if (program == "claude") {
                installHooks(true);
                markOnboardingDone();
            }
            return runWrapper(options);
        }
        if (verb == "listen" && args.size() <= 2)
            return runListen({args.begin() + 1, args.end()});
        if (verb == "pair")
            return runPair({args.begin() + 1, args.end()});
        if (verb == "devices")
            return runDevices();
        if (verb == "revoke" && args.size() == 2)
            return runRevoke(args[1]);
        if (verb == "codex")
            return runCodex({args.begin() + 1, args.end()});
        if (verb == "start") {
            json message = {{"type", "start"}, {"ref", "cli"}, {"args", json::array()}};
            std::string dir;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--name" && i + 1 < args.size()) {
                    message["label"] = args[++i];
                } else if (args[i] == "--") {
                    for (++i; i < args.size(); ++i)
                        message["args"].push_back(args[i]);
                } else {
                    dir = args[i];
                }
            }
            char resolved[PATH_MAX];
            message["cwd"] = ::realpath(dir.empty() ? "." : dir.c_str(), resolved) ? resolved : dir;
            return localClient(message, false, true);
        }
        if (verb == "attach") {
            if (args.size() == 2)
                return runAttach(args[1]);
            std::fputs("pvai attach SESSION (see pvai sessions)\n", stderr);
            return 2;
        }
        if (verb == "sessions")
            return localClient(nullptr, false);
        if (verb == "answer" && args.size() == 3)
            return localClient({{"type", "answer"}, {"ref", "cli"}, {"session", args[1]}, {"choice", args[2]}}, false);
        if (verb == "forget" && args.size() == 3)
            return localClient({{"type", "forget"}, {"ref", "cli"}, {"assistant", "claude"}, {"cwd", args[1]}, {"id", args[2]}}, false);
        if (verb == "clear-input" && args.size() == 2)
            return localClient({{"type", "clearInput"}, {"ref", "cli"}, {"session", args[1]}}, false);
        if (verb == "stop" && args.size() == 2)
            return localClient({{"type", "stop"}, {"ref", "cli"}, {"session", args[1]}}, false);
        if (verb == "daemon" && args.size() >= 2 && (args[1] == "stop" || args[1] == "restart")
            && (args.size() == 2 || (args.size() == 3 && args[2] == "--force")))
            return runDaemonControl(args[1] == "restart", args.size() == 3);
        if (verb == "daemon") {
            int port = kDefaultPort;
            if (args.size() == 3 && args[1] == "--port")
                port = std::atoi(args[2].c_str());
            else if (const char *env = std::getenv("PVAI_PORT"))
                port = std::atoi(env);
            return runDaemon(port);
        }
        if (verb == "hook")
            return runHook();
        if (verb == "statusline")
            return runStatusline(args.size() == 3 && args[1] == "--chain" ? args[2] : "");
        if (verb == "install-hooks")
            return installHooks(false);
        if (verb == "uninstall-hooks")
            return uninstallHooks();
        if (verb == "install-service" && args.size() == 1)
            return installService();
        if (verb == "uninstall-service" && args.size() == 1)
            return uninstallService();
        if (verb == "token") {
            std::printf("%s\n", token().c_str());
            std::fflush(stdout);
            // The app asks for the token before it connects; on a host
            // where nothing has started the daemon yet, this does.
            if (const int fd = openDaemon(true); fd >= 0)
                ::close(fd);
            return 0;
        }
        if (verb == "send" && args.size() >= 3) {
            const bool voice = args[1] == "--voice";
            const std::size_t at = voice ? 2 : 1;
            if (args.size() != at + 2) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            return localClient({{"type", "prompt"}, {"ref", "cli"}, {"session", args[at]},
                                {"text", args[at + 1]}, {"voice", voice}}, false);
        }
        if (verb == "propose") {
            std::size_t at = 1;
            std::string replyTo;
            if (args.size() > 2 && args[1] == "--reply") {
                replyTo = args[2];
                at = 3;
            }
            if (args.size() == at + 1 || args.size() == at + 2)
                return runPropose(args[at], args.size() == at + 2 ? args[at + 1] : "", args.size() == at + 1, replyTo);
        }
        if ((verb == "install" || verb == "login") && args.size() == 2)
            return runSetup(verb, args[1]);
        if (verb == "peers" && args.size() == 1)
            return runPeers();
        if (verb == "watch")
            return localClient(nullptr, true);
        if (verb == "--version" || verb == "version") {
            std::printf("%s\n", kVersion);
            return 0;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "pvai: %s\n", e.what());
        return verb == "hook" ? 0 : 1;
    }
    std::fputs(kUsage, stderr);
    return 2;
}
