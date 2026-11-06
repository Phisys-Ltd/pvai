// Shared pieces of the test programs: check(), a temp HOME, the daemon as a
// child process, a line client on its socket, a popen helper and a base for
// the fake agents. Header-only; every function is inline.
#pragma once

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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pvai::test {

// ---- check() and the summary

inline int &failures()
{
    static int count = 0;
    return count;
}

inline void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures();
}

// Prints the summary; the exit code of the test.
inline int finish()
{
    std::printf("%s\n", failures() ? "FAILED" : "all passed");
    return failures() ? 1 : 0;
}

// ---- A temporary directory. With `setEnv` it is HOME, and its run/ is
// XDG_RUNTIME_DIR. Removed with the object.

class TestHome {
public:
    explicit TestHome(const std::string &prefix = "/tmp/pvai-test-", bool setEnv = true)
    {
        std::string pattern = prefix + "XXXXXX";
        if (!::mkdtemp(pattern.data()))
            std::exit(2);
        path = pattern;
        if (!setEnv)
            return;
        runtime = path + "/run";
        std::filesystem::create_directories(runtime);
        std::filesystem::permissions(runtime, std::filesystem::perms::owner_all);
        ::setenv("HOME", path.c_str(), 1);
        ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
        ::setenv("PVAI_HOST", "testhost", 1); // daemons and CLIs see one host name everywhere
    }
    ~TestHome()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TestHome(const TestHome &) = delete;
    TestHome &operator=(const TestHome &) = delete;

    std::string path;
    std::string runtime;
};

// ---- The daemon as a child process.

class DaemonProcess {
public:
    DaemonProcess() = default;
    ~DaemonProcess() { stop(); }
    DaemonProcess(const DaemonProcess &) = delete;
    DaemonProcess &operator=(const DaemonProcess &) = delete;

    // Forks `pvai daemon --port`, with `env` set in the child, waits for the
    // socket and then `settleMs` more. `dieWithParent`: gone with the test,
    // even when it crashes.
    void start(const std::string &pvai, int port, int settleMs = 500, bool dieWithParent = false,
               const std::vector<std::pair<std::string, std::string>> &env = {})
    {
        pid_ = ::fork();
        if (pid_ == 0) {
#ifdef __linux__
            if (dieWithParent)
                ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#else
            (void)dieWithParent;
#endif
            for (const auto &[name, value] : env)
                ::setenv(name.c_str(), value.c_str(), 1);
            const std::string portText = std::to_string(port);
            ::execl(pvai.c_str(), "pvai", "daemon", "--port", portText.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!socketUp() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
    }

    // SIGTERM and reap; nothing when it is gone already.
    void stop()
    {
        if (pid_ <= 0)
            return;
        ::kill(pid_, SIGTERM);
        ::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }

    // Waits for it to end by itself; its wait status.
    int wait()
    {
        int status = -1;
        if (pid_ > 0)
            ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return status;
    }

private:
    static bool socketUp()
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", daemonSocket().c_str());
        const bool up = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
        ::close(fd);
        return up;
    }

    pid_t pid_ = -1;
};

// ---- A line client on the daemon's unix socket.

class LineClient {
public:
    LineClient()
    {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", daemonSocket().c_str());
        ::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    }
    ~LineClient() { ::close(fd_); }
    LineClient(const LineClient &) = delete;
    LineClient &operator=(const LineClient &) = delete;

    void send(const json &message)
    {
        const std::string line = message.dump() + "\n";
        (void)::write(fd_, line.data(), line.size());
    }

    // The next message within `ms`; an empty object after (so that a
    // missing answer fails a check instead of the test).
    json next(int ms = 2000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (const auto end = buffer_.find('\n'); end != std::string::npos) {
                const std::string line = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                return json::parse(line, nullptr, false);
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            pollfd p{fd_, POLLIN, 0};
            if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0)
                return json::object();
            char chunk[65536];
            const ssize_t n = ::read(fd_, chunk, sizeof chunk);
            if (n <= 0)
                return json::object();
            buffer_.append(chunk, static_cast<std::size_t>(n));
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

private:
    int fd_ = -1;
    std::string buffer_;
};

// ---- Running a command

struct RunResult {
    int status = -1;
    std::string out; // stdout and stderr, trailing line ends cut
};

// `command 2>&1` through popen. `trim`: cut the trailing line ends.
inline RunResult runCommand(const std::string &command, bool trim = true)
{
    RunResult result;
    FILE *pipe = ::popen((command + " 2>&1").c_str(), "r");
    if (!pipe)
        return result;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe))
        result.out += buffer;
    const int status = ::pclose(pipe);
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    while (trim && !result.out.empty() && (result.out.back() == '\n' || result.out.back() == '\r'))
        result.out.pop_back();
    return result;
}

