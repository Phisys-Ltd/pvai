#include "setup.h"

#include "session.h" // trim, cut, oneLine

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace pvai {

namespace {

constexpr auto kInstallTime = std::chrono::minutes(15);
constexpr auto kLoginTime = std::chrono::minutes(30);
constexpr auto kLogoutTime = std::chrono::minutes(1);
constexpr std::size_t kMaxOutput = 64 * 1024;
constexpr const char *kStep = "pvai-step: ";
// `claude setup-token` is run in a terminal this wide, so that it does not
// wrap its link or its token; wraps are put together anyway.
constexpr int kTokenCols = 1000;
constexpr std::size_t kWrapMin = 40; // a line this long may go on in the next one
constexpr std::size_t kTokenMin = 40;

// For claude: the official installer. For codex and kimi: npm into a
// prefix of their own under ~/.local/share/pvai, with a Node of the
// system's when new enough, else Node 22 from nodejs.org (checked against
// its SHASUMS256.txt) beside it; a wrapper in ~/.local/bin starts it with
// that Node. Nothing outside the home directory is touched.
const char *const kInstall = R"SH(set -eu
step() { echo "pvai-step: $1"; }
a="$1"
mkdir -p "$HOME/.local/bin"
case "$a" in
claude)
  step download
  curl -fsSL https://claude.ai/install.sh | bash
  ;;
codex|kimi)
  if [ "$a" = kimi ]; then need=22.19.0; pkg=@moonshot-ai/kimi-code; else need=20.0.0; pkg=@openai/codex; fi
  step node
  newEnough() {
    v=$("$1" -v 2>/dev/null | sed 's/^v//') || return 1
    [ -n "$v" ] || return 1
    printf '%s\n%s\n' "$need" "$v" | sort -t. -k1,1n -k2,2n -k3,3n -C
  }
  dir="$HOME/.local/share/pvai"
  node=""
  for c in "$dir/node/bin/node" "$(command -v node || true)"; do
    if [ -n "$c" ] && [ -x "$c" ] && newEnough "$c"; then node="$c"; break; fi
  done
  if [ -z "$node" ]; then
    case "$(uname -s)" in Linux) os=linux;; Darwin) os=darwin;; *) echo "no Node for $(uname -s)"; exit 1;; esac
    case "$(uname -m)" in x86_64|amd64) m=x64;; aarch64|arm64) m=arm64;; *) echo "no Node for $(uname -m)"; exit 1;; esac
    base=https://nodejs.org/dist/latest-v22.x
    mkdir -p "$dir"
    tmp=$(mktemp -d)
    curl -fsSL "$base/SHASUMS256.txt" -o "$tmp/SHASUMS256.txt"
    file=$(awk -v want="-$os-$m.tar.gz" '{ if (substr($2, length($2) - length(want) + 1) == want) { print $2; exit } }' "$tmp/SHASUMS256.txt")
    [ -n "$file" ] || { echo "no Node 22 for $os-$m"; exit 1; }
    echo "downloading $file"
    curl -fsSL "$base/$file" -o "$tmp/$file"
    if command -v sha256sum >/dev/null 2>&1; then sum=$(sha256sum "$tmp/$file" | awk '{print $1}'); else sum=$(shasum -a 256 "$tmp/$file" | awk '{print $1}'); fi
    grep -q "^$sum  $file\$" "$tmp/SHASUMS256.txt" || { echo "checksum mismatch for $file"; exit 1; }
    tar -xzf "$tmp/$file" -C "$dir"
    ln -sfn "$dir/${file%.tar.gz}" "$dir/node"
    rm -rf "$tmp"
    node="$dir/node/bin/node"
  fi
  nodebin=$(dirname "$node")
  npm="$nodebin/npm"
  [ -x "$npm" ] || npm=$(command -v npm || true)
  [ -n "$npm" ] || { echo "Node $("$node" -v) has no npm"; exit 1; }
  step npm
  PATH="$nodebin:$PATH" "$npm" install -g --prefix "$dir/$a" "$pkg"
  printf '#!/bin/sh\nPATH="%s:$PATH" exec "%s" "$@"\n' "$nodebin" "$dir/$a/bin/$a" > "$HOME/.local/bin/$a"
  chmod 755 "$HOME/.local/bin/$a"
  ;;
