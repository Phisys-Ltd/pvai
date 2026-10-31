// Installing an assistant and logging it in, from the app (PROTOCOL.md,
// "Install and login"): one child process per assistant at a time, its
// output read as it comes. An install is a shell script (steps marked with
// "pvai-step: NAME" lines); a login runs the assistant's own login command
// in a pseudo-terminal, which shows a link and a code (Claude Code asks for
// a code back instead).
#pragma once

#include "common.h"

#include <sys/types.h>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pvai {

class Setup {
public:
    using Clock = std::chrono::steady_clock;
    // Every event goes to every app; `finished` runs when a task ended
    // (the assistant's name, whether it was an install, and success).
    using Emit = std::function<void(const json &event)>;
    using Finished = std::function<void(const std::string &assistant, bool install, bool ok)>;

    Setup(Emit emit, Finished finished);
    ~Setup();
    Setup(const Setup &) = delete;
    Setup &operator=(const Setup &) = delete;

    // Each returns the reason for a `rejected`, or empty. `binary` is the
    // assistant's program for a login.
    std::string install(const std::string &assistant, const json &ref);
    std::string login(const std::string &assistant, const std::string &binary, const json &ref);
    std::string loginCode(const std::string &assistant, const std::string &code);
    std::string loginCancel(const std::string &assistant);
    bool busy(const std::string &assistant) const { return tasks_.count(assistant) > 0; }

    // The daemon's loop.
    std::vector<int> fds() const;
    void service(int fd);
    void tick(Clock::time_point now);
    std::optional<Clock::time_point> deadline() const;

private:
    struct Task {
        bool install = false;
        json ref;
        pid_t pid = -1;
        int fd = -1;
        std::string buffer;  // output not yet a whole line
        std::string output;  // the output so far, without escape sequences (a login's)
        std::string step;    // install: the current step
        std::string state;   // login: waiting, code-needed
        std::string url, code, last;
        Clock::time_point until;
    };

    void spawn(const std::string &assistant, Task task, const std::vector<std::string> &argv, bool pty);
    void line(const std::string &assistant, Task &task, const std::string &text);
    void loginOutput(const std::string &assistant, Task &task);
    void end(const std::string &assistant, int status, const std::string &why = {});
    void event(const std::string &assistant, const Task &task, const std::string &state, const std::string &error = {});

    Emit emit_;
    Finished finished_;
    std::map<std::string, Task> tasks_;
};

// The install script for an assistant (claude, codex, kimi), or empty;
// PVAI_INSTALL_<NAME> replaces it (a shell command, for tests).
std::string installScript(const std::string &assistant);

} // namespace pvai