// Runs `pvai args` with `env` before it and `input` on stdin.
inline RunResult runPvai(const std::string &pvai, const std::string &args, const std::string &env = "", const std::string &input = "")
{
    return runCommand((input.empty() ? "" : "printf %s '" + input + "' | ") + env + " " + pvai + " " + args);
}

// ---- The base of the fake agents: a unix socket with one client at a time
// and a thread that moves messages. A subclass decides how they are framed
// (encode, onData) and answers (onMessage); it calls start() at the end of
// its constructor and stop() at the start of its destructor.

class FakeServer {
public:
    // `holdUntilConnected`: messages sent while nobody is connected wait for
    // the client; otherwise they are dropped.
    explicit FakeServer(const std::string &path, bool holdUntilConnected = false) : hold_(holdUntilConnected)
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        listen_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        ::bind(listen_, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        ::listen(listen_, 4);
    }
    virtual ~FakeServer() { stop(); }
    FakeServer(const FakeServer &) = delete;
    FakeServer &operator=(const FakeServer &) = delete;

    // To the client, from any thread.
    void send(const json &message)
    {
        std::lock_guard lock(mutex_);
        out_.push_back(message);
    }
    void respond(const json &request, const json &result)
    {
        send({{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}});
    }

    // The first message from the daemon that matches, waiting up to `ms`;
    // null when none came. `remove`: take it off the list.
    json take(const std::function<bool(const json &)> &pred, int ms = 3000, bool remove = true)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard lock(mutex_);
                for (auto it = in_.begin(); it != in_.end(); ++it)
                    if (pred(*it)) {
                        json m = *it;
                        if (remove)
                            in_.erase(it);
                        return m;
                    }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return nullptr;
    }

protected:
    void start() { thread_ = std::thread([this] { loop(); }); }
    void stop()
    {
        if (!thread_.joinable())
            return;
        stop_ = true;
        thread_.join();
        ::close(listen_);
    }

    // A connection came in (the loop thread); the client after this.
    virtual void onAccept(int /*fd*/) {}
    // Bytes from the client (the loop thread). By default lines of JSON.
    virtual void onData(const std::string &chunk)
    {
        lines_.append(chunk);
        for (auto end = lines_.find('\n'); end != std::string::npos; end = lines_.find('\n')) {
            json m = json::parse(lines_.substr(0, end), nullptr, false);
            lines_.erase(0, end + 1);
            push(m);
            onMessage(m);
        }
    }
    // One message was received and stored (the loop thread).
    virtual void onMessage(const json & /*message*/) {}
    // A message on the wire.
    virtual std::string encode(const json &message) { return message.dump() + "\n"; }

    // Stores a received message for take().
    void push(const json &message)
    {
        std::lock_guard lock(mutex_);
        in_.push_back(message);
    }
    // Writes at once; the loop thread only.
    void writeNow(const json &message)
    {
        const std::string data = encode(message);
        if (client_ >= 0)
            (void)::write(client_, data.data(), data.size());
    }

private:
    void loop()
    {
        while (!stop_) {
            pollfd p[2] = {{listen_, POLLIN, 0}, {client_, POLLIN, 0}};
            ::poll(p, 2, 20);
            if (p[0].revents & POLLIN) {
                const int fd = ::accept(listen_, nullptr, nullptr);
                onAccept(fd);
                client_ = fd;
            }
            if (client_ >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
                char chunk[65536];
                const ssize_t n = ::read(client_, chunk, sizeof chunk);
                if (n <= 0) {
                    ::close(client_);
                    client_ = -1;
                } else {
                    onData(std::string(chunk, static_cast<std::size_t>(n)));
                }
            }
            std::deque<json> out;
            {
                std::lock_guard lock(mutex_);
                if (!hold_ || client_ >= 0)
                    out.swap(out_);
            }
            for (const auto &m : out)
                writeNow(m);
        }
        if (client_ >= 0)
            ::close(client_);
    }

    bool hold_;
    int listen_ = -1;
    int client_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<json> out_;
    std::deque<json> in_;
    std::string lines_;
};

} // namespace pvai::test
