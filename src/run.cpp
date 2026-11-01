// pvai run: Claude Code in a pseudo-terminal of ours. The owner's terminal is
// passed through unchanged; the daemon may type a prompt into it, which it
// only does while Claude's prompt is free (the daemon knows from the hooks).
#include "common.h"
#include "run.h"
#include "screen.h"
#include "service.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <deque>
#include <optional>
#include <thread>
#include <cctype>
#include <cstdint>
#include <map>

namespace pvai {

namespace {

using Clock = std::chrono::steady_clock;

pid_t g_child = -1;
int g_winchPipe[2] = {-1, -1};
termios g_savedTermios;
bool g_rawTerminal = false;

void restoreTerminal()
{
    if (g_rawTerminal) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_savedTermios);
        g_rawTerminal = false;
    }
}

void onWinch(int)
{
    const int saved = errno;
    [[maybe_unused]] const auto n = ::write(g_winchPipe[1], "w", 1);
    errno = saved;
}

void forwardSignal(int signal)
{
    if (g_child > 0)
        ::kill(g_child, signal);
}

void spawnDaemon()
{
    // Installed as a service: the daemon belongs to systemd, not to us.
    if (startServiceDaemon())
        return;
    const pid_t first = ::fork();
    if (first < 0)
        return;
    if (first == 0) {
        ::setsid();
        if (::fork() != 0)
            ::_exit(0);
        dropClaudeEnvironment();
        const std::string log = runtimeDir() + "/daemon.log";
        const int in = ::open("/dev/null", O_RDONLY);
        const int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (in >= 0)
            ::dup2(in, STDIN_FILENO);
        if (out >= 0) {
            ::dup2(out, STDOUT_FILENO);
            ::dup2(out, STDERR_FILENO);
        }
        const std::string exe = selfExe();
        ::execl(exe.c_str(), "pvai", "daemon", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::waitpid(first, nullptr, 0);
}

// Connects to the daemon, starting it when nobody listens.
int connectDaemon(bool start)
{
    int fd = connectUnix(daemonSocket());
    if (fd >= 0 || !start)
        return fd;
    spawnDaemon();
    for (int i = 0; i < 30 && fd < 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fd = connectUnix(daemonSocket());
    }
    return fd;
}

// A rough idea of whether the owner has unsent text in Claude's prompt, from
// their keys alone: characters add, Backspace takes one away, Return, Ctrl+C
// and Ctrl+U empty it. Keys that fill the prompt with text we cannot count
// (history, Tab completion, a paste) leave it non-empty until it is emptied.
// Wrong towards "non-empty" only delays a prompt; the app shows why.
class Draft {
public:
    void feed(const char *data, std::size_t n)
    {
        if (n == 1 && data[0] == '\x1b')
            return; // a key of its own, not the start of a sequence
        for (std::size_t i = 0; i < n; ++i)
            feed(static_cast<unsigned char>(data[i]));
    }
    bool dirty() const { return count_ > 0; }

private:
    static constexpr int kUnknown = 1 << 20;

    void feed(unsigned char c)
    {
        if (!sequence_.empty()) {
            sequence_ += static_cast<char>(c);
            sequenceStep();
            return;
        }
        if (inPaste_) {
            if (c == 0x1b)
                sequence_ = "\x1b";
            else
                count_ = kUnknown;
            return;
        }
        switch (c) {
        case 0x1b: sequence_ = "\x1b"; return;
        case '\r': case '\n': case 0x03: case 0x15: count_ = 0; return;
        case 0x7f: case 0x08: count_ = std::max(0, count_ - 1); return;
        case '\t': count_ = kUnknown; return;
        default:
            if (c >= 0x20 && (c & 0xC0) != 0x80)
                ++count_;
        }
    }

    void sequenceStep()
    {
        const std::string &q = sequence_;
        if (q.size() == 2 && q[1] == '\r') { // Meta+Return: a newline in the prompt
            ++count_;
        } else if (q.size() == 2 && q[1] != '[' && q[1] != 'O') {
            // Esc then a key: the key counts on its own
            const unsigned char key = static_cast<unsigned char>(q[1]);
            sequence_.clear();
            feed(key);
            return;
        } else if (q.size() < 3 || static_cast<unsigned char>(q.back()) < 0x40 || static_cast<unsigned char>(q.back()) > 0x7e) {
            if (q.size() < 16)
                return; // not finished
        } else if (q == "\x1b[200~") {
            inPaste_ = true;
        } else if (q == "\x1b[201~") {
            inPaste_ = false;
        } else if (!inPaste_ && (q == "\x1b[A" || q == "\x1b[B" || q == "\x1bOA" || q == "\x1bOB")) {
            count_ = kUnknown; // history
        }
        sequence_.clear();
    }

    int count_ = 0;
    bool inPaste_ = false;
    std::string sequence_;
};

std::string currentDir()
{
    char buffer[PATH_MAX];
    return ::getcwd(buffer, sizeof buffer) ? buffer : "";
}

long long msUntil(Clock::time_point at)
{
    return std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(at - Clock::now()).count());
}

// Frames between `pvai attach` and a detached wrapper: a type byte, a
// big-endian 32-bit length, the data. 'd' is terminal data both ways, 'r'
// a resize from the viewer ("rows cols"), 'x' the end of the session.
std::string frame(char type, const std::string &data)
{
    std::string out(1, type);
    const auto n = static_cast<std::uint32_t>(data.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        out += static_cast<char>((n >> shift) & 0xff);
    return out + data;
}

bool nextFrame(std::string &buffer, char &type, std::string &data)
{
    if (buffer.size() < 5)
        return false;
    std::uint32_t n = 0;
    for (int i = 1; i <= 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
    if (buffer.size() < 5 + static_cast<std::size_t>(n))
        return false;
    type = buffer[0];
    data = buffer.substr(5, n);
    buffer.erase(0, 5 + n);
    return true;
}

// The terminal modes Claude Code switched on (bracketed paste, cursor,
// keyboard protocols), so a viewer that attaches later gets them too.
class ModeTracker {
public:
    void feed(const char *data, std::size_t n)
    {
        std::string text = tail_ + std::string(data, n);
        for (std::size_t at = 0; (at = text.find("\x1b[?", at)) != std::string::npos; ++at) {
            std::size_t end = at + 3;
            while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == ';'))
                ++end;
            if (end >= text.size() || (text[end] != 'h' && text[end] != 'l') || end == at + 3)
                continue;
            modes_[text.substr(at + 3, end - at - 3)] = text[end];
        }
        tail_ = text.size() > 24 ? text.substr(text.size() - 24) : text;
    }
    bool on(const std::string &mode) const
    {
        const auto it = modes_.find(mode);
        return it != modes_.end() && it->second == 'h';
    }
    std::string replay() const
    {
        std::string out;
        for (const auto &[mode, state] : modes_)
            out += "\x1b[?" + mode + state;
        return out;
    }

private:
    std::string tail_;
    std::map<std::string, char> modes_;
};

// Spots Claude Code's folder trust question in the raw output: the letters
// outside escape sequences, since the screen is drawn with cursor moves
// instead of spaces.
class TrustSpotter {
public:
    bool feed(const char *data, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            switch (state_) {
            case State::Text:
                if (c == 0x1b)
                    state_ = State::Escape;
                else if (std::isalpha(c))
                    letters_ += static_cast<char>(c);
                break;
            case State::Escape:
                state_ = c == '[' ? State::Csi : c == ']' ? State::Osc : State::Text;
                break;
            case State::Csi:
                if (c >= 0x40 && c <= 0x7e)
                    state_ = State::Text;
                break;
            case State::Osc:
                if (c == 0x07 || c == 0x1b)
                    state_ = State::Text;
                break;
            }
        }
        if (letters_.size() > 400)
            letters_.erase(0, letters_.size() - 200);
        if (seen_ || letters_.find("Itrustthisfolder") == std::string::npos)
            return false;
        seen_ = true;
        return true;
    }

private:
    enum class State { Text, Escape, Csi, Osc };
    State state_ = State::Text;
    std::string letters_;
    bool seen_ = false;
};

