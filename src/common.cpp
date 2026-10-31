#include "common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

namespace pvai {

namespace {

std::string home()
{
    const char *h = std::getenv("HOME");
    return h && *h ? h : "/";
}

void makeDir(const std::string &path, mode_t mode)
{
    if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create " + path + ": " + std::strerror(errno));
}

bool fillUnixAddress(sockaddr_un &address, const std::string &path)
{
    std::memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path)
        return false;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

} // namespace

std::string runtimeDir()
{
    std::string dir;
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        dir = std::string(xdg) + "/pvai";
    } else {
        const char *tmp = std::getenv("TMPDIR");
        std::string base = tmp && *tmp ? tmp : "/tmp";
        while (base.size() > 1 && base.back() == '/')
            base.pop_back();
        dir = base + "/pvai-" + std::to_string(::getuid());
    }
    makeDir(dir, 0700);
    return dir;
}

std::string daemonSocket()
{
    return runtimeDir() + "/daemon.sock";
}

std::string configDir()
{
    std::string base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else
        base = home() + "/.config";
    makeDir(base, 0755);
    makeDir(base + "/pvai", 0700);
    return base + "/pvai";
}

std::string token()
{
    const std::string path = configDir() + "/token";
    {
        std::ifstream in(path);
        std::string existing;
        if (in && std::getline(in, existing) && !existing.empty())
            return existing;
    }
    const std::string fresh = randomHex(16);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || !writeAll(fd, fresh + "\n"))
        throw std::runtime_error("cannot write " + path);
    ::close(fd);
    return fresh;
}

std::string selfExe()
{
    char buffer[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        char resolved[PATH_MAX];
        if (::realpath(buffer, resolved))
            return resolved;
        return buffer;
    }
    return "pvai";
#else
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n <= 0)
        return "pvai";
    std::string path(buffer, static_cast<std::size_t>(n));
    // Replaced on disk since we started (an update): the new file is at the
    // same path.
    const std::string deleted = " (deleted)";
    if (path.size() > deleted.size() && path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0)
        path.resize(path.size() - deleted.size());
    return path;
#endif
}

std::string hostName()
{
    char buffer[256] = {};
    ::gethostname(buffer, sizeof buffer - 1);
    std::string name = buffer;
    if (const auto dot = name.find('.'); dot != std::string::npos)
        name.resize(dot);
    return name;
}

const char *const kVoiceContext =
    "This session runs under pvai (PhiVoice AI): the owner can talk to it from a "
    "push-to-talk app and hears the end of each turn. A prompt that starts with \xF0\x9F\x8E\x99 was "
    "spoken and transcribed by speech recognition: expect misheard names and technical words and read "
    "them by meaning; ask back only when the meaning is really unclear. When a turn's prompt started "
    "with \xF0\x9F\x8E\x99, end your final answer with one line that starts with \xF0\x9F\x94\x8A and "
    "holds one or two plain sentences for the ear in the conversation's language: what you did or "
    "found, or what you need. No code, paths, markdown or lists in that line; the owner may be away "
    "from the screen. To write to another session, also on another host (the owner may say \"ask M4\" or "
    "\"tell the taifun session\"), list them with `pvai peers` and send with `pvai propose HOST/SESSION "
    "\"TEXT\"` (to answer a message, the command its first line names, with `--reply`); the owner releases "
    "each message in the app, so it arrives later, and the answer comes back as a new prompt.";

std::string findBinary(const std::string &name)
{
    const char *home = std::getenv("HOME");
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    path += ":" + std::string(home ? home : "") + "/.local/bin:/usr/local/bin:/opt/homebrew/bin";
    std::size_t at = 0;
    while (at <= path.size()) {
        auto end = path.find(':', at);
        if (end == std::string::npos)
            end = path.size();
        const std::string candidate = path.substr(at, end - at) + "/" + name;
        struct stat st;
        if (end > at && ::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(candidate.c_str(), X_OK) == 0)
            return candidate;
        at = end + 1;
    }
    return {};
}

std::string randomHex(std::size_t bytes)
{
    std::string raw(bytes, '\0');
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.read(raw.data(), static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("cannot read /dev/urandom");
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const unsigned char c : raw) {
        hex += digits[c >> 4];
        hex += digits[c & 15];
    }
    return hex;
}