*)
  echo "unknown assistant $a"; exit 1
  ;;
esac
step check
"$HOME/.local/bin/$a" --version
)SH";

// Without escape sequences and carriage returns.
std::string plain(const std::string &text)
{
    static const std::regex escapes("\x1b\\[[0-9;?<>=]*[ -/]*[@-~]|\x1b\\][^\x07\x1b]*(\x07|\x1b\\\\)|\x1b[@-Z\\\\-_]|\r");
    return std::regex_replace(text, escapes, "");
}

std::string lastLine(const std::string &text)
{
    std::string out;
    std::size_t end = text.size();
    while (end > 0) {
        const std::size_t start = text.rfind('\n', end - 1);
        const std::size_t from = start == std::string::npos ? 0 : start + 1;
        out = trim(text.substr(from, end - from));
        if (!out.empty())
            break;
        if (start == std::string::npos)
            break;
        end = start;
    }
    return cut(out, 300);
}

bool urlSafe(unsigned char c)
{
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

bool hasSpace(const std::string &text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) || c == '\'' || c == '"' || c == '<' || c == '>'; });
}

// The first link in `text`, put together when the terminal wrapped it over
// lines (a long line goes on in a next one without a space). `closed` is
// false while more of it may still come.
std::string findLink(const std::string &text, bool &closed)
{
    closed = false;
    const std::size_t at = text.find("https://");
    if (at == std::string::npos)
        return {};
    const std::size_t found = text.rfind('\n', at);
    std::size_t lineStart = found == std::string::npos ? 0 : found + 1;
    std::size_t end = at;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && std::strchr("'\"<>", text[end]) == nullptr)
        ++end;
    std::string link = text.substr(at, end - at);
    for (;;) {
        if (end >= text.size())
            return link;
        if (text[end] != '\n') {
            closed = true;
            return link;
        }
        if (end - lineStart < kWrapMin) {
            closed = true;
            return link;
        }
        const std::size_t nextEnd = text.find('\n', end + 1);
        const std::string next = trim(text.substr(end + 1, (nextEnd == std::string::npos ? text.size() : nextEnd) - end - 1));
        if (nextEnd == std::string::npos && next.empty())
            return link; // the next line has not started yet
        if (next.empty() || hasSpace(next)) {
            closed = true;
            return link;
        }
        if (nextEnd == std::string::npos)
            return link; // may be a word of the prompt; wait for more
        link += next;
        lineStart = end + 1;
        end = nextEnd;
    }
}

// The token `claude setup-token` printed, or empty: `sk-ant-oat` and
// URL-safe characters, put together when the terminal wrapped it. Anything
// else is not taken (an unrecognized output is "token-not-recognized").
std::string findToken(const std::string &text)
{
    static const std::string prefix = "sk-ant-oat";
    auto tokenChar = [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; };
    for (std::size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at + 1)) {
        if (at > 0 && tokenChar(static_cast<unsigned char>(text[at - 1])))
            continue;
        const std::size_t found = text.rfind('\n', at);
        std::size_t lineStart = found == std::string::npos ? 0 : found + 1;
        std::size_t end = at;
        while (end < text.size() && tokenChar(static_cast<unsigned char>(text[end])))
            ++end;
        std::string token = text.substr(at, end - at);
        while (end < text.size() && text[end] == '\n' && end - lineStart >= kWrapMin) {
            const std::size_t nextEnd = text.find('\n', end + 1);
            const std::string next = trim(text.substr(end + 1, (nextEnd == std::string::npos ? text.size() : nextEnd) - end - 1));
            if (next.empty() || !std::all_of(next.begin(), next.end(), [&](unsigned char c) { return tokenChar(c); }))
                break;
            token += next;
            lineStart = end + 1;
            end = nextEnd == std::string::npos ? text.size() : nextEnd;
        }
        if (token.size() >= kTokenMin && claudeTokenProblem(token).empty())
            return token;
    }
    return {};
}

} // namespace