struct Viewer {
    std::unique_ptr<Conn> conn; // only its fd and buffers; frames are not lines
    std::string in;
    std::string out;
};

// The same command, going on with conversation `id`: an earlier
// --continue or --resume (with its id, if it had one) gives way to
// `--resume <id>`.
std::vector<std::string> resumeCommand(const std::vector<std::string> &command, const std::string &id)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const std::string &arg = command[i];
        if (i > 0 && (arg == "--continue" || arg == "-c" || arg.rfind("--resume=", 0) == 0))
            continue;
        if (i > 0 && (arg == "--resume" || arg == "-r")) {
            if (i + 1 < command.size() && command[i + 1].rfind("-", 0) != 0)
                ++i;
            continue;
        }
        out.push_back(arg);
    }
    out.push_back("--resume");
    out.push_back(id);
    return out;
}

} // namespace

int runWrapper(const WrapperOptions &options)
{
    const std::string id = options.id.empty() ? "w" + randomHex(4) : options.id;
    const std::string cwd = currentDir();
    const bool detached = options.detached;

    const bool tty = !detached && ::isatty(STDIN_FILENO);
    winsize size{};
    size.ws_row = 40;
    size.ws_col = 120;
    if (tty) {
        ::tcgetattr(STDIN_FILENO, &g_savedTermios);
        ::ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    }

    std::unique_ptr<Conn> daemon;
    if (const int fd = connectDaemon(true); fd >= 0)
        daemon = std::make_unique<Conn>(fd);
    else if (!detached)
        std::fprintf(stderr, "pvai: no daemon, running without voice\r\n");

    // A detached session is seen through `pvai attach` on this socket.
    const std::string viewerPath = runtimeDir() + "/" + id + ".sock";
    const int viewerListen = detached ? listenUnix(viewerPath) : -1;
    if (detached && viewerListen < 0)
        return 1;

    // The command runs in a pseudo-terminal; a `restart` from the daemon
    // runs it again in a new one, going on with the same conversation.
    std::vector<std::string> command = options.command;
    int master = -1;
    auto spawn = [&]() -> bool {
        g_child = ::forkpty(&master, nullptr, tty ? &g_savedTermios : nullptr, &size);
        if (g_child < 0) {
            std::perror("pvai: forkpty");
            return false;
        }
        if (g_child == 0) {
            ::setenv(kEnvId, id.c_str(), 1);
            ::setenv(kEnvSocket, daemonSocket().c_str(), 1);
            std::vector<char *> argv;
            for (const auto &arg : command)
                argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            std::fprintf(stderr, "pvai: cannot run %s: %s\n", argv[0], std::strerror(errno));
            ::_exit(127);
        }
        setNonBlocking(master);
        setCloseOnExec(master);
        return true;
    };
    if (!spawn())
        return 1;
    std::string restartWith; // the conversation to go on with once the child has ended
    std::optional<Clock::time_point> restartKillAt;

    if (tty) {
        termios raw = g_savedTermios;
        ::cfmakeraw(&raw);
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        g_rawTerminal = true;
        std::atexit(restoreTerminal);
    }
    if (::pipe(g_winchPipe) == 0) {
        setNonBlocking(g_winchPipe[0]);
        setCloseOnExec(g_winchPipe[0]);
        setCloseOnExec(g_winchPipe[1]);
        ::signal(SIGWINCH, onWinch);
    }
    ::signal(SIGTERM, forwardSignal);
    ::signal(SIGHUP, forwardSignal);

    Draft draft;
    ModeTracker modes;
    TrustSpotter trust;
    bool trustAsked = false;
    // A dialog no hook reports (a picker, a notice to confirm): read off
    // the screen once the output has paused.
    Screen screen(size.ws_row, size.ws_col);
    std::string screenShown;
    bool promptShown = false; // the prompt box is on the screen, no dialog over it
    bool shellShown = false;  // that box is in shell mode ("!"): taken, like a draft
    bool draftSent = false;   // what the daemon last heard as `draft`
    std::string modeShown;    // the permission mode the footer shows
    std::optional<Clock::time_point> screenCheckAt;
    auto sendScreen = [&] {
        daemon->send({{"type", "screen"}, {"dialog", screenShown.empty() ? json() : json(screenShown)}, {"prompt", promptShown},
                      {"mode", modeShown.empty() ? json() : json(modeShown)}});
    };
    auto promptFree = [&](std::string &box) {
        bool shell = false;
        return promptBox(screen, box, &shell) && !shell && screenDialog(screen).empty();
    };
    // Keys that go in one small group at a time: Claude Code drops control
    // keys that arrive in one large chunk.
    std::deque<std::string> paced;
    std::optional<Clock::time_point> pacedAt;
    // The owner's unsent words, a box in shell mode, or one being emptied:
    // no prompt goes in.
    auto reportDraft = [&](bool force) {
        const bool dirty = draft.dirty() || shellShown || !paced.empty();
        if (daemon && (force || dirty != draftSent))
            daemon->send({{"type", "draft"}, {"dirty", dirty}});
        draftSent = dirty;
    };
    // A prompt typed with `verify`: pasted only into a free prompt box, and
    // Return only once the box shows it; else it did not go in.
    struct Typing {
        std::string before; // the box before the paste
        Clock::time_point until;
    };
    std::optional<Typing> typing;
    auto typed = [&](bool ok) {
        if (daemon)
            daemon->send({{"type", "typed"}, {"ok", ok}, {"screen", ok ? json() : json(screenSnapshot(screen))}});
    };
    bool registered = false;
    auto registerSelf = [&] {
        daemon->send({{"type", "register"}, {"id", id}, {"label", options.label}, {"cwd", cwd},
                      {"pid", static_cast<int>(::getpid())}, {"running", registered}, {"detached", detached},
                      {"restart", true}, {"verify", true}, {"modes", true}, {"clear", true}});
        if (draft.dirty() || shellShown)
            reportDraft(true);
        if (trustAsked)
            daemon->send({{"type", "dialog"}, {"kind", "trust"}});
        if (!screenShown.empty())
            sendScreen();
        registered = true;
    };
    if (daemon)
        registerSelf();

    std::vector<std::unique_ptr<Viewer>> viewers;
    std::string toMaster;
    bool stdinOpen = !detached;
    std::optional<Clock::time_point> returnAt;
    std::optional<Clock::time_point> resizeBackAt; // the second half of a redraw nudge
    Clock::time_point lastInputNote{};
    Clock::time_point nextReconnect = Clock::now() + std::chrono::seconds(2);

    // Keys from the owner, from the terminal or an attached viewer.
    auto userBytes = [&](const char *data, std::size_t n) {
        toMaster.append(data, n);
        draft.feed(data, n);
        if (!daemon)
            return;
        reportDraft(false);
        // A lone Esc (not the start of a key sequence) may interrupt the turn.
        if (n == 1 && data[0] == '\x1b') {
            daemon->send({{"type", "input"}, {"escape", true}});
            lastInputNote = Clock::now();
        } else if (Clock::now() - lastInputNote > std::chrono::seconds(1)) {
            daemon->send({{"type", "input"}});
            lastInputNote = Clock::now();
        }
    };

    // Claude Code redraws its whole screen on a size change; a viewer that
    // just attached needs that.
    auto resize = [&](unsigned short rows, unsigned short cols, bool nudge) {
        if (rows == 0 || cols < 2)
            return;
        size.ws_row = rows;
        size.ws_col = cols;
        screen.resize(rows, cols);
        if (nudge) {
            winsize smaller = size;
            --smaller.ws_col;
            ::ioctl(master, TIOCSWINSZ, &smaller);
            resizeBackAt = Clock::now() + std::chrono::milliseconds(120);
        } else {
            ::ioctl(master, TIOCSWINSZ, &size);
        }
    };

    // After a restart's child has ended: the same conversation in a fresh
    // pseudo-terminal, with the screen state of a new start.
    auto relaunch = [&]() -> bool {
        ::close(master);
        command = resumeCommand(command, restartWith);
        restartWith.clear();
        restartKillAt.reset();
        if (!spawn())
            return false;
        draft = Draft();
        modes = ModeTracker();
        trust = TrustSpotter();
        trustAsked = false;
        screen.reset();
        screenCheckAt.reset();
        if (daemon && !screenShown.empty()) {
            screenShown.clear();
            sendScreen();
        }
        screenShown.clear();
        promptShown = false;
        shellShown = false;
        modeShown.clear();
        if (typing) {
            typing.reset();
            typed(false);
        }
        toMaster.clear();
        returnAt.reset();
        paced.clear();
        pacedAt.reset();
        reportDraft(false);
        return true;
    };

    for (;;) {
        std::vector<pollfd> fds;
        fds.push_back({master, static_cast<short>(POLLIN | (toMaster.empty() ? 0 : POLLOUT)), 0});
        fds.push_back({stdinOpen ? STDIN_FILENO : -1, POLLIN, 0});
        fds.push_back({g_winchPipe[0], POLLIN, 0});
        fds.push_back({daemon ? daemon->fd() : -1, static_cast<short>(POLLIN | (daemon && daemon->wantsWrite() ? POLLOUT : 0)), 0});
        fds.push_back({viewerListen, POLLIN, 0});
        for (const auto &viewer : viewers)
            fds.push_back({viewer->conn->fd(), static_cast<short>(POLLIN | (viewer->out.empty() ? 0 : POLLOUT)), 0});

        long long timeout = -1;
        auto soonest = [&](long long ms) { timeout = timeout < 0 ? ms : std::min(timeout, ms); };
        if (returnAt)
            soonest(msUntil(*returnAt));
        if (pacedAt)
            soonest(msUntil(*pacedAt));
        if (resizeBackAt)
            soonest(msUntil(*resizeBackAt));
        if (screenCheckAt)
            soonest(msUntil(*screenCheckAt));
        if (typing)
            soonest(std::min(msUntil(typing->until), 50LL));
        if (!daemon)
            soonest(msUntil(nextReconnect));
        if (!restartWith.empty())
            soonest(100);
        if (::poll(fds.data(), fds.size(), static_cast<int>(timeout)) < 0 && errno != EINTR)
            break;

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[65536];
            const ssize_t n = ::read(master, buffer, sizeof buffer);
            if (n > 0) {
                const std::string chunk(buffer, static_cast<std::size_t>(n));
                if (!detached)
                    writeAll(STDOUT_FILENO, chunk);
                for (auto &viewer : viewers)
                    if (viewer->out.size() < 8 * 1024 * 1024)
                        viewer->out += frame('d', chunk);
                modes.feed(buffer, static_cast<std::size_t>(n));
                screen.feed(buffer, static_cast<std::size_t>(n));
                screenCheckAt = Clock::now() + std::chrono::milliseconds(250);
                if (trust.feed(buffer, static_cast<std::size_t>(n))) {
                    trustAsked = true;
                    if (daemon)
                        daemon->send({{"type", "dialog"}, {"kind", "trust"}});
                }
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                if (restartWith.empty())
                    break; // Claude Code is gone
                ::waitpid(g_child, nullptr, 0);
                if (!relaunch())
                    break;
                continue;
            }
        }
        if ((fds[0].revents & POLLOUT) && !toMaster.empty()) {
            const ssize_t n = ::write(master, toMaster.data(), toMaster.size());
            if (n > 0)
                toMaster.erase(0, static_cast<std::size_t>(n));
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char buffer[4096];
            const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n > 0)
                userBytes(buffer, static_cast<std::size_t>(n));
            else if (n == 0)
                stdinOpen = false;
        }
        if (fds[2].revents & POLLIN) {
            char drain[64];
            while (::read(g_winchPipe[0], drain, sizeof drain) > 0) {
            }
            winsize now{};
            if (tty && ::ioctl(STDIN_FILENO, TIOCGWINSZ, &now) == 0)
                resize(now.ws_row, now.ws_col, false);
        }
        if (daemon && fds[3].revents) {
            std::vector<json> messages;
            bool alive = daemon->readMessages(messages) && daemon->flush();
            for (const auto &message : messages) {
                const std::string type = message.is_object() ? message.value("type", "") : "";
                if (type == "type") {
                    std::string text = message.value("text", "");
                    std::string box;
                    if (message.value("verify", false) && (typing || !paced.empty() || !promptFree(box))) {
                        typed(false);
                        continue;
                    }
                    if (modes.on("2004")) {
                        toMaster += "\x1b[200~" + text + "\x1b[201~";
                    } else {
                        for (char &c : text)
                            if (c == '\n' || c == '\r')
                                c = ' ';
                        toMaster += text;
                    }
                    // Return as a separate keystroke, after the paste has landed
                    // (with `verify`: once the prompt box shows it).
                    if (message.value("verify", false))
                        typing = Typing{box, Clock::now() + std::chrono::milliseconds(2000)};
                    else
                        returnAt = Clock::now() + std::chrono::milliseconds(150);
                } else if (type == "keys") {
                    toMaster += message.value("data", "");
                } else if (type == "clear") {
                    // Empties the prompt box wherever the cursor is, also a
                    // long or multi-line one, and leaves shell mode: Ctrl+K
                    // and Ctrl+U each take one line, Backspace into an empty
                    // box ends shell mode. Esc does neither.
                    paced.assign(40, "\x0b\x15");
                    paced.insert(paced.end(), 3, "\x7f");
                    pacedAt = Clock::now();
                    draft = Draft();
                    reportDraft(false);
                } else if (type == "snapshot") {
                    daemon->send({{"type", "snapshot"}, {"screen", screenSnapshot(screen)}});
                } else if (type == "terminate") {
                    restartWith.clear();
                    forwardSignal(SIGHUP);
                } else if (type == "restart") {
                    restartWith = message.value("resume", "");
                    if (!restartWith.empty()) {
                        forwardSignal(SIGHUP);
                        restartKillAt = Clock::now() + std::chrono::seconds(5);
                    }
                }
            }
            if (!alive) {
                daemon.reset();
                nextReconnect = Clock::now() + std::chrono::seconds(2);
            }
        }
        if (fds[4].revents & POLLIN) {
            if (const int fd = ::accept(viewerListen, nullptr, nullptr); fd >= 0) {
                auto viewer = std::make_unique<Viewer>();
                viewer->conn = std::make_unique<Conn>(fd);
                viewer->out = frame('d', "\x1b[0m\x1b[2J\x1b[H" + modes.replay());
                viewers.push_back(std::move(viewer));
            }
        }
        for (std::size_t i = 0; i < viewers.size(); ++i) {
            Viewer &viewer = *viewers[i];
            const short revents = fds[5 + i].revents;
            bool alive = true;
            if (revents & (POLLIN | POLLHUP | POLLERR)) {
                char buffer[16384];
                const ssize_t n = ::read(viewer.conn->fd(), buffer, sizeof buffer);
                if (n > 0)
                    viewer.in.append(buffer, static_cast<std::size_t>(n));
                else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                    alive = false;
                char type;
                std::string data;
                while (nextFrame(viewer.in, type, data)) {
                    if (type == 'd') {
                        userBytes(data.data(), data.size());
                    } else if (type == 'r') {
                        unsigned rows = 0, cols = 0;
                        if (std::sscanf(data.c_str(), "%u %u", &rows, &cols) == 2)
                            resize(static_cast<unsigned short>(rows), static_cast<unsigned short>(cols), true);
                    }
                }
            }
            if (alive && !viewer.out.empty()) {
                const ssize_t n = ::write(viewer.conn->fd(), viewer.out.data(), viewer.out.size());
                if (n > 0)
                    viewer.out.erase(0, static_cast<std::size_t>(n));
                else if (n < 0 && errno != EAGAIN && errno != EINTR)
                    alive = false;
            }
            if (!alive)
                viewer.conn.reset();
        }
        std::erase_if(viewers, [](const auto &v) { return !v->conn; });

        // A child that has ended while something it started still holds
        // the terminal open gives no end of file: look for it directly.
        if (!restartWith.empty()) {
            if (::waitpid(g_child, nullptr, WNOHANG) == g_child) {
                if (!relaunch())
                    break;
                continue;
            }
            if (restartKillAt && Clock::now() >= *restartKillAt) {
                ::kill(g_child, SIGKILL);
                restartKillAt.reset();
            }
        }
        if (resizeBackAt && Clock::now() >= *resizeBackAt) {
            ::ioctl(master, TIOCSWINSZ, &size);
            resizeBackAt.reset();
        }
        if (typing) {
            std::string box;
            const bool shown = promptBox(screen, box) && box != typing->before && screenDialog(screen).empty();
            if (shown || Clock::now() >= typing->until) {
                typing.reset();
                if (shown)
                    returnAt = Clock::now() + std::chrono::milliseconds(100);
                typed(shown);
            }
        }
        if (screenCheckAt && Clock::now() >= *screenCheckAt) {
            screenCheckAt.reset();
            std::string box;
            bool shell = false;
            std::string shown = screenDialog(screen);
            const bool prompt = shown.empty() && promptBox(screen, box, &shell);
            const std::string mode = shown.empty() ? screenMode(screen) : modeShown;
            if (prompt && shell != shellShown) {
                shellShown = shell;
                reportDraft(false);
            }
            if (shown != screenShown || prompt != promptShown || mode != modeShown) {
                screenShown = std::move(shown);
                promptShown = prompt;
                modeShown = mode;
                if (daemon)
                    sendScreen();
            }
        }
        if (pacedAt && Clock::now() >= *pacedAt) {
            toMaster += paced.front();
            paced.pop_front();
            if (paced.empty()) {
                pacedAt.reset();
                reportDraft(false); // the screen says when shell mode is gone
            } else
                pacedAt = Clock::now() + std::chrono::milliseconds(20);
        }
        if (returnAt && Clock::now() >= *returnAt) {
            toMaster += "\r";
            returnAt.reset();
        }
        if (!daemon && Clock::now() >= nextReconnect) {
            if (const int fd = connectDaemon(true); fd >= 0) {
                daemon = std::make_unique<Conn>(fd);
                registerSelf();
            }
            nextReconnect = Clock::now() + std::chrono::seconds(2);
        }
    }

    int status = 0;
    ::waitpid(g_child, &status, 0);
    // How it ended, with the last lines it showed ("command not found",
    // "Not logged in"): the daemon tells the app when a session ends early.
    if (daemon) {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        daemon->send({{"type", "exited"}, {"status", code}, {"screen", screenSnapshot(screen)}});
        const auto until = Clock::now() + std::chrono::seconds(1);
        while (daemon->wantsWrite() && Clock::now() < until) {
            pollfd p{daemon->fd(), POLLOUT, 0};
            ::poll(&p, 1, 100);
            if (!daemon->flush())
                break;
        }
    }
    restoreTerminal();
    for (auto &viewer : viewers) {
        ::fcntl(viewer->conn->fd(), F_SETFL, 0);
        writeAll(viewer->conn->fd(), viewer->out + frame('x', ""));
    }
    if (detached)
        ::unlink(viewerPath.c_str());
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int openDaemon(bool start)
{
    return connectDaemon(start);
}

