#include "codex.h"
#include "common.h"
#include "pairing.h"
#include "run.h"
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
    "       pvai listen [ADDR[:PORT] | off]            direct access without ssh (TLS, port 7471), or show it\n"
    "       pvai pair [--name N] [--host H] [--link]   let one device pair within 10 minutes (link and QR code)\n"
    "       pvai devices                               list paired devices\n"
    "       pvai revoke NAME|FINGERPRINT               remove a paired device and close its connections\n"
    "       pvai hook                                  called by Claude Code's hooks\n"
    "       pvai statusline [--chain COMMAND]          called as Claude Code's status line\n"
    "       pvai install-hooks                         add the hooks and the status line to ~/.claude/settings.json\n"
    "                                                  (pvai run does that itself when they are missing)\n"
    "       pvai token                                 print the token the app needs\n"
    "       pvai start [--name LABEL] [DIR] [-- ARGS...] start Claude Code without a terminal\n"
    "       pvai attach SESSION                        a terminal onto such a session (Ctrl+\\ leaves it)\n"
    "       pvai sessions                              list this host's sessions\n"
    "       pvai answer SESSION allow|deny             answer the dialog a session waits on\n"
    "       pvai stop SESSION                          end a session started with start\n"
    "       pvai send [--voice] SESSION TEXT           type TEXT into a session (for tests)\n"
    "       pvai propose HOST[/SESSION] [TEXT]         suggest a message to another session; the owner releases it\n"
    "       pvai watch                                 print what the daemon tells apps\n";

const char *kHookEvents[] = {"SessionStart", "UserPromptSubmit", "Stop", "Notification"};

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

// Puts the hooks and the status line into ~/.claude/settings.json when they
// are missing or name another pvai binary; a file that is already right is
// not touched. `quiet`: say only what changed or failed, on stderr (for
// `pvai run`, which checks on every start). 0 when they are in place.
int installHooks(bool quiet)
{
    const char *home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : "") + "/.claude";
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
    const json wanted = withHooks(settings);
    if (exists && wanted == settings) {
        if (!quiet)
            std::printf("hooks and status line for %s are in place in %s\n", selfExe().c_str(), path.c_str());
        return 0;
    }
    // Written beside it and renamed, so that Claude Code never reads half a
    // file; the old one stays as .pvai-bak.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (exists)
        std::ofstream(path + ".pvai-bak") << settings.dump(2) << "\n";
    const std::string temp = path + ".pvai-new";
    {
        std::ofstream out(temp);
        out << wanted.dump(2) << "\n";
        if (!out) {
            std::fprintf(stderr, "pvai: cannot write %s\n", temp.c_str());
            return 1;
        }
    }
    if (exists)
        ::chmod(temp.c_str(), st.st_mode & 07777);
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "pvai: cannot replace %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    std::fprintf(quiet ? stderr : stdout, "pvai: hooks and status line for %s written to %s\n", selfExe().c_str(), path.c_str());
    return 0;
}

int localClient(const json &first, bool follow, bool startDaemon = false)
{
    const int fd = openDaemon(startDaemon);
    if (fd < 0) {
        std::fprintf(stderr, "pvai: no daemon\n");
        return 1;
    }
    Conn conn(fd);
    conn.send({{"type", "hello"}, {"client", "pvai"}, {"version", kVersion}});
    if (!first.is_null())
        conn.send(first);
    for (;;) {
        pollfd p{conn.fd(), static_cast<short>(POLLIN | (conn.wantsWrite() ? POLLOUT : 0)), 0};
        if (::poll(&p, 1, follow ? -1 : 3000) <= 0)
            return follow ? 1 : 2;
        std::vector<json> messages;
        const bool alive = conn.readMessages(messages) && conn.flush();
        for (const auto &message : messages) {
            std::cout << message.dump() << std::endl;
            const std::string type = message.is_object() ? message.value("type", "") : "";
            if (!follow && (type == "accepted" || type == "answered" || type == "started" || type == "stopping"))
                return 0;
            if (!follow && (type == "rejected" || (type == "sessions" && first.is_null())))
                return type == "rejected" ? 1 : 0;
        }
        if (!alive)
            return 1;
    }
}