std::string installScript(const std::string &assistant)
{
    std::string name = assistant;
    for (char &c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (const char *own = std::getenv(("PVAI_INSTALL_" + name).c_str()))
        return own;
    if (assistant != "claude" && assistant != "codex" && assistant != "kimi")
        return {};
    return kInstall;
}

Setup::Setup(Emit emit, Finished finished)
    : emit_(std::move(emit))
    , finished_(std::move(finished))
{
}

Setup::~Setup()
{
    for (auto &[name, task] : tasks_) {
        ::kill(-task.pid, SIGTERM);
        ::close(task.fd);
        ::waitpid(task.pid, nullptr, 0);
    }
}

std::string Setup::install(const std::string &assistant, const json &ref)
{
    if (busy(assistant))
        return "busy";
    const std::string script = installScript(assistant);
    if (script.empty())
        return "unknown-assistant";
    Task task;
    task.install = true;
    task.ref = ref;
    task.until = Clock::now() + kInstallTime;
    spawn(assistant, std::move(task), {"/bin/sh", "-c", script, "pvai-install", assistant}, false);
    return tasks_.count(assistant) ? "" : "cannot-start";
}

std::string Setup::login(const std::string &assistant, const std::string &binary, const json &ref, const std::string &method)
{
    if (busy(assistant))
        return "busy";
    const bool token = method == "token";
    if (!method.empty() && method != "account" && !token)
        return "bad-method";
    if (token && assistant != "claude")
        return "bad-method";
    std::vector<std::string> argv;
    if (assistant == "claude")
        argv = token ? std::vector<std::string>{binary, "setup-token"} : std::vector<std::string>{binary, "auth", "login"};
    else if (assistant == "codex")
        argv = {binary, "login", "--device-auth"};
    else if (assistant == "kimi")
        argv = {binary, "login"};
    else
        return "unknown-assistant";
    Task task;
    task.ref = ref;
    task.token = token;
    task.state = "waiting";
    task.until = Clock::now() + kLoginTime;
    spawn(assistant, std::move(task), argv, true);
    return tasks_.count(assistant) ? "" : "cannot-start";
}

// Claude Code `auth logout`, Codex `logout`; Kimi Code has no logout command.
std::string Setup::logout(const std::string &assistant, const std::string &binary, const json &ref)
{
    if (busy(assistant))
        return "busy";
    std::vector<std::string> argv;
    if (assistant == "claude")
        argv = {binary, "auth", "logout"};
    else if (assistant == "codex")
        argv = {binary, "logout"};
    else if (assistant == "kimi")
        return "not-supported";
    else
        return "unknown-assistant";
    Task task;
    task.logout = true;
    task.ref = ref;
    task.until = Clock::now() + kLogoutTime;
    spawn(assistant, std::move(task), argv, false);
    return tasks_.count(assistant) ? "" : "cannot-start";
}

std::string Setup::loginCode(const std::string &assistant, const std::string &code)
{
    const auto it = tasks_.find(assistant);
    if (it == tasks_.end() || it->second.install || it->second.logout)
        return "no-login";
    const std::string clean = oneLine(code, 512);
    if (clean.empty())
        return "empty";
    const std::string keys = clean + "\r";
    if (::write(it->second.fd, keys.data(), keys.size()) < 0)
        return "no-login";
    it->second.state = "waiting";
    event(assistant, it->second, "waiting");
    return {};
}

std::string Setup::loginCancel(const std::string &assistant)
{
    const auto it = tasks_.find(assistant);
    if (it == tasks_.end() || it->second.install || it->second.logout)
        return "no-login";
    ::kill(-it->second.pid, SIGTERM);
    ::waitpid(it->second.pid, nullptr, 0);
    end(assistant, -1, "cancelled");
    return {};
}

void Setup::spawn(const std::string &assistant, Task task, const std::vector<std::string> &argv, bool pty)
{
    int fd = -1;
    int out[2] = {-1, -1};
    ChildOptions options;
    options.argv = argv;
    options.search = true;
    options.cwd = homeDir();
    options.cwdFailure = 127;
    options.env = {{"BROWSER", "true"}, // no browser on this host: the link goes to the app
                   {"TERM", "dumb"}};
    if (pty) {
        options.ptyRows = 40;
        options.ptyCols = task.token ? kTokenCols : 200; // a long link on one line
        options.ptyMaster = &fd;
    } else if (::pipe(out) == 0) {
        fd = out[0];
        options.ownGroup = true;
        options.in = Stdio::null();
        options.out = Stdio::descriptor(out[1]);
        options.err = Stdio::sameAsOut();
    }
    const pid_t pid = pty || out[0] >= 0 ? spawnChild(options) : -1;
    if (pid < 0) {
        if (out[0] >= 0) {
            ::close(out[0]);
            ::close(out[1]);
        }
        return;
    }
    if (!pty)
        ::close(out[1]);
    setNonBlocking(fd);
    setCloseOnExec(fd);
    task.pid = pid;
    task.fd = fd;
    Task &stored = tasks_[assistant] = std::move(task);
    event(assistant, stored, stored.install || stored.logout ? "running" : "waiting");
}

std::vector<int> Setup::fds() const
{
    std::vector<int> out;
    for (const auto &[name, task] : tasks_)
        out.push_back(task.fd);
    return out;
}

void Setup::service(int fd)
{
    const auto it = std::find_if(tasks_.begin(), tasks_.end(), [&](const auto &entry) { return entry.second.fd == fd; });
    if (it == tasks_.end())
        return;
    const std::string assistant = it->first;
    Task &task = it->second;
    // A pty reads EIO once the child has gone.
    const bool eof = readAvailable(fd, task.buffer);
    if (task.install || task.logout) {
        for (auto nl = task.buffer.find('\n'); nl != std::string::npos; nl = task.buffer.find('\n')) {
            line(assistant, task, plain(task.buffer.substr(0, nl)));
            task.buffer.erase(0, nl + 1);
        }
        // An installer that only redraws with '\r' never ends a line.
        if (task.buffer.size() > kMaxOutput)
            task.buffer.erase(0, task.buffer.size() - kMaxOutput);
    } else {
        if (task.output.size() < kMaxOutput)
            task.output += plain(task.buffer);
        task.buffer.clear();
        loginOutput(assistant, task);
    }
    if (eof) {
        if ((task.install || task.logout) && !task.buffer.empty())
            line(assistant, task, plain(task.buffer));
        int status = 0;
        ::waitpid(task.pid, &status, 0);
        status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        std::string why;
        if (task.token && status == 0) {
            // Whole `setup-token` output is read; the token goes to its file
            // and nowhere else.
            const std::string found = findToken(task.output);
            if (found.empty()) {
                status = -1;
                why = "token-not-recognized";
            } else if (!storeClaudeToken(found)) {
                status = -1;
                why = "cannot-store-token";
            }
        } else if (task.token && status != 0) {
            why = "setup-token failed";
        }
        task.output.clear();
        end(assistant, status, why);
    }
}

void Setup::line(const std::string &assistant, Task &task, const std::string &text)
{
    const std::string clean = trim(text);
    if (clean.empty())
        return;
    if (clean.rfind(kStep, 0) == 0) {
        task.step = clean.substr(std::strlen(kStep));
        event(assistant, task, "running");
        return;
    }
    task.last = cut(clean, 300);
    event(assistant, task, "running");
}

// A login's link and code, as the three show them: Claude Code a link and
// then "Paste code here", Codex a device link and a code such as
// ABCD-12345, Kimi a link that holds its code and the code again.
void Setup::loginOutput(const std::string &assistant, Task &task)
{
    static const std::regex url("https://[^\\s'\"<>]+");
    static const std::regex code("\\b[A-Z0-9]{4}-[A-Z0-9]{4,5}\\b");
    bool news = false;
    std::smatch m;
    if (task.url.empty() && assistant == "claude") {
        // A link the terminal wrapped is put together, and taken when whole.
        bool closed = false;
        const std::string link = findLink(task.output, closed);
        if (closed && !link.empty()) {
            task.url = link;
            news = true;
        }
    } else if (task.url.empty() && std::regex_search(task.output, m, url)) {
        task.url = m.str();
        news = true;
    }
    if (assistant != "claude" && task.code.empty()) {
        const std::string afterUrl = task.url.empty() ? task.output : task.output.substr(task.output.find(task.url) + task.url.size());
        if (std::regex_search(afterUrl, m, code)) {
            task.code = m.str();
            news = true;
        }
    }
    if (assistant == "claude" && task.state != "code-needed" && task.output.find("Paste code") != std::string::npos) {
        task.state = "code-needed";
        task.output.clear(); // what comes after the code is what counts now
        news = true;
    }
    // The raw output of `setup-token` may hold the token: never passed on.
    if (const std::string last = lastLine(task.output); !task.token && !last.empty() && last != task.last) {
        task.last = last;
        news = true;
    }
    if (news)
        event(assistant, task, task.state);
}

void Setup::end(const std::string &assistant, int status, const std::string &why)
{
    const auto it = tasks_.find(assistant);
    if (it == tasks_.end())
        return;
    Task task = std::move(it->second);
    tasks_.erase(it);
    ::close(task.fd);
    const bool ok = status == 0;
    std::string error;
    if (!ok)
        error = !why.empty() ? why : !task.last.empty() ? task.last : "exit status " + std::to_string(status);
    event(assistant, task, ok ? "done" : "failed", error);
    finished_(assistant, kindOf(task), ok);
}

void Setup::event(const std::string &assistant, const Task &task, const std::string &state, const std::string &error)
{
    json text = task.last.empty() ? json() : json(task.last);
    if (task.token) {
        // Fixed words only: the output of `setup-token` is never passed on.
        text = state == "waiting" ? "Waiting for the sign-in" : state == "code-needed" ? "Waiting for the code"
               : state == "done" ? "Token stored" : json();
    }
    json message = {{"type", kindOf(task)}, {"ref", task.ref}, {"assistant", assistant}, {"state", state}, {"text", text}};
    if (task.logout) {
        // text only
    } else if (task.install) {
        message["step"] = task.step.empty() ? json() : json(task.step);
    } else {
        message["url"] = task.url.empty() ? json() : json(task.url);
        message["code"] = task.code.empty() ? json() : json(task.code);
    }
    if (!error.empty())
        message["error"] = error;
    emit_(message);
}

void Setup::tick(Clock::time_point now)
{
    std::vector<std::string> late;
    for (const auto &[name, task] : tasks_)
        if (now >= task.until)
            late.push_back(name);
    for (const auto &name : late) {
        ::kill(-tasks_[name].pid, SIGKILL);
        ::waitpid(tasks_[name].pid, nullptr, 0);
        end(name, -1, "timed out");
    }
}

std::optional<Setup::Clock::time_point> Setup::deadline() const
{
    std::optional<Clock::time_point> next;
    for (const auto &[name, task] : tasks_)
        if (!next || task.until < *next)
            next = task.until;
    return next;
}

} // namespace pvai