// pvai attach: a terminal onto a detached session; Ctrl+\ leaves it running.
int runAttach(const std::string &id)
{
    const int fd = connectUnix(runtimeDir() + "/" + id + ".sock");
    if (fd < 0) {
        std::fprintf(stderr, "pvai: no detached session %s on this host\n", id.c_str());
        return 1;
    }
    if (!::isatty(STDIN_FILENO)) {
        std::fprintf(stderr, "pvai: attach needs a terminal\n");
        return 1;
    }
    ::tcgetattr(STDIN_FILENO, &g_savedTermios);
    termios raw = g_savedTermios;
    ::cfmakeraw(&raw);
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_rawTerminal = true;
    std::atexit(restoreTerminal);
    if (::pipe(g_winchPipe) == 0) {
        setNonBlocking(g_winchPipe[0]);
        ::signal(SIGWINCH, onWinch);
    }
    auto sendSize = [&] {
        winsize ws{};
        ::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        writeAll(fd, frame('r', std::to_string(ws.ws_row) + " " + std::to_string(ws.ws_col)));
    };
    sendSize();

    std::string in;
    bool ended = false;
    for (;;) {
        pollfd fds[3] = {{STDIN_FILENO, POLLIN, 0}, {fd, POLLIN, 0}, {g_winchPipe[0], POLLIN, 0}};
        if (::poll(fds, 3, -1) < 0 && errno != EINTR)
            break;
        if (fds[0].revents & POLLIN) {
            char buffer[4096];
            const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n <= 0)
                break;
            std::string keys(buffer, static_cast<std::size_t>(n));
            if (const auto quit = keys.find('\x1c'); quit != std::string::npos) {
                keys.resize(quit);
                if (!keys.empty())
                    writeAll(fd, frame('d', keys));
                break;
            }
            writeAll(fd, frame('d', keys));
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[65536];
            const ssize_t n = ::read(fd, buffer, sizeof buffer);
            if (n <= 0) {
                ended = true;
                break;
            }
            in.append(buffer, static_cast<std::size_t>(n));
            char type;
            std::string data;
            while (nextFrame(in, type, data)) {
                if (type == 'd')
                    writeAll(STDOUT_FILENO, data);
                else if (type == 'x')
                    ended = true;
            }
            if (ended)
                break;
        }
        if (fds[2].revents & POLLIN) {
            char drain[64];
            while (::read(g_winchPipe[0], drain, sizeof drain) > 0) {
            }
            sendSize();
        }
    }
    ::close(fd);
    // Leave the screen as a shell expects it.
    writeAll(STDOUT_FILENO, "\x1b[0m\x1b[?2004l\x1b[?25h\x1b[<u\r\n");
    restoreTerminal();
    std::fprintf(stderr, ended ? "pvai: the session ended\n" : "pvai: detached from %s, it keeps running\n", id.c_str());
    return 0;
}

} // namespace pvai
