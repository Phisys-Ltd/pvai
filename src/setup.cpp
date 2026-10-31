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
constexpr std::size_t kMaxOutput = 64 * 1024;
constexpr const char *kStep = "pvai-step: ";

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

std::string Setup::login(const std::string &assistant, const std::string &binary, const json &ref)
{
    if (busy(assistant))
        return "busy";
    std::vector<std::string> argv;
    if (assistant == "claude")
        argv = {binary, "auth", "login"};
    else if (assistant == "codex")
        argv = {binary, "login", "--device-auth"};
    else if (assistant == "kimi")
        argv = {binary, "login"};
    else
        return "unknown-assistant";
    Task task;
    task.ref = ref;
    task.state = "waiting";
    task.until = Clock::now() + kLoginTime;
    spawn(assistant, std::move(task), argv, true);
    return tasks_.count(assistant) ? "" : "cannot-start";
}

std::string Setup::loginCode(const std::string &assistant, const std::string &code)
{
    const auto it = tasks_.find(assistant);
    if (it == tasks_.end() || it->second.install)
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
    if (it == tasks_.end() || it->second.install)
        return "no-login";
    ::kill(-it->second.pid, SIGTERM);
    ::waitpid(it->second.pid, nullptr, 0);
    end(assistant, -1, "cancelled");
    return {};
}

void Setup::spawn(const std::string &assistant, Task task, const std::vector<std::string> &argv, bool pty)
{
    int fd = -1;
    pid_t pid = -1;
    int out[2] = {-1, -1};
    if (pty) {
        winsize size{};
        size.ws_row = 40;
        size.ws_col = 200; // a long link on one line
        pid = ::forkpty(&fd, nullptr, nullptr, &size);
    } else if (::pipe(out) == 0) {
        pid = ::fork();
        fd = out[0];
    }
    if (pid < 0) {
        if (out[0] >= 0) {
            ::close(out[0]);
            ::close(out[1]);
        }
        return;
    }
    if (pid == 0) {
        if (!pty) {
            ::setpgid(0, 0);
            const int null = ::open("/dev/null", O_RDONLY);
            ::dup2(null, STDIN_FILENO);
            ::dup2(out[1], STDOUT_FILENO);
            ::dup2(out[1], STDERR_FILENO);
        }
        const char *home = std::getenv("HOME");
        if (::chdir(home ? home : "/") != 0)
            ::_exit(127);
        dropClaudeEnvironment();
        ::setenv("BROWSER", "true", 1); // no browser on this host: the link goes to the app
        ::setenv("TERM", "dumb", 1);
        std::vector<char *> args;
        for (const auto &arg : argv)
            args.push_back(const_cast<char *>(arg.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    if (!pty)
        ::close(out[1]);
    setNonBlocking(fd);
    setCloseOnExec(fd);
    task.pid = pid;
    task.fd = fd;
    Task &stored = tasks_[assistant] = std::move(task);
    event(assistant, stored, stored.install ? "running" : "waiting");
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
    char chunk[4096];
    bool eof = false;
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n > 0) {
            task.buffer.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        // A pty reads EIO once the child has gone.
        eof = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        break;
    }
    if (task.install) {
        for (auto nl = task.buffer.find('\n'); nl != std::string::npos; nl = task.buffer.find('\n')) {
            line(assistant, task, plain(task.buffer.substr(0, nl)));
            task.buffer.erase(0, nl + 1);
        }
    } else {
        if (task.output.size() < kMaxOutput)
            task.output += plain(task.buffer);
        task.buffer.clear();
        loginOutput(assistant, task);
    }
    if (eof) {
        if (task.install && !task.buffer.empty())
            line(assistant, task, plain(task.buffer));
        int status = 0;
        ::waitpid(task.pid, &status, 0);
        end(assistant, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
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
    if (task.url.empty() && std::regex_search(task.output, m, url)) {
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
    if (const std::string last = lastLine(task.output); !last.empty() && last != task.last) {
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
    finished_(assistant, task.install, ok);
}

void Setup::event(const std::string &assistant, const Task &task, const std::string &state, const std::string &error)
{
    json message = {{"type", task.install ? "install" : "login"}, {"ref", task.ref}, {"assistant", assistant}, {"state", state},
                    {"text", task.last.empty() ? json() : json(task.last)}};
    if (task.install) {
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