void dropClaudeEnvironment()
{
    std::vector<std::string> names;
    for (char **e = environ; *e; ++e) {
        const std::string name(*e, std::strcspn(*e, "="));
        // The owner's login token is theirs, not the parent session's.
        if (name == "CLAUDE_CODE_OAUTH_TOKEN")
            continue;
        if (name.rfind("CLAUDE_CODE_", 0) == 0 || name == "CLAUDECODE" || name == kEnvId || name == kEnvSocket)
            names.push_back(name);
    }
    for (const auto &name : names)
        ::unsetenv(name.c_str());
}

void spawnDetached(const std::string &cwd, const std::vector<std::string> &argv)
{
    const pid_t first = ::fork();
    if (first != 0) {
        if (first > 0)
            ::waitpid(first, nullptr, 0);
        return;
    }
    ::setsid();
    if (::fork() != 0)
        ::_exit(0);
    if (::chdir(cwd.c_str()) != 0)
        ::_exit(1);
    dropClaudeEnvironment();
    const int null = ::open("/dev/null", O_RDWR);
    ::dup2(null, STDIN_FILENO);
    ::dup2(null, STDOUT_FILENO);
    ::dup2(null, STDERR_FILENO);
    std::vector<char *> args;
    for (const auto &arg : argv)
        args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);
    ::execv(args[0], args.data());
    ::_exit(127);
}

int connectUnix(const std::string &path)
{
    sockaddr_un address;
    if (!fillUnixAddress(address, path))
        return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setCloseOnExec(fd);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int listenUnix(const std::string &path)
{
    sockaddr_un address;
    if (!fillUnixAddress(address, path))
        return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setCloseOnExec(fd);
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0
        || ::listen(fd, 16) != 0) {
        ::close(fd);
        return -1;
    }
    ::chmod(path.c_str(), 0600);
    return fd;
}

int listenTcp(const std::string &host, int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setCloseOnExec(fd);
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1
        || ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0
        || ::listen(fd, 16) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void setNonBlocking(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

void setCloseOnExec(int fd)
{
    ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

bool writeAll(int fd, const std::string &data)
{
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void sendLine(int fd, const json &message)
{
    writeAll(fd, message.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
}

PlainStream::PlainStream(int fd)
    : fd_(fd)
{
    setNonBlocking(fd_);
    setCloseOnExec(fd_);
}

PlainStream::~PlainStream()
{
    ::close(fd_);
}

long PlainStream::read(char *buffer, std::size_t size)
{
    for (;;) {
        const ssize_t n = ::read(fd_, buffer, size);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? kWouldBlock : kError;
    }
}

long PlainStream::write(const char *data, std::size_t size)
{
    for (;;) {
        const ssize_t n = ::write(fd_, data, size);
        if (n > 0)
            return n;
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? kWouldBlock : kError;
    }
}

Conn::Conn(int fd)
    : stream_(std::make_unique<PlainStream>(fd))
{
}

Conn::Conn(std::unique_ptr<Stream> stream)
    : stream_(std::move(stream))
{
}

void Conn::send(const json &message)
{
    out_ += message.dump(-1, ' ', false, json::error_handler_t::replace);
    out_ += '\n';
}

bool Conn::readMessages(std::vector<json> &messages)
{
    char buffer[16384];
    bool open = true;
    for (;;) {
        const long n = stream_->read(buffer, sizeof buffer);
        if (n == Stream::kWouldBlock)
            break;
        if (n <= 0) {
            open = false; // the lines before the end still count
            break;
        }
        in_.append(buffer, static_cast<std::size_t>(n));
        if (in_.size() > maxLine_ + sizeof buffer)
            break; // too much at once; the check below ends it
    }
    std::size_t start = 0;
    for (std::size_t end; (end = in_.find('\n', start)) != std::string::npos; start = end + 1) {
        if (end - start > maxLine_)
            return false;
        const std::string line = in_.substr(start, end - start);
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        messages.push_back(json::parse(line, nullptr, false));
        if (messages.back().is_discarded())
            messages.back() = nullptr;
    }
    in_.erase(0, start);
    return open && in_.size() <= maxLine_;
}

bool Conn::flush()
{
    while (!out_.empty()) {
        const long n = stream_->write(out_.data(), out_.size());
        if (n == Stream::kWouldBlock)
            return true;
        if (n <= 0)
            return false;
        out_.erase(0, static_cast<std::size_t>(n));
    }
    return true;
}

} // namespace pvai