// A request to the daemon that is no app's: no hello; `each` sees every
// answer and returns true when it was the last.
int adminRequest(const json &request, bool startDaemon, const std::function<bool(const json &)> &each)
{
    const int fd = openDaemon(startDaemon);
    if (fd < 0)
        return -1;
    Conn conn(fd);
    conn.send(request);
    for (;;) {
        pollfd p{conn.fd(), static_cast<short>(POLLIN | (conn.wantsWrite() ? POLLOUT : 0)), 0};
        if (::poll(&p, 1, -1) < 0 && errno != EINTR)
            return 1;
        std::vector<json> messages;
        const bool alive = conn.readMessages(messages) && conn.flush();
        for (const auto &message : messages)
            if (message.is_object() && each(message))
                return 0;
        if (!alive)
            return 1;
    }
}

// An agent message for the owner to release in the app; never waits for it.
int runPropose(const std::string &to, std::string text, bool fromStdin)
{
    if (fromStdin)
        text.assign(std::istreambuf_iterator<char>(std::cin), {});
    const int fd = openDaemon(false);
    if (fd < 0) {
        std::fprintf(stderr, "pvai: no daemon\n");
        return 1;
    }
    const char *from = std::getenv(kEnvId);
    Conn conn(fd);
    conn.send({{"type", "hello"}, {"client", "pvai"}, {"version", kVersion}});
    conn.send({{"type", "propose"}, {"ref", "cli"}, {"to", to}, {"text", text}, {"from", from ? from : ""}});
    for (;;) {
        pollfd p{conn.fd(), static_cast<short>(POLLIN | (conn.wantsWrite() ? POLLOUT : 0)), 0};
        if (::poll(&p, 1, 3000) <= 0)
            return 1;
        std::vector<json> messages;
        const bool alive = conn.readMessages(messages) && conn.flush();
        for (const auto &message : messages) {
            const std::string type = message.is_object() ? message.value("type", "") : "";
            if (type == "proposed") {
                const int apps = message.value("apps", 0);
                if (apps > 0)
                    std::printf("waiting for the owner in the app (%d app%s connected)\n", apps, apps == 1 ? "" : "s");
                else
                    std::printf("waiting for the owner in the app (no app connected now; it waits up to 24 hours)\n");
                return 0;
            }
            if (type == "rejected") {
                std::fprintf(stderr, "pvai: not proposed: %s\n", message.value("reason", "").c_str());
                return 1;
            }
        }
        if (!alive)
            return 1;
    }
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
    const int status = adminRequest({{"type", "revoke"}, {"device", which}}, false, [&](const json &answer) {
        for (const auto &name : answer.value("devices", json::array()))
            names.push_back(name.get<std::string>());
        return true;
    });
    if (status < 0) // no daemon, so no connections either
        for (const auto &device : Devices(devicesPath()).remove(which))
            names.push_back(device.name);
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
        const pid_t pid = ::fork();
        if (pid == 0) {
            const int null = ::open("/dev/null", O_WRONLY);
            ::dup2(null, STDOUT_FILENO);
            ::execl(codex.c_str(), "codex", "app-server", "daemon", "start", static_cast<char *>(nullptr));
            ::_exit(127);
        }
        if (pid > 0)
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
    std::vector<char *> raw;
    for (auto &arg : argv)
        raw.push_back(arg.data());
    raw.push_back(nullptr);
    dropClaudeEnvironment();
    ::execv(codex.c_str(), raw.data());
    std::perror("pvai: codex");
    return 127;
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
            if (program == "codex" && !options.detached)
                return runCodex({options.command.begin() + 1, options.command.end()});
            // Claude Code reports through the hooks and the status line; a
            // missing or stale entry (another pvai binary) is fixed here.
            if (program == "claude")
                installHooks(true);
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
        if (verb == "stop" && args.size() == 2)
            return localClient({{"type", "stop"}, {"ref", "cli"}, {"session", args[1]}}, false);
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
        if (verb == "token") {
            std::printf("%s\n", token().c_str());
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
        if (verb == "propose" && (args.size() == 2 || args.size() == 3))
            return runPropose(args[1], args.size() == 3 ? args[2] : "", args.size() == 2);
        if (verb == "watch")
            return localClient(nullptr, true);
        if (verb == "--version") {
            std::printf("pvai %s\n", kVersion);
            return 0;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "pvai: %s\n", e.what());
        return verb == "hook" ? 0 : 1;
    }
    std::fputs(kUsage, stderr);
    return 2;
}
