// pvai daemon: the host's live sessions and the app's side of
// docs/PROTOCOL.md. What a session is and how it runs is its backend's
// business (session.h); the daemon routes messages to them by id.
//
// Three kinds of peers, all line-framed JSON:
// - wrappers (`pvai run`) on the Unix socket, first line `register`; they type
//   what their session tells them into their Claude Code;
// - hooks (`pvai hook`) on the Unix socket, one `hook` line each, then gone;
// - apps on 127.0.0.1:7470 after a `hello` with the token, or over direct
//   access (TLS). Local tools (`pvai sessions`, `pvai watch`) use the Unix
//   socket and skip the token: the runtime directory is the user's own.
#include "claude.h"
#include "codex_backend.h"
#include "kimi_backend.h"
#include "common.h"
#include "inbox.h"
#include "pairing.h"
#include "setup.h"
#include "session.h"
#include "share.h"
#include "tls.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace pvai {

namespace fs = std::filesystem;

namespace {

// An upload connection after its `upload` line (PROTOCOL.md, "Files"): what
// follows on it is the file's bytes, not lines.
struct UploadState {
    json ref;
    std::string session;
    unsigned long long size = 0;
    std::unique_ptr<InboxUpload> file; // null once refused: the rest is only read and dropped
    unsigned long long drain = 0;      // bytes still to read and drop after a refusal
};

// A download connection after its `download` line (PROTOCOL.md, "Files"): the
// daemon writes one file of a proposal to it in chunks and closes it. The file
// is read from a descriptor opened at the request, so a dismissal in between
// does not cut the transfer.
struct DownloadState {
    int fd = -1;
    std::string memory;      // a `fetch`: the whole file, read before the header; fd is -1 then
    std::size_t offset = 0;  // how much of `memory` went out
    unsigned long long remaining = 0;
    std::chrono::steady_clock::time_point progress = std::chrono::steady_clock::now(); // the last time bytes left
    DownloadState() = default;
    DownloadState(const DownloadState &) = delete;
    DownloadState &operator=(const DownloadState &) = delete;
    ~DownloadState()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

// ---- fetch (PROTOCOL.md, "Files"): a read-only copy of a file a session named ----

bool underDir(const std::string &real, const std::string &dir)
{
    if (dir.empty() || dir == "/")
        return false;
    return real == dir || real.compare(0, dir.size() + 1, dir + "/") == 0;
}

std::string realOr(const std::string &path)
{
    char buf[PATH_MAX];
    return ::realpath(path.c_str(), buf) ? std::string(buf) : path;
}

// Files and directories fetch never serves, wherever they lie (also inside a
// working directory, also when a session printed them).
bool protectedPath(const std::string &real)
{
    const std::string home = homeDir();
    const std::string config = configDir(); // pvai's devices, TLS key, tokens
    std::vector<std::string> dirs = {home + "/.ssh", home + "/.config/phi-secrets", home + "/.kimi/credentials", config, runtimeDir()};
    for (const std::string &dir : dirs)
        if (underDir(real, realOr(dir)) || underDir(real, dir))
            return true;
    for (const std::string &file : {home + "/.claude/.credentials.json", home + "/.codex/auth.json", claudeTokenPath()})
        if (real == realOr(file) || real == file)
            return true;
    return false;
}

bool nameChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || (static_cast<unsigned char>(c) >= 0x80);
}

// `text` holds `path` as a whole path: not the middle of a longer one.
bool textHasPath(const std::string &text, const std::string &path)
{
    for (std::size_t at = text.find(path); at != std::string::npos; at = text.find(path, at + 1)) {
        if (at > 0 && (nameChar(text[at - 1]) || text[at - 1] == '/' || text[at - 1] == '.'))
            continue;
        const std::size_t end = at + path.size();
        if (end < text.size()) {
            const char c = text[end];
            if (nameChar(c) || c == '/')
                continue;
            if (c == '.' && end + 1 < text.size() && nameChar(text[end + 1]))
                continue;
        }
        return true;
    }
    return false;
}

bool jsonHasPath(const json &value, const std::string &path, const std::string &other)
{
    if (value.is_string()) {
        const std::string &text = value.get_ref<const std::string &>();
        return textHasPath(text, path) || (other != path && textHasPath(text, other));
    }
    if (value.is_array() || value.is_object())
        for (const auto &item : value)
            if (jsonHasPath(item, path, other))
                return true;
    return false;
}

// Process: an assistant's own process that pvai started in a terminal
// (`pvai codex`); its connection lasts as long as the process.
enum class Kind { Unknown, Wrapper, App, Process };

struct Peer {
    std::unique_ptr<Conn> conn;
    unsigned long long serial = 0; // to find it again after an asynchronous call
    Kind kind = Kind::Unknown;
    bool tcp = false; // loopback TCP or direct access: no wrapper or hook messages
    // Direct access (TLS): the peer's address; a paired device's fingerprint
    // and name, or the fingerprint of a client in a pairing window.
    std::string address;
    std::string device;
    std::string deviceName;
    std::string pairing;
    bool guest = false; // a device that has a share: see "Sharing a session" (its entry holds the rest)
    SSL *ssl = nullptr; // owned by conn's stream
    std::optional<std::chrono::steady_clock::time_point> deadline; // before login
    std::chrono::steady_clock::time_point lastReceived = std::chrono::steady_clock::now();
    std::string client;                 // apps: `hello.client`
    std::string sessionId;              // wrappers
    std::string assistant;              // processes: whose
    std::set<std::string> subscriptions; // apps: sessions whose transcript they follow
    std::unique_ptr<UploadState> upload; // set once the connection sent `upload`: not an app any more
    std::unique_ptr<DownloadState> download; // the same for `download`

    // A connection that left the app protocol: no broadcasts, no keepalive.
    bool detached() const { return upload || download; }
};

constexpr int kDirectPort = 7471;
constexpr int kPairWindowSeconds = 600;
constexpr auto kPreAuth = std::chrono::seconds(10);
// An app on TCP pings every 15 s; one silent for this long is gone.
constexpr auto kAppSilence = std::chrono::seconds(60);
constexpr std::size_t kMaxOut = 4 * 1024 * 1024; // unsent bytes of one peer
constexpr std::size_t kMaxHandshakes = 32;
// After a refused upload at most this much of the rest is read and dropped, so
// that the answer is not lost in a reset; then the connection closes.
constexpr unsigned long long kMaxDrain = 16ull * 1024 * 1024;
constexpr auto kInboxSweep = std::chrono::hours(1);

// An upload connection that sends nothing for this long is short (60 s;
// PVAI_UPLOAD_IDLE seconds overrides it, for tests).
std::chrono::seconds uploadIdle()
{
    static const std::chrono::seconds idle = [] {
        const char *text = std::getenv("PVAI_UPLOAD_IDLE");
        const long seconds = text ? std::atol(text) : 0;
        return std::chrono::seconds(seconds > 0 ? seconds : 60);
    }();
    return idle;
}

// A download connection whose peer takes no byte for this long is dead (the
// same 60 s, and the same override, as an upload's idle time).
std::chrono::seconds downloadIdle() { return uploadIdle(); }

// The proposal limits (PROTOCOL.md, "Agent messages"); PVAI_PROPOSAL_LIMITS
// "FILE,PROPOSAL,OUTBOX" in bytes overrides them, for tests.
struct ProposalLimits {
    unsigned long long file = kMaxUploadFile, proposal = kMaxProposalBytes, outbox = kMaxOutboxBytes;
};

const ProposalLimits &proposalLimits()
{
    static const ProposalLimits limits = [] {
        ProposalLimits l;
        if (const char *text = std::getenv("PVAI_PROPOSAL_LIMITS")) {
            unsigned long long a = 0, b = 0, c = 0;
            if (std::sscanf(text, "%llu,%llu,%llu", &a, &b, &c) == 3) {
                l.file = a;
                l.proposal = b;
                l.outbox = c;
            }
        }
        return l;
    }();
    return limits;
}

std::chrono::seconds inboxSweepEvery()
{
    static const std::chrono::seconds every = [] {
        const char *text = std::getenv("PVAI_INBOX_SWEEP");
        const long seconds = text ? std::atol(text) : 0;
        return seconds > 0 ? std::chrono::seconds(seconds) : std::chrono::duration_cast<std::chrono::seconds>(kInboxSweep);
    }();
    return every;
}

struct Handshake {
    std::unique_ptr<TlsAccept> accept;
    std::string address;
    std::chrono::steady_clock::time_point deadline;
    bool done = false;
};

struct PairWindow {
    std::string code;
    std::chrono::steady_clock::time_point until;
    unsigned long long waiter = 0; // the `pvai pair` that waits for the result
};

// An open invitation to one session (PROTOCOL.md, "Sharing a session"): a
// pairing window of its own that lasts as long as the share would, is used
// up by one device, and is kept in memory only.
struct Invite {
    std::string id, code;
    std::string session, assistant, conversation;
    bool dialogs = false;
    long long expires = 0; // Unix time, 0: never
    std::optional<Clock::time_point> until; // none: never
};
constexpr std::size_t kMaxInvites = 16;
// A file as `stat` shows it, to see that it changed.
struct FileSig {
    long long mtime = -1, size = -1, inode = -1;
    bool operator==(const FileSig &) const = default;
};
// A share's session may be missing this long (a restart, a daemon that just
// started, a Codex app server coming back) before the share has `ended`.
// PVAI_SHARE_GRACE seconds overrides it, for tests.
std::chrono::seconds shareGrace()
{
    static const std::chrono::seconds grace = [] {
        const char *text = std::getenv("PVAI_SHARE_GRACE");
        const long seconds = text ? std::atol(text) : 0;
        return std::chrono::seconds(seconds > 0 ? seconds : 120);
    }();
    return grace;
}

// An agent message waiting for the owner (PROTOCOL.md, "Agent messages").
// A prompt of an ended Claude session, kept for its conversation.
struct KeptPrompt {
    std::string conversation, cwd, session; // session: the ended one, as `dropped` named it
    Prompt prompt;
    Clock::time_point expires;
    unsigned long long seq;
};
constexpr auto kKeptLife = std::chrono::hours(24);
constexpr std::size_t kMaxKeptPerConversation = 16, kMaxKept = 64;

// A file offered with a proposal: a copy in the outbox.
struct ProposalFile {
    std::string id; // f1, f2, ...
    std::string name;
    unsigned long long size = 0;
    std::string sha256;
    std::string source; // where it was, as the agent gave it
    std::string path;   // the copy in the outbox
};

struct Proposal {
    std::string id;
    std::string from; // the sender's session id, empty from a Codex thread
    std::string fromLabel;
    std::string to;   // host or host/session
    std::string text;
    std::string replyTo; // the proposal this one answers, or empty
    long long at = 0; // Unix time
    Clock::time_point expires;
    std::vector<ProposalFile> files;
};

constexpr std::size_t kMaxProposals = 16;
constexpr std::size_t kProposalsPerMinute = 10;
constexpr auto kProposalLife = std::chrono::hours(24);
constexpr std::size_t kMaxPeers = 256;

struct DirectListener {
    int fd = -1;
    std::string address;
    int port = 0;
};


// A child process the daemon waits for (`Host::runProcess`).
struct Job {
    pid_t pid = -1;
    int fd = -1;
    std::string output;
    Clock::time_point deadline;
    std::function<void(int status, const std::string &output)> done;
    // Its output has ended (or it timed out): the process is reaped by the
    // loop without waiting, SIGKILL for one that stays; never a blocking wait.
    bool ended = false;
    bool timedOut = false;
    bool killed = false;
};

constexpr std::size_t kMaxJobOutput = 64 * 1024;
// An ended job's process that is still there is killed after this.
constexpr auto kJobKillAfter = std::chrono::seconds(2);
// `installed` is read again on a `hello` when it is older than this.
constexpr auto kVersionAge = std::chrono::minutes(10);

// `host` or `host/session`: a short hostname, then a session id or label.
// A proposal's id as the daemon makes them: "p" and hex digits.
bool goodProposalId(const std::string &id)
{
    return id.size() >= 2 && id.size() <= 33 && id[0] == 'p'
        && std::all_of(id.begin() + 1, id.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

bool goodTarget(const std::string &to)
{
    const auto slash = to.find('/');
    const std::string host = to.substr(0, slash);
    if (host.empty() || host.size() > 64)
        return false;
    for (const char c : host)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.' && c != '_')
            return false;
    if (slash == std::string::npos)
        return true;
    const std::string session = to.substr(slash + 1);
    return !session.empty() && session.size() <= 128 && oneLine(session, 128) == session;
}


class Daemon : public Host {
public:
    Daemon(int unixFd, int appFd, int tcpFd, int port)
        : unixFd_(unixFd)
        , appFd_(appFd)
        , tcpFd_(tcpFd)
        , port_(port)
    {
        loadLabels();
        devicesFile_ = devicesPath();
        prompts_ = std::make_unique<GuestPrompts>(configDir() + "/guest-prompts.json");
        refreshGuests(true);
        // The assistants this daemon knows, in the order the app lists them.
        auto claude = std::make_unique<ClaudeBackend>(*this);
        claude_ = claude.get();
        backends_.push_back(std::move(claude));
        backends_.push_back(std::make_unique<CodexBackend>(*this));
        backends_.push_back(std::make_unique<KimiBackend>(*this));
        setup_ = std::make_unique<Setup>([this](const json &event) { broadcast(event); },
                                         [this](const std::string &name, const std::string &kind, bool ok) { setupFinished(name, kind, ok); });
        loadListen();
        refreshVersions();
        // What an earlier run left: old files and empty directories. Sessions
        // of other runs may not have come back yet, so no directory is judged by its session.
        cleanInbox(nullptr, kInboxLife);
        clearOutbox(); // proposals live in memory only: no copy outlives the daemon
        nextSweep_ = Clock::now() + inboxSweepEvery();
    }

    // One step of the loop that reads what other programs wrote (a transcript,
    // an assistant's output): an exception in it is logged, never fatal, and
    // the loop goes on. Only the kind of failure is logged, no content.
    template <class F> void guarded(const std::string &what, F &&step)
    {
        try {
            step();
        } catch (const json::exception &e) {
            logLine(what + " failed: JSON error " + std::to_string(e.id));
        } catch (const std::exception &e) {
            logLine(what + " failed: " + oneLine(e.what(), 120));
        }
    }

    int run()
    {
        for (;;) {
            std::vector<pollfd> fds;
            fds.push_back({unixFd_, POLLIN, 0});
            fds.push_back({tcpFd_, POLLIN, 0});
            fds.push_back({direct_.fd, POLLIN, 0});
            fds.push_back({appFd_, POLLIN, 0});
            const std::size_t firstBackend = fds.size();
            for (const auto &backend : backends_)
                fds.push_back({backend->fd(), backend->events(), 0});
            const std::size_t firstJob = fds.size();
            for (const auto &job : jobs_)
                fds.push_back({job.ended ? -1 : job.fd, POLLIN, 0}); // an ended job's EOF is known
            const std::size_t firstSetup = fds.size();
            const std::vector<int> setupFds = setup_->fds();
            for (const int fd : setupFds)
                fds.push_back({fd, POLLIN, 0});
            const std::size_t firstHandshake = fds.size();
            for (const auto &h : handshakes_)
                fds.push_back({h.accept->fd(), h.accept->events(), 0});
            const std::size_t firstPeer = fds.size();
            for (const auto &peer : peers_)
                fds.push_back({peer->conn->fd(),
                               static_cast<short>(POLLIN | (peer->conn->wantsWrite() || (peer->download && peer->download->remaining > 0) ? POLLOUT : 0)), 0});
            if (::poll(fds.data(), fds.size(), nextTimeout()) < 0 && errno != EINTR)
                return 1;
            refreshGuests();
            const auto now = Clock::now();
            for (Session *session : sessions())
                guarded("session " + session->id() + " tick", [&] { session->tick(now); });
            for (std::size_t i = 0; i < backends_.size(); ++i)
                if (fds[firstBackend + i].fd >= 0 && fds[firstBackend + i].revents)
                    guarded(backends_[i]->name() + " service", [&] { backends_[i]->service(); });
            if (fds[0].revents & POLLIN)
                accept(unixFd_, false);
            if (fds[1].revents & POLLIN)
                accept(tcpFd_, true, true);
            if (direct_.fd >= 0 && fds[2].fd == direct_.fd && (fds[2].revents & POLLIN))
                acceptDirect();
            if (appFd_ >= 0 && fds[3].fd == appFd_ && (fds[3].revents & POLLIN))
                accept(appFd_, true, false);
            // Peers, jobs and handshakes may be added or dropped while
            // handling; walk snapshots.
            std::vector<Peer *> ready;
            for (std::size_t i = firstPeer; i < fds.size(); ++i)
                if (fds[i].revents)
                    ready.push_back(peers_[i - firstPeer].get());
            std::vector<pid_t> readable;
            for (std::size_t i = firstJob; i < firstSetup; ++i)
                if (fds[i].revents)
                    readable.push_back(jobs_[i - firstJob].pid);
            std::vector<int> setupReady;
            for (std::size_t i = firstSetup; i < firstHandshake; ++i)
                if (fds[i].revents)
                    setupReady.push_back(fds[i].fd);
            std::vector<Handshake *> shaking;
            for (std::size_t i = firstHandshake; i < firstPeer; ++i)
                if (fds[i].revents)
                    shaking.push_back(&handshakes_[i - firstHandshake]);
            for (Handshake *h : shaking)
                stepHandshake(*h);
            for (Peer *peer : ready)
                service(peer);
            serviceJobs(readable);
            for (const int fd : setupReady)
                setup_->service(fd);
            setup_->tick(Clock::now());
            for (const auto &backend : backends_)
                guarded(backend->name() + " tick", [&] { backend->tick(Clock::now()); });
            directTick();
            expireProposals();
            expireKept();
            sweepSessions();
            sweepInbox();
            shareTick();
            reap();
            if (prompts_->dirty() && !prompts_->save())
                logLine("guest prompts: cannot write " + configDir() + "/guest-prompts.json");
            if (sessionsDirty_) {
                sessionsDirty_ = false;
                broadcast(sessionsMessage());
            }
            if (stopping_) {
                // What was said goes out before the sockets close.
                const auto until = Clock::now() + std::chrono::seconds(2);
                for (bool pending = true; pending && Clock::now() < until;) {
                    pending = false;
                    for (auto &peer : peers_)
                        if (peer->conn->wantsWrite() && peer->conn->flush() && peer->conn->wantsWrite())
                            pending = true;
                    if (pending)
                        ::poll(nullptr, 0, 20);
                }
                logLine("stopped");
                return 0;
            }
        }
    }

    // ---- Host ----

    void broadcast(const json &message) override
    {
        std::string wire;
        for (auto &peer : peers_)
            if (peer->kind == Kind::App && !peer->detached()) {
                if (peer->guest) {
                    if (const auto view = guestView(*peer, message))
                        peer->conn->send(*view);
                    continue;
                }
                if (wire.empty())
                    wire = Conn::line(message);
                peer->conn->sendRaw(wire);
            }
    }

    void sessionsChanged() override { sessionsDirty_ = true; }
    void assistantsChanged() override { broadcast(assistantsMessage()); }

    void toSubscribers(const std::string &id, const json &message) override
    {
        std::string wire;
        for (auto &peer : peers_)
            if (peer->subscriptions.count(id)) {
                if (peer->guest) {
                    if (const auto view = guestView(*peer, message))
                        peer->conn->send(*view);
                    continue;
                }
                if (wire.empty())
                    wire = Conn::line(message);
                peer->conn->sendRaw(wire);
            }
    }

    Session *session(const std::string &id) override
    {
        const auto it = sessions_.find(id);
        return it == sessions_.end() || it->second->ended() ? nullptr : it->second.get();
    }

    std::vector<Session *> sessions() override
    {
        std::vector<Session *> out;
        for (auto &[id, session] : sessions_)
            if (!session->ended())
                out.push_back(session.get());
        return out;
    }

    void addSession(std::unique_ptr<Session> session) override
    {
        const std::string id = session->id();
        // A rename kept over a restart wins over what the assistant says.
        if (session->assistant() != "claude")
            if (const auto saved = savedLabels_.find(labelKey(*session)); saved != savedLabels_.end() && saved->is_string())
                session->applyLabel(saved->get<std::string>());
        sessions_[id] = std::move(session);
        sessionsDirty_ = true;
    }

    void runProcess(const std::vector<std::string> &argv, std::chrono::seconds timeout,
                    std::function<void(int status, const std::string &output)> done,
                    const std::vector<std::pair<std::string, std::string>> &env) override
    {
        int out[2];
        if (argv.empty() || ::pipe(out) != 0) {
            done(127, "cannot start");
            return;
        }
        ChildOptions options;
        options.argv = argv;
        options.search = true;
        options.ownGroup = true;
        options.env = env;
        options.in = Stdio::null();
        options.out = Stdio::descriptor(out[1]);
        options.err = Stdio::sameAsOut();
        const pid_t pid = spawnChild(options);
        ::close(out[1]);
        if (pid < 0) {
            ::close(out[0]);
            done(127, "cannot start");
            return;
        }
        setNonBlocking(out[0]);
        setCloseOnExec(out[0]);
        jobs_.push_back({pid, out[0], {}, Clock::now() + timeout, std::move(done)});
    }

    void log(const std::string &text) override { logLine(text); }

    // A prompt entry a guest sent says who (PROTOCOL.md, "Sharing a session").
    void decorateEntries(Session &session, std::vector<json> &entries) override
    {
        const std::string conversation = conversationOf(session);
        if (conversation.empty())
            return;
        for (json &entry : entries)
            prompts_->annotate(conversation, entry);
    }

    // The history as an app sees it: `from` on guests' prompts, and the
    // daemon's own entries (who answered a dialog) where their time puts them.
    json historyView(Session &session, int limit) override
    {
        json entries = session.history(limit);
        const std::string conversation = conversationOf(session);
        if (!conversation.empty())
            for (json &entry : entries)
                prompts_->annotate(conversation, entry);
        const auto own = daemonEntries_.find(session.id());
        if (own == daemonEntries_.end() || own->second.empty())
            return entries;
        std::vector<json> merged(entries.begin(), entries.end());
        for (const json &entry : own->second) {
            const std::string time = str(entry, "time");
            auto at = merged.end();
            while (at != merged.begin()) {
                const std::string before = str(*(at - 1), "time");
                if (before.empty() || before <= time)
                    break;
                --at;
            }
            merged.insert(at, entry);
        }
        const std::size_t keep = static_cast<std::size_t>(std::max(0, limit));
        if (merged.size() > keep)
            merged.erase(merged.begin(), merged.end() - static_cast<long>(keep));
        return json(merged);
    }

    void setAppSocket(const std::string &path) { appSocket_ = path; }

private:
    // `tcp`: an app (token in hello, no wrapper or hook messages); `inet`:
    // over TCP, not the app socket.
    void accept(int listenFd, bool tcp, bool inet = false)
    {
        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0)
            return;
        auto peer = std::make_unique<Peer>();
        peer->conn = std::make_unique<Conn>(fd);
        peer->serial = ++serial_;
        peer->tcp = tcp;
        if (!tcp)
            peer->conn->setMaxLine(kMaxLocalLine);
        peer->deadline = Clock::now() + kPreAuth; // cleared by hello (or the wrapper's register)
        if (inet)
            keepAlive(fd);
        peers_.push_back(std::move(peer));
    }

    void service(Peer *peer)
    {
        bool open = peer->conn->fill();
        bool tooLong = false;
        bool any = false;
        json message;
        // One line at a time: an `upload` line ends line framing, and the
        // bytes after it (already buffered or still to come) are the file.
        while (!peer->conn->closing() && !peer->detached() && peer->conn->next(message, tooLong)) {
            any = true;
            try {
                handle(peer, message);
            } catch (const json::exception &e) {
                // A field of the wrong JSON type: this peer's problem only.
                logLine("bad message from " + (peer->address.empty() ? std::string("a local peer") : peer->address) + ": " + e.what());
                peer->conn->send({{"type", "error"}, {"code", "bad-message"}, {"text", "a field has the wrong type"}});
                peer->conn->closeAfterFlush();
                if (!peer->pairing.empty() && window_) {
                    // Counts as a bad proof: the window closes.
                    const unsigned long long waiter = window_->waiter;
                    window_.reset();
                    directFailure(peer->address, "bad proof");
                    notifyWaiter(waiter, {{"type", "pair-closed"}, {"reason", "bad-proof"}});
                }
                break;
            }
        }
        if (any)
            peer->lastReceived = Clock::now();
        if (peer->upload) {
            serviceUpload(peer, open);
        } else if (peer->download) {
            // Whatever the peer sends after its request is dropped.
            peer->conn->takeBuffered();
            if (!open)
                dead_.push_back(peer);
            else if (!peer->conn->closing())
                pumpDownload(peer);
        } else if (!open || tooLong || peer->conn->overlong()) {
            dead_.push_back(peer);
        }
        if (peer->pairing.empty() && peer->kind != Kind::Unknown)
            peer->deadline.reset();
        if (!peer->conn->flush())
            dead_.push_back(peer);
        else if (peer->conn->closing() && !peer->conn->wantsWrite())
            dead_.push_back(peer);
    }

    // ---- Uploads (PROTOCOL.md, "Files") ----

    void uploadFail(Peer *peer, const std::string &reason, unsigned long long drain)
    {
        UploadState &up = *peer->upload;
        logLine("upload to " + (up.session.empty() ? std::string("?") : up.session) + " rejected: " + reason);
        up.file.reset(); // the .part goes
        peer->conn->send({{"type", "rejected"}, {"ref", up.ref}, {"session", up.session}, {"reason", reason}});
        up.drain = std::min(drain, kMaxDrain);
        if (up.drain == 0)
            peer->conn->closeAfterFlush();
    }

    void uploadDone(Peer *peer)
    {
        UploadState &up = *peer->upload;
        const std::string reason = up.file->finish();
        if (!reason.empty())
            return uploadFail(peer, reason, 0);
        const json done = {{"type", "uploaded"}, {"ref", up.ref}, {"session", up.session}, {"path", up.file->path()},
                           {"name", up.file->name()}, {"size", up.size}};
        logLine("upload to " + up.session + ": " + std::to_string(up.size) + " bytes stored");
        peer->conn->send(done);
        peer->conn->closeAfterFlush();
        broadcast(done); // the upload connection itself is no app
        up.file.reset();
    }

    // Bytes of the file, or of what is dropped after a refusal.
    void uploadBytes(Peer *peer, const char *data, std::size_t size)
    {
        UploadState &up = *peer->upload;
        if (peer->conn->closing())
            return;
        if (!up.file) {
            up.drain -= std::min<unsigned long long>(up.drain, size);
            if (up.drain == 0)
                peer->conn->closeAfterFlush();
            return;
        }
        const unsigned long long take = std::min<unsigned long long>(size, up.size - up.file->received());
        if (take > 0 && !up.file->write(data, take))
            return uploadFail(peer, "write-failed", up.size - up.file->received());
        if (up.file->received() == up.size)
            uploadDone(peer);
    }

    // Reads what is there; `open` is false when the peer's end is closed.
    void serviceUpload(Peer *peer, bool open)
    {
        UploadState &up = *peer->upload;
        const std::string buffered = peer->conn->takeBuffered();
        if (!buffered.empty()) {
            peer->lastReceived = Clock::now();
            uploadBytes(peer, buffered.data(), buffered.size());
        }
        char chunk[65536];
        while (open && !peer->conn->closing()) {
            const unsigned long long wanted = up.file ? up.size - up.file->received() : up.drain;
            const long n = peer->conn->readRaw(chunk, static_cast<std::size_t>(std::min<unsigned long long>(sizeof chunk, wanted)));
            if (n == Stream::kWouldBlock)
                break;
            if (n <= 0) {
                open = false;
                break;
            }
            peer->lastReceived = Clock::now();
            uploadBytes(peer, chunk, static_cast<std::size_t>(n));
        }
        if (!open && !peer->conn->closing())
            uploadEnded(peer, "short"); // the connection ended before the last byte
    }

    // An upload that cannot go on: the answer is tried once, the connection goes.
    void uploadEnded(Peer *peer, const std::string &reason)
    {
        if (peer->upload->file)
            uploadFail(peer, reason, 0);
        peer->conn->flush();
        dead_.push_back(peer);
    }

    // ---- Downloads (PROTOCOL.md, "Files"): one file of a proposal ----

    // Moves the file to the socket as far as the socket takes it, never more
    // than one chunk ahead of it: nothing but a chunk is in memory.
    void pumpDownload(Peer *peer)
    {
        DownloadState &down = *peer->download;
        constexpr std::size_t kChunk = 65536;
        for (;;) {
            while (down.remaining > 0 && peer->conn->pending() < kChunk) {
                char buffer[kChunk];
                if (down.fd < 0) { // an in-memory file (`fetch`)
                    const std::size_t take = std::min<std::size_t>(kChunk, static_cast<std::size_t>(down.remaining));
                    peer->conn->sendRaw(down.memory.substr(down.offset, take));
                    down.offset += take;
                    down.remaining -= take;
                    continue;
                }
                const ssize_t n = ::read(down.fd, buffer, static_cast<std::size_t>(std::min<unsigned long long>(sizeof buffer, down.remaining)));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) {
                    // The copy got shorter than said: the header is out, so only the end of the connection tells.
                    logLine("download: the file ended early");
                    dead_.push_back(peer);
                    return;
                }
                peer->conn->sendRaw(std::string(buffer, static_cast<std::size_t>(n)));
                down.remaining -= static_cast<unsigned long long>(n);
            }
            const std::size_t before = peer->conn->pending();
            if (!peer->conn->flush()) {
                dead_.push_back(peer);
                return;
            }
            if (peer->conn->pending() < before)
                down.progress = Clock::now();
            if (peer->conn->pending() > 0)
                return; // the socket is full: POLLOUT brings us back
            if (down.remaining == 0) {
                peer->conn->closeAfterFlush();
                return;
            }
        }
    }

    void onDownload(Peer *peer, const json &message)
    {
        peer->download = std::make_unique<DownloadState>();
        DownloadState &down = *peer->download;
        const json ref = message.value("ref", json());
        const bool goodRequest = message.contains("proposal") && message["proposal"].is_string() && message.contains("file")
            && message["file"].is_string();
        const json proposalId = goodRequest ? message["proposal"] : json();
        const json fileId = goodRequest ? message["file"] : json();
        auto fail = [&](const char *reason) {
            logLine(std::string("download rejected: ") + reason);
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"proposal", proposalId}, {"file", fileId}, {"reason", reason}});
            peer->conn->closeAfterFlush();
        };
        if (!goodRequest)
            return fail("bad-request");
        const auto proposal = std::find_if(proposals_.begin(), proposals_.end(),
                                           [&](const Proposal &p) { return p.id == proposalId.get<std::string>(); });
        if (proposal == proposals_.end())
            return fail("unknown-proposal");
        const auto file = std::find_if(proposal->files.begin(), proposal->files.end(),
                                       [&](const ProposalFile &f) { return f.id == fileId.get<std::string>(); });
        if (file == proposal->files.end())
            return fail("unknown-file");
        struct stat st;
        down.fd = ::open(file->path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (down.fd < 0 || ::fstat(down.fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<unsigned long long>(st.st_size) != file->size)
            return fail("read-failed");
        down.remaining = file->size;
        down.progress = Clock::now();
        logLine("download of " + proposal->id + "/" + file->id + ": " + std::to_string(file->size) + " bytes");
        peer->conn->send({{"type", "download"}, {"ref", ref}, {"proposal", proposal->id}, {"file", file->id},
                          {"name", file->name}, {"size", file->size}, {"sha256", file->sha256}});
        // The first chunk goes in service(), when the socket is known to take it.
    }

    // A read-only copy of one file a session worked with (PROTOCOL.md, "Files"):
    // the whole file is read first, then header and bytes go out like a download.
    void onFetch(Peer *peer, const json &message)
    {
        peer->download = std::make_unique<DownloadState>();
        DownloadState &down = *peer->download;
        const json ref = message.value("ref", json());
        const bool good = message.contains("session") && message["session"].is_string() && message.contains("path")
            && message["path"].is_string();
        const json sessionField = message.contains("session") ? message["session"] : json();
        const std::string path = good ? message["path"].get<std::string>() : "";
        std::string shown = good ? path : "?";
        auto fail = [&](const char *reason) {
            logLine("fetch of " + (sessionField.is_string() ? sessionField.get<std::string>() : std::string("?")) + " " + shown
                    + " rejected: " + reason);
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"session", sessionField}, {"reason", reason}});
            peer->conn->closeAfterFlush();
        };
        Session *target = sessionField.is_string() ? session(sessionField.get<std::string>()) : nullptr;
        if (sessionField.is_string() && !target)
            return fail("unknown-session");
        if (!good || path.empty() || path[0] != '/' || path.find('\0') != std::string::npos)
            return fail("bad-request");
        char buf[PATH_MAX];
        if (!::realpath(path.c_str(), buf))
            return fail("not-found");
        const std::string real = buf;
        struct stat st;
        if (::stat(real.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return fail("not-a-file");
        if (protectedPath(real))
            return fail("not-allowed");
        const std::string home = realOr(homeDir());
        const std::string cwd = realOr(target->cwd());
        const bool inCwd = !target->cwd().empty() && cwd != home && underDir(real, cwd);
        const std::string inbox = inboxDir(target->id());
        bool allowed = inCwd || (!inbox.empty() && underDir(real, realOr(inbox)));
        if (!allowed)
            allowed = jsonHasPath(target->history(1000000), path, real);
        if (!allowed)
            return fail("not-allowed");
        if (static_cast<unsigned long long>(st.st_size) > kMaxUploadFile)
            return fail("too-large");
        const int fd = ::open(real.c_str(), O_RDONLY | O_CLOEXEC);
        std::string bytes;
        bool ok = fd >= 0;
        if (ok) {
            char chunk[65536];
            for (;;) {
                const ssize_t n = ::read(fd, chunk, sizeof chunk);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0)
                    ok = false;
                if (n <= 0)
                    break;
                bytes.append(chunk, static_cast<std::size_t>(n));
                if (bytes.size() > kMaxUploadFile) {
                    ok = false;
                    break;
                }
            }
            ::close(fd);
        }
        if (!ok)
            return fail("read-failed");
        down.memory = std::move(bytes);
        down.remaining = down.memory.size();
        down.progress = Clock::now();
        logLine("fetch of " + target->id() + " " + real + ": " + std::to_string(down.memory.size()) + " bytes");
        peer->conn->send({{"type", "fetch"}, {"ref", ref}, {"session", target->id()}, {"path", real},
                          {"name", fs::path(real).filename().string()}, {"size", down.memory.size()},
                          {"sha256", bytesSha256(down.memory)}, {"mtime", static_cast<long long>(st.st_mtime)}});
    }

    void onUpload(Peer *peer, const json &message)
    {
        peer->upload = std::make_unique<UploadState>();
        UploadState &up = *peer->upload;
        up.ref = message.value("ref", json());
        up.session = message.contains("session") && message["session"].is_string() ? message["session"].get<std::string>() : "";
        const std::string name = message.contains("name") && message["name"].is_string() ? message["name"].get<std::string>() : "";
        std::string sha = message.contains("sha256") && message["sha256"].is_string() ? message["sha256"].get<std::string>() : "";
        for (char &c : sha)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool sized = message.contains("size") && message["size"].is_number_integer() && message["size"].get<long long>() >= 0;
        if (sized)
            up.size = message["size"].get<unsigned long long>();
        if (!sized || sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos)
            return uploadFail(peer, "bad-request", 0);
        const unsigned long long drain = up.size;
        if (!session(up.session))
            return uploadFail(peer, "unknown-session", drain);
        if (up.size > kMaxUploadFile)
            return uploadFail(peer, "too-large", drain);
        up.file = std::make_unique<InboxUpload>(up.session, name, up.size, sha);
        if (const std::string reason = up.file->open(); !reason.empty())
            return uploadFail(peer, reason, drain);
        if (up.size == 0)
            uploadDone(peer);
    }

    // The hourly pass (and one at the start): old files, empty directories and
    // the directories of sessions that are gone.
    void sweepInbox()
    {
        if (Clock::now() < nextSweep_)
            return;
        nextSweep_ = Clock::now() + inboxSweepEvery();
        std::set<std::string> live;
        for (Session *s : sessions())
            live.insert(s->id());
        cleanInbox(&live, kInboxLife);
        std::set<std::string> open;
        for (const auto &p : proposals_)
            open.insert(p.id);
        cleanOutbox(open);
    }

    void reap()
    {
        for (auto &peer : peers_)
            if (peer->conn->wantsWrite() && !peer->conn->flush())
                dead_.push_back(peer.get());
        if (dead_.empty())
            return;
        for (Peer *peer : dead_) {
            // A `pvai pair` that gave up closes its window.
            if (window_ && window_->waiter && window_->waiter == peer->serial) {
                window_.reset();
                logLine("pairing window closed");
            }
            // The session holds the wrapper's connection: it goes first.
            if (peer->kind == Kind::Wrapper) {
                if (const auto found = sessions_.find(peer->sessionId); found != sessions_.end()) {
                    const std::optional<StartRequest> again = found->second->replacement();
                    const std::string gone = found->first;
                    sessionGone(*found->second);
                    if (!again) { // a replacement takes the session's id and its files
                        removeInbox(gone);
                        daemonEntries_.erase(gone);
                        sessionEndedForShares(gone);
                    }
                    sessions_.erase(found);
                    sessionsDirty_ = true;
                    if (again) // a restart that replaces the wrapper
                        claude_->start(*again, [id = again->id](const std::string &, const std::string &reason) {
                            logLine(id + (reason.empty() ? ": a new pvai run takes over" : ": no new pvai run: " + reason));
                        });
                }
            }
            if (peer->kind == Kind::Process)
                if (Backend *owner = backend(peer->assistant))
                    owner->processEnded(peer->serial);
        }
        std::erase_if(peers_, [&](const auto &p) { return std::find(dead_.begin(), dead_.end(), p.get()) != dead_.end(); });
        dead_.clear();
    }

    void sweepSessions()
    {
        if (std::erase_if(sessions_, [this](const auto &entry) {
                if (!entry.second->ended())
                    return false;
                sessionGone(*entry.second);
                removeInbox(entry.first);
                daemonEntries_.erase(entry.first);
                sessionEndedForShares(entry.first);
                return true;
            }))
            sessionsDirty_ = true;
    }

    // A session is over: the prompts it did not take are lost, and every
    // app hears which (with their whole text). Those of a Claude session
    // whose conversation is known are kept for 24 h (`kept: true`) for a
    // session that goes on with it; what the caps push out goes with a
    // second, plain `dropped`.
    void sessionGone(Session &session)
    {
        const std::vector<Prompt> prompts = session.unsent();
        if (prompts.empty())
            return;
        const std::string conversation = session.conversationId();
        std::vector<unsigned long long> mine;
        const auto now = Clock::now();
        if (!conversation.empty())
            for (const auto &p : prompts) {
                // The files went with the session's inbox: only its text is kept, and nothing when there is none.
                if (p.text.empty()) {
                    mine.push_back(0);
                    continue;
                }
                Prompt text = p;
                text.files.clear();
                kept_.push_back({conversation, session.cwd(), session.id(), std::move(text), now + kKeptLife, ++keptSeq_});
                mine.push_back(keptSeq_);
            }
        // What the caps push out, oldest first; of a prompt of this very
        // session, the first message already says it is not kept.
        json later = json::array();
        auto evict = [&](std::deque<KeptPrompt>::iterator it) {
            if (std::find(mine.begin(), mine.end(), it->seq) == mine.end())
                later.push_back({{"session", it->session}, {"ref", it->prompt.ref}, {"text", it->prompt.text}});
            return kept_.erase(it);
        };
        if (!conversation.empty()) {
            auto count = std::count_if(kept_.begin(), kept_.end(), [&](const auto &k) { return k.conversation == conversation; });
            for (auto it = kept_.begin(); count > static_cast<long>(kMaxKeptPerConversation) && it != kept_.end();) {
                if (it->conversation == conversation) {
                    it = evict(it);
                    --count;
                } else {
                    ++it;
                }
            }
        }
        while (kept_.size() > kMaxKept)
            evict(kept_.begin());
        json dropped = json::array();
        std::size_t keptCount = 0;
        for (std::size_t i = 0; i < prompts.size(); ++i) {
            json entry = {{"session", session.id()}, {"ref", prompts[i].ref}, {"text", prompts[i].text}};
            if (i < mine.size() && std::any_of(kept_.begin(), kept_.end(), [&](const auto &k) { return k.seq == mine[i]; })) {
                entry["kept"] = true;
                ++keptCount;
            }
            dropped.push_back(entry);
        }
        logLine(session.id() + ": ended with " + std::to_string(dropped.size()) + " prompt(s) not taken, " + std::to_string(keptCount)
                + " kept");
        broadcast({{"type", "dropped"}, {"prompts", dropped}});
        if (!later.empty())
            broadcast({{"type", "dropped"}, {"prompts", later}});
    }

    std::vector<Prompt> takeKept(const std::string &conversation, const std::string &cwd) override
    {
        std::vector<Prompt> out;
        for (auto it = kept_.begin(); it != kept_.end();) {
            if (it->conversation == conversation && it->cwd == cwd) {
                out.push_back(it->prompt);
                it = kept_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    // Kept prompts that go for good: a plain `dropped`. `all` for the daemon
    // stopping, else the expired ones, or those of one forgotten conversation.
    void dropKept(const std::function<bool(const KeptPrompt &)> &which)
    {
        json dropped = json::array();
        for (auto it = kept_.begin(); it != kept_.end();) {
            if (which(*it)) {
                dropped.push_back({{"session", it->session}, {"ref", it->prompt.ref}, {"text", it->prompt.text}});
                it = kept_.erase(it);
            } else {
                ++it;
            }
        }
        if (!dropped.empty()) {
            logLine(std::to_string(dropped.size()) + " kept prompt(s) dropped for good");
            broadcast({{"type", "dropped"}, {"prompts", dropped}});
        }
    }

    void expireKept()
    {
        const auto now = Clock::now();
        if (!kept_.empty() && kept_.front().expires <= now)
            dropKept([&](const KeptPrompt &k) { return k.expires <= now; });
    }

    void serviceJobs(const std::vector<pid_t> &readable)
    {
        const auto now = Clock::now();
        std::vector<std::pair<std::function<void(int, const std::string &)>, std::pair<int, std::string>>> finished;
        for (auto &job : jobs_) {
            if (!job.ended) {
                bool eof = false;
                if (std::find(readable.begin(), readable.end(), job.pid) != readable.end())
                    eof = readAvailable(job.fd, job.output, kMaxJobOutput);
                if (eof) {
                    job.ended = true;
                    job.deadline = now + kJobKillAfter;
                } else if (now >= job.deadline) {
                    ::kill(-job.pid, SIGKILL);
                    job.ended = job.timedOut = job.killed = true;
                    job.deadline = now + kJobKillAfter;
                } else {
                    continue;
                }
            }
            int status = 0;
            const pid_t got = ::waitpid(job.pid, &status, WNOHANG);
            if (got == 0) {
                if (now >= job.deadline && !job.killed) {
                    ::kill(-job.pid, SIGKILL);
                    job.killed = true;
                }
                continue; // looked at again in 20 ms
            }
            status = job.timedOut || got < 0 || !WIFEXITED(status) ? -1 : WEXITSTATUS(status);
            ::close(job.fd);
            job.fd = -1;
            finished.push_back({std::move(job.done), {status, std::move(job.output)}});
        }
        std::erase_if(jobs_, [](const Job &job) { return job.fd < 0; });
        // Last: a callback may start the next job.
        for (auto &[done, result] : finished)
            done(result.first, result.second);
    }

    ClaudeSession *claudeSession(const std::string &id) { return dynamic_cast<ClaudeSession *>(session(id)); }

    Backend *backend(const std::string &name)
    {
        for (auto &backend : backends_)
            if (backend->name() == name)
                return backend.get();
        return nullptr;
    }

    void refreshVersions()
    {
        versionsRead_ = Clock::now();
        for (auto &backend : backends_)
            if (backend->installedHere()) {
                backend->refreshVersion();
                backend->refreshLogin([this](bool changed) {
                    if (changed)
                        broadcast(assistantsMessage());
                });
            }
    }

    // ---- Install and login from the app (setup.h) ----

    void onInstall(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        Backend *chosen = backend(message.value("assistant", ""));
        std::string reason = !chosen ? "unknown-assistant" : chosen->installedHere() ? "already-installed" : "";
        if (reason.empty())
            reason = setup_->install(chosen->name(), ref);
        if (!reason.empty())
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
        else
            logLine("install of " + chosen->name() + " started");
    }

    void onLogin(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string type = message.value("type", "");
        Backend *chosen = backend(message.value("assistant", ""));
        std::string reason = !chosen ? "unknown-assistant" : "";
        if (reason.empty() && type == "login")
            reason = !chosen->installedHere() ? "not-installed"
                     : chosen->ownAuth()      ? (setup_->busy(chosen->name()) ? "busy" : chosen->login(ref, message.value("method", "")))
                                              : setup_->login(chosen->name(), chosen->program(), ref, message.value("method", ""));
        else if (reason.empty() && type == "login-code")
            reason = setup_->loginCode(chosen->name(), message.value("code", ""));
        else if (reason.empty())
            reason = chosen->ownAuth() ? chosen->loginCancel() : setup_->loginCancel(chosen->name());
        if (!reason.empty())
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
        else if (type == "login")
            logLine("login of " + chosen->name() + (message.value("method", "") == "token" ? " (token)" : "") + " started");
    }

    // `set-token`: the token goes to its file and nowhere else. Claude Code
    // is not started to check it (on macOS that alone can prompt for the
    // Keychain); the only check is local. No log line, reply or event holds it.
    void onSetToken(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        Backend *chosen = backend(message.value("assistant", ""));
        const json &value = message.contains("token") ? message["token"] : json();
        std::string reason = !chosen ? "unknown-assistant" : chosen->name() != "claude" ? "not-supported" : "";
        if (reason.empty() && setup_->busy(chosen->name()))
            reason = "busy";
        if (reason.empty() && (!value.is_string() || !claudeTokenProblem(value.get<std::string>()).empty()))
            reason = "bad-token";
        if (reason.empty() && !storeClaudeToken(value.get<std::string>()))
            reason = "cannot-store-token";
        if (!reason.empty()) {
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
            return;
        }
        logLine("token of " + chosen->name() + " stored");
        rearmLogin(chosen->name());
        peer->conn->send({{"type", "token-set"}, {"ref", ref}, {"assistant", chosen->name()}});
        // On standby nothing runs; with the account login missing the token
        // becomes the fallback and the login is read with it.
        claude_->tokenStored([this] { broadcast(assistantsMessage()); });
    }

    void onLogout(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        Backend *chosen = backend(message.value("assistant", ""));
        std::string reason = !chosen ? "unknown-assistant" : !chosen->installedHere() ? "not-installed" : "";
        if (reason.empty() && chosen->name() == "kimi")
            reason = "not-supported";
        else if (reason.empty() && !setup_->busy(chosen->name()) && !chosen->authBusy() && chosen->loggedIn() == json(false)
                 && !(chosen->name() == "claude" && !readClaudeToken().empty()))
            reason = "not-logged-in";
        // A Claude logout drops the stored token, the marker and the decision
        // too, whatever the logout command says.
        if (reason.empty() && chosen->name() == "claude" && !setup_->busy("claude"))
            claude_->dropToken();
        if (reason.empty())
            reason = chosen->ownAuth() ? (setup_->busy(chosen->name()) ? "busy" : chosen->logout(ref))
                                       : setup_->logout(chosen->name(), chosen->program(), ref);
        if (!reason.empty())
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
        else
            logLine("logout of " + chosen->name() + " started");
    }

    // An install, login or logout ended: the program, its version and its login
    // are read again, and every app hears the new state.
    void setupFinished(const std::string &name, const std::string &kind, bool ok)
    {
        const bool install = kind == "install";
        logLine(kind + " of " + name + (ok ? " done" : " failed"));
        Backend *chosen = backend(name);
        if (!chosen)
            return;
        if (install)
            chosen->findProgram();
        if (kind == "login" && ok)
            if (auto *kimi = dynamic_cast<KimiBackend *>(chosen))
                kimi->setLoggedIn(true);
        // A login went through: the next login problem is news again.
        if (kind == "login" && ok)
            rearmLogin(name);
        broadcast(assistantsMessage());
        chosen->refreshVersion([this] { broadcast(assistantsMessage()); });
        chosen->refreshLogin([this](bool changed) {
            if (changed)
                broadcast(assistantsMessage());
        });
    }

    void rearmLogin(const std::string &assistant)
    {
        for (Session *s : sessions())
            if (s->assistant() == assistant)
                s->rearmLogin();
    }

    json assistantsMessage() const
    {
        json message = welcomeMessage();
        return {{"type", "assistants"}, {"assistants", message["assistants"]}, {"installed", message["installed"]}};
    }

    json welcomeMessage() const
    {
        json assistants = json::array(), installed = json::object();
        for (const auto &backend : backends_) {
            assistants.push_back(backend->describe());
            if (!backend->installed().empty())
                installed[backend->name()] = backend->installed();
        }
        return {{"type", "welcome"}, {"host", hostName()}, {"user", userName()}, {"version", kVersion},
                {"assistants", assistants}, {"installed", installed}, {"conversations", true},
                {"peers", true}, {"forget", true}, {"model", true}, {"effort", true}, {"logout", true}, {"rename", true}, {"tokenLogin", true},
                {"upload", {{"maxFile", kMaxUploadFile}, {"maxSession", kMaxUploadSession},
                            {"maxProposal", proposalLimits().proposal}, {"maxOutbox", proposalLimits().outbox}}},
                {"proposalFiles", true}, {"fetch", {{"maxFile", kMaxUploadFile}}}};
    }

    // What this peer is told as `welcome`: an owner's feature flag for sharing,
    // or a guest's rights (and none of the owner's accounts or other features).
    json welcomeFor(const Peer &peer)
    {
        json welcome = welcomeMessage();
        if (!peer.guest) {
            welcome["share"] = true;
            return welcome;
        }
        for (json &assistant : welcome["assistants"])
            assistant["account"] = nullptr;
        for (const char *key : {"conversations", "peers", "forget", "model", "effort", "logout", "rename", "tokenLogin", "proposalFiles"})
            welcome[key] = false;
        if (const Device *device = guestDevice(peer)) {
            const Guest &g = *device->guest;
            welcome["guest"] = {{"session", g.session}, {"assistant", g.assistant}, {"rights", guestRights(g.dialogs)},
                                {"dialogs", g.dialogs}, {"expires", g.expires > 0 ? json(g.expires) : json()}, {"state", g.state}};
        }
        return welcome;
    }

    void handle(Peer *peer, const json &message)
    {
        // In a pairing window every line that is no well-formed `pair` is a
        // bad proof (the window closes), also one without a type.
        if (!peer->pairing.empty()) {
            onPair(peer, message);
            return;
        }
        const std::string type = message.is_object() ? message.value("type", "") : "";
        if (type.empty()) {
            peer->conn->send({{"type", "error"}, {"code", "bad-message"}, {"text", "not a JSON object with a type"}});
            return;
        }
        if (peer->kind == Kind::Unknown) {
            if (!peer->tcp && type == "endpoint") {
                // Where apps can reach this daemon; null: not at all.
                peer->conn->send({{"type", "endpoint"},
                                  {"socket", appSocket_.empty() ? json(nullptr) : json(appSocket_)},
                                  {"port", tcpFd_ >= 0 ? json(port_) : json(nullptr)}});
                peer->conn->closeAfterFlush();
                return;
            }
            if (!peer->tcp && (type == "pair-open" || type == "revoke" || type == "listen")) {
                if (type == "pair-open")
                    onPairOpen(peer, message);
                else if (type == "revoke")
                    onRevoke(peer, message);
                else {
                    loadListen();
                    peer->conn->send({{"type", "listening"}, {"spec", listenSpec_}, {"ok", direct_.fd >= 0 || listenSpec_.empty()},
                                      {"error", listenSpec_.empty() || direct_.fd >= 0 ? "" : listenError_},
                                      {"fingerprint", tls_ ? tls_->fingerprint() : ""}});
                    peer->conn->closeAfterFlush();
                }
                return;
            }
            if (!peer->tcp && (type == "share" || type == "shares" || type == "share-revoke" || type == "share-confirm" || type == "share-move")) {
                onShareRequest(peer, type, message);
                return;
            }
            if (!peer->tcp && type == "register") {
                registerWrapper(peer, message);
                return;
            }
            if (!peer->tcp && type == "attach") {
                if (Backend *owner = backend(message.value("assistant", ""))) {
                    peer->kind = Kind::Process;
                    peer->assistant = owner->name();
                    owner->processStarted(peer->serial, message);
                } else {
                    peer->conn->closeAfterFlush();
                }
                return;
            }
            if (!peer->tcp && (type == "hook" || type == "status")) {
                const char *key = type == "hook" ? "event" : "status";
                if (auto *session = claudeSession(message.value("id", "")); session && message.contains(key) && message[key].is_object()) {
                    if (type == "hook")
                        session->onHook(message[key]);
                    else
                        session->onStatus(message[key]);
                }
                peer->conn->closeAfterFlush();
                return;
            }
            if (type == "hello") {
                // Over direct access the client certificate is the login.
                if (peer->tcp && peer->device.empty() && message.value("token", "") != token_) {
                    peer->conn->send({{"type", "error"}, {"code", "auth"}, {"text", "bad token"}});
                    peer->conn->closeAfterFlush();
                    return;
                }
                peer->kind = Kind::App;
                peer->client = message.value("client", "");
                peer->conn->send(welcomeFor(*peer));
                if (peer->guest) {
                    const Device *device = guestDevice(*peer);
                    const auto sessionsView = guestView(*peer, sessionsMessage());
                    peer->conn->send(sessionsView ? *sessionsView : json({{"type", "sessions"}, {"sessions", json::array()}}));
                    peer->conn->send({{"type", "proposals"}, {"proposals", json::array()}});
                    if (device && expiredShare(*device->guest)) {
                        peer->conn->send({{"type", "share-state"}, {"state", "expired"}, {"session", device->guest->session}});
                        peer->conn->closeAfterFlush();
                    } else if (device && device->guest->state != "active") {
                        peer->conn->send({{"type", "share-state"}, {"state", device->guest->state}, {"session", device->guest->session}});
                    }
                    return;
                }
                peer->conn->send(sessionsMessage());
                peer->conn->send(proposalsMessage());
                if (Clock::now() - versionsRead_ > kVersionAge)
                    refreshVersions();
                return;
            }
            peer->conn->send({{"type", "error"}, {"code", "auth"}, {"text", "hello first"}});
            peer->conn->closeAfterFlush();
            return;
        }
        if (peer->kind == Kind::Process)
            return; // it only has to stay connected
        if (peer->kind == Kind::Wrapper) {
            ClaudeSession *session = claudeSession(peer->sessionId);
            if (!session)
                return;
            if (type == "input")
                session->onInput(message.value("escape", false), message.value("key", ""));
            else if (type == "draft")
                session->onDraft(message.value("dirty", false));
            else if (type == "dialog")
                session->onDialog(message.value("kind", ""));
            else if (type == "exited")
                session->onExited(message.value("status", 0), str(message, "screen"));
            else if (type == "screen")
                session->onScreen(str(message, "dialog"),
                                  message.value("prompt", false),
                                  str(message, "mode"), message.value("login", false),
                                  message.contains("working") && message["working"].is_boolean() ? std::optional<bool>(message["working"].get<bool>())
                                                                                              : std::nullopt);
            else if (type == "typed")
                session->onTyped(message.value("ok", false), str(message, "screen"), str(message, "how"));
            else if (type == "submitted")
                session->onSubmitted(message.value("ok", false), str(message, "why"), str(message, "screen"));
            else if (type == "snapshot")
                session->onSnapshot(str(message, "screen"));
            return;
        }
        if (peer->guest && !guestAllows(peer, type, message))
            return;
        if (type == "ping")
            peer->conn->send({{"type", "pong"}});
        else if (type == "share" || type == "shares" || type == "share-revoke" || type == "share-confirm" || type == "share-move")
            onShareRequest(peer, type, message);
        else if (type == "subscribe")
            onSubscribe(peer, message);
        else if (type == "start")
            onStart(peer, message);
        else if (type == "conversations")
            onConversations(peer, message);
        else if (type == "forget")
            onForget(peer, message);
        else if (type == "install")
            onInstall(peer, message);
        else if (const SessionCommand *command = sessionCommand(type))
            onSessionCommand(peer, message, *command);
        else if (type == "login" || type == "login-code" || type == "login-cancel")
            onLogin(peer, message);
        else if (type == "logout")
            onLogout(peer, message);
        else if (type == "set-token")
            onSetToken(peer, message);
        else if (type == "update")
            onUpdate(peer, message);
        else if (type == "projects")
            peer->conn->send({{"type", "projects"}, {"dirs", knownProjects()}});
        else if (type == "browse")
            peer->conn->send(browse(message.value("path", "")));
        else if (type == "unsubscribe")
            peer->subscriptions.erase(message.value("session", ""));
        else if (type == "prompt")
            onPrompt(peer, message);
        else if (type == "upload")
            onUpload(peer, message);
        else if (type == "download")
            onDownload(peer, message);
        else if (type == "fetch")
            onFetch(peer, message);
        else if (type == "dismiss")
            onDismiss(peer, message);
        else if (type == "propose" && !peer->ssl)
            onPropose(peer, message);
        else if (type == "peers")
            onPeers(message);
        else if (type == "listPeers" && !peer->ssl)
            peer->conn->send(peerListMessage());
        else if (type == "shutdown" && !peer->ssl)
            onShutdown(peer, message);
    }

    void registerWrapper(Peer *peer, const json &message)
    {
        const std::string id = message.value("id", "");
        if (id.empty() || sessions_.count(id)) {
            peer->conn->closeAfterFlush();
            return;
        }
        peer->kind = Kind::Wrapper;
        peer->sessionId = id;
        addSession(claude_->attach(*peer->conn, message));
    }

    // Starts a session in a directory, without a terminal; a new
    // conversation or, with `resume`, an ended one.
    void onStart(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        StartRequest request;
        request.cwd = message.value("cwd", "");
        request.label = message.value("label", "");
        request.resume = message.value("resume", "");
        if (message.contains("args") && message["args"].is_array())
            for (const auto &arg : message["args"])
                if (arg.is_string())
                    request.args.push_back(arg.get<std::string>());
        struct stat st;
        if (request.cwd.empty() || request.cwd[0] != '/' || ::stat(request.cwd.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", "bad-cwd"}});
            return;
        }
        Backend *chosen = backend(message.value("assistant", "claude"));
        if (!chosen) {
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", "unknown-assistant"}});
            return;
        }
        if (!chosen->installedHere()) {
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", "not-installed"}});
            return;
        }
        const auto serial = peer->serial;
        chosen->start(request, [this, serial, ref](const std::string &id, const std::string &reason) {
            Peer *peer = peerBySerial(serial);
            if (!peer)
                return;
            if (!reason.empty())
                peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
            else
                peer->conn->send({{"type", "started"}, {"ref", ref}, {"session", id}});
        });
    }

    void onConversations(Peer *peer, const json &message)
    {
        const std::string assistant = message.value("assistant", "claude");
        const std::string cwd = message.value("cwd", "");
        const auto serial = peer->serial;
        auto answer = [this, serial, assistant, cwd](const std::vector<Conversation> &found) {
            json list = json::array();
            for (const auto &c : found)
                list.push_back({{"id", c.id}, {"title", c.title}, {"at", c.at}});
            if (Peer *peer = peerBySerial(serial))
                peer->conn->send({{"type", "conversations"}, {"assistant", assistant}, {"cwd", cwd}, {"conversations", list}});
        };
        Backend *chosen = backend(assistant);
        if (!chosen || cwd.empty() || cwd[0] != '/')
            return answer({});
        chosen->conversations(cwd, answer);
    }

    void onForget(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string assistant = message.value("assistant", "claude");
        const std::string cwd = message.value("cwd", "");
        const std::string id = message.value("id", "");
        const auto serial = peer->serial;
        auto answer = [this, serial, ref, assistant, cwd, id](const std::string &reason) {
            Peer *peer = peerBySerial(serial);
            if (!peer)
                return;
            if (reason.empty())
                forgetConversation(id);
            if (reason.empty())
                forgetLabel(assistant, id);
            if (reason.empty() && assistant == "claude")
                dropKept([&](const KeptPrompt &k) { return k.conversation == id && k.cwd == cwd; });
            if (reason.empty())
                peer->conn->send({{"type", "forgotten"}, {"ref", ref}, {"assistant", assistant}, {"cwd", cwd}, {"id", id}});
            else
                peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"assistant", assistant}, {"cwd", cwd}, {"id", id}, {"reason", reason}});
        };
        Backend *chosen = backend(assistant);
        if (!chosen)
            return answer("unknown-assistant");
        if (cwd.empty() || cwd[0] != '/')
            return answer("bad-cwd");
        chosen->forget(cwd, id, answer);
    }

    // Updates every assistant installed here; running sessions keep the
    // version they started with.
    void onUpdate(Peer *peer, const json &message)
    {
        struct Pending {
            std::size_t left = 0;
            json changed = json::object(), errors = json::object();
        };
        auto pending = std::make_shared<Pending>();
        std::vector<Backend *> chosen;
        for (auto &backend : backends_)
            if (backend->available())
                chosen.push_back(backend.get());
        const auto serial = peer->serial;
        const json ref = message.value("ref", json());
        auto finish = [this, serial, ref, pending] {
            versionsRead_ = Clock::now();
            if (Peer *peer = peerBySerial(serial))
                peer->conn->send({{"type", "updated"}, {"ref", ref}, {"installed", welcomeMessage()["installed"]},
                                  {"changed", pending->changed}, {"errors", pending->errors}});
        };
        pending->left = chosen.size();
        if (chosen.empty())
            return finish();
        for (Backend *backend : chosen) {
            const std::string name = backend->name();
            backend->update([pending, name, finish](const Backend::UpdateResult &result) {
                if (!result.error.empty())
                    pending->errors[name] = result.error;
                else if (result.before != result.after)
                    pending->changed[name] = {result.before, result.after};
                if (--pending->left == 0)
                    finish();
            });
        }
    }

    // The directories Claude Code has run in on this host, most recent
    // first, for the app's "new session" choice: the `cwd` its transcripts
    // under ~/.claude/projects record (the folder names there are lossy).
    static json knownProjects()
    {
        const std::filesystem::path root = std::filesystem::path(homeDir()) / ".claude" / "projects";
        std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
        std::error_code ec;
        // The newest transcript of a folder is found by a scan every time
        // (a resumed old conversation must move its project up). Only the
        // cwd is cached: it sits at the head of the file and stays while
        // the file grows; a smaller file is a replaced one.
        struct Known {
            std::uintmax_t size = 0;
            std::string cwd;
        };
        static std::map<std::string, Known> cache;
        std::set<std::string> present;
        for (const auto &folder : std::filesystem::directory_iterator(root, ec)) {
            std::filesystem::path newest;
            std::filesystem::file_time_type newestTime{};
            for (const auto &file : std::filesystem::directory_iterator(folder.path(), ec)) {
                if (file.path().extension() != ".jsonl")
                    continue;
                const auto t = file.last_write_time(ec);
                if (newest.empty() || t > newestTime) {
                    newest = file.path();
                    newestTime = t;
                }
            }
            if (newest.empty())
                continue;
            const std::string key = newest.string();
            present.insert(key);
            const std::uintmax_t size = std::filesystem::file_size(newest, ec);
            Known &known = cache[key];
            if (known.cwd.empty() || size < known.size) {
                known = {size, ""};
                std::ifstream in(newest);
                std::string line;
                for (int i = 0; i < 50 && std::getline(in, line); ++i) {
                    if (line.find("\"cwd\"") == std::string::npos)
                        continue;
                    const json entry = json::parse(line, nullptr, false);
                    known.cwd = entry.is_object() ? entry.value("cwd", "") : "";
                    if (!known.cwd.empty())
                        break;
                }
            }
            known.size = size;
            if (!known.cwd.empty() && std::filesystem::is_directory(known.cwd, ec))
                found.emplace_back(newestTime, known.cwd);
        }
        for (auto it = cache.begin(); it != cache.end();)
            it = present.count(it->first) ? std::next(it) : cache.erase(it);
        std::sort(found.begin(), found.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
        json dirs = json::array();
        std::set<std::string> seen;
        for (const auto &[time, dir] : found)
            if (seen.insert(dir).second && dirs.size() < 100)
                dirs.push_back(dir);
        return dirs;
    }

    // The subdirectories of a directory (hidden ones left out), to pick
    // where a new session starts.
    static json browse(const std::string &path)
    {
        json dirs = json::array();
        std::error_code ec;
        std::vector<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator(path, ec))
            if (entry.is_directory(ec) && entry.path().filename().string()[0] != '.')
                names.push_back(entry.path().filename().string());
        std::sort(names.begin(), names.end());
        for (const auto &name : names)
            dirs.push_back(name);
        return {{"type", "browse"}, {"path", path}, {"dirs", dirs}, {"ok", !ec}};
    }

    void reject(Peer *peer, const json &ref, const std::string &id, const std::string &reason)
    {
        peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"session", id}, {"reason", reason}});
    }

    // What a client asks of one session: `run` answers a reason to reject,
    // empty when it went through; then `okType` goes back (none: the session
    // answers itself).
    struct SessionCommand {
        const char *type;
        const char *okType;
        std::function<std::string(Session &, const json &message, const json &ref)> run;
    };

    static const SessionCommand *sessionCommand(const std::string &type)
    {
        for (const auto &command : sessionCommands())
            if (type == command.type)
                return &command;
        return nullptr;
    }

    static const std::vector<SessionCommand> &sessionCommands()
    {
        static const std::vector<SessionCommand> commands = {
            {"mode", nullptr, [](Session &s, const json &m, const json &ref) { return s.setMode(m.value("mode", ""), ref); }},
            {"model", nullptr,
             [](Session &s, const json &m, const json &ref) {
                 const std::string model = m.value("model", "");
                 // A model's name or alias: letters, digits, . - _ [ ] and no more than 80.
                 if (model.empty() || model.size() > 80
                     || model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_[]:/") != std::string::npos)
                     return std::string("bad-model");
                 return s.setModel(model, ref);
             }},
            {"effort", nullptr,
             [](Session &s, const json &m, const json &ref) {
                 const std::string effort = m.value("effort", "");
                 const json levels = s.efforts();
                 if (levels.empty())
                     return std::string("not-supported");
                 if (std::find(levels.begin(), levels.end(), json(effort)) == levels.end())
                     return std::string("bad-effort");
                 return s.setEffort(effort, ref);
             }},
            {"answer", "answered", [](Session &s, const json &m, const json &) { return s.answer(m.value("choice", "")); }},
            {"stop", "stopping", [](Session &s, const json &, const json &) { return s.stop(); }},
            {"interrupt", "interrupting",
             [](Session &s, const json &, const json &) {
                 s.interrupt();
                 return std::string();
             }},
            {"rename", "renamed", [](Session &s, const json &m, const json &) { return s.rename(m.contains("label") && m["label"].is_string() ? m["label"].get<std::string>() : std::string()); }},
            {"restart", "restarting", [](Session &s, const json &, const json &) { return s.restart(); }},
            {"clearInput", "inputCleared", [](Session &s, const json &, const json &) { return s.clear(); }},
        };
        return commands;
    }

    void onSessionCommand(Peer *peer, const json &message, const SessionCommand &command)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        // A wrapper older than kMinWrapperVersion types prompts, no more.
        static const std::set<std::string> needsWrapper = {"answer", "clearInput", "mode", "model", "effort", "restart"};
        if (needsWrapper.count(command.type) && session->wrapperTooOld())
            return reject(peer, ref, id, "stale-wrapper");
        // Without a version the wrapper cannot show a /model's dialog: a model
        // change goes through a restart of the session instead.
        if ((std::string(command.type) == "model" || std::string(command.type) == "effort") && session->wrapperUnversioned())
            return reject(peer, ref, id, "stale-wrapper");
        if (const std::string reason = command.run(*session, message, ref); !reason.empty())
            return reject(peer, ref, id, reason);
        if (command.okType) {
            json ok = {{"type", command.okType}, {"ref", ref}, {"session", id}};
            if (std::string(command.type) == "rename")
                ok["label"] = session->label();
            peer->conn->send(ok);
        }
        if (std::string(command.type) == "answer")
            noteAnswer(peer, *session, message.value("choice", ""));
    }

    // ---- Renames: kept in ~/.config/pvai/labels.json ----

    // A Claude session's wrapper keeps its label itself (and registers with
    // it again); the file holds the sessions rebuilt from an assistant's own
    // state, by "<assistant>/<id>".
    static std::string labelKey(const Session &session) { return session.assistant() + "/" + session.id(); }

    std::string renameSession(Session &session, const std::string &raw) override
    {
        const std::string label = trim(raw);
        if (label.empty() || label.size() > 64
            || std::any_of(label.begin(), label.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
            return "bad-label";
        for (Session *other : sessions())
            if (other != &session && other->label() == label)
                return "taken";
        session.applyLabel(label);
        if (session.assistant() != "claude") {
            savedLabels_[labelKey(session)] = label;
            saveLabels();
        }
        return "";
    }

    std::string labelsPath() const { return configDir() + "/labels.json"; }

    void loadLabels()
    {
        std::ifstream in(labelsPath());
        const json file = json::parse(in, nullptr, false);
        savedLabels_ = file.is_object() ? file : json::object();
    }

    void saveLabels()
    {
        const std::string path = labelsPath(), temp = path + ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            out << savedLabels_.dump(1) << "\n";
        }
        ::chmod(temp.c_str(), 0600);
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
    }

    void forgetLabel(const std::string &assistant, const std::string &id)
    {
        if (savedLabels_.erase(assistant + "/" + id))
            saveLabels();
    }

    void onSubscribe(Peer *peer, const json &message)
    {
        const std::string id = message.value("session", "");
        Session *session = this->session(id);
        if (!session) {
            peer->conn->send({{"type", "error"}, {"code", "unknown-session"}, {"text", id}});
            return;
        }
        peer->subscriptions.insert(id);
        peer->conn->send({{"type", "history"}, {"session", id}, {"entries", historyView(*session, message.value("limit", 200))}});
    }

    void onPrompt(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        const std::string text = trim(message.value("text", ""));
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        // Files the app uploaded for this session, and only those.
        std::vector<std::string> files;
        if (message.contains("files") && !message["files"].is_null()) {
            const json &list = message["files"];
            if (!list.is_array() || list.size() > kMaxPromptFiles)
                return reject(peer, ref, id, "bad-file");
            for (const auto &entry : list) {
                const std::string resolved = entry.is_string() ? inboxFile(id, entry.get<std::string>()) : "";
                if (resolved.empty())
                    return reject(peer, ref, id, "bad-file");
                files.push_back(resolved);
            }
        }
        if (text.empty() && files.empty())
            return reject(peer, ref, id, "empty");
        if (text.size() > kMaxPrompt)
            return reject(peer, ref, id, "too-long");
        // A guest's slash and shell commands would do what its requests may not
        // (/exit, /model, /logout, ! anything): they are refused.
        if (peer->guest && !text.empty() && (text[0] == '/' || text[0] == '!')) {
            logLine("guest \"" + peer->deviceName + "\": prompt refused: guest-denied (a command)");
            return reject(peer, ref, id, "guest-denied");
        }
        // A message from a session to itself is no agent message: a proposal
        // whose target the app could not place has landed in its sender. A
        // guest's message is never an agent message, whatever it claims.
        if (const std::string from = oneLine(message.value("from", ""), 128); !from.empty() && !peer->guest) {
            if (from == hostName() + "/" + id || (!peersSelf_.empty() && from == peersSelf_ + "/" + id))
                return reject(peer, ref, id, "self-message");
        }
        const std::string header = peer->guest ? std::string() : fromLine(message);
        // With files and no text the header stands alone: the paths follow it, never lead.
        Prompt prompt{text.empty() ? trim(header) : header + text, message.value("voice", false), ref};
        prompt.agent = !header.empty();
        prompt.files = std::move(files);
        const std::string typedText = prompt.text;
        const std::vector<std::string> typedFiles = prompt.files;
        const bool typed = session->prompt(std::move(prompt));
        if (peer->guest)
            noteGuestPrompt(peer, *session, typedText, typedFiles);
        peer->conn->send({{"type", "accepted"}, {"ref", ref}, {"session", id},
                          {"delivery", typed && !session->confirmsDelivery() ? "typed" : "queued"}});
    }

    // An agent message the owner released: which one, who wrote, what it
    // answers and how to answer it, as its own first line.
    static std::string fromLine(const json &message)
    {
        const std::string from = oneLine(message.value("from", ""), 128);
        if (from.empty())
            return {};
        const std::string label = oneLine(message.value("fromLabel", ""), 64);
        const std::string id = message.value("proposal", json()).is_string() ? message["proposal"].get<std::string>() : "";
        const std::string replyTo = message.value("replyTo", json()).is_string() ? message["replyTo"].get<std::string>() : "";
        const bool known = goodProposalId(id);
        // A sender that is a host only (a thread without a session id) cannot
        // be written to: a reply would have no session to go to.
        const auto slash = from.find('/');
        if (slash == std::string::npos || slash + 1 == from.size())
            return "[Message " + (known ? id + " " : std::string()) + "from " + from + (label.empty() ? "" : " (" + label + ")")
                + (goodProposalId(replyTo) ? ", answering " + replyTo : "")
                + ", released by the owner. It cannot be answered: the sender is not a session.]\n";
        return "[Message " + (known ? id + " " : std::string()) + "from " + from + (label.empty() ? "" : " (" + label + ")")
            + (goodProposalId(replyTo) ? ", answering " + replyTo : "") + ", released by the owner. Answer with: pvai propose "
            + (known ? "--reply " + id + " " : std::string()) + from + " \"...\"]\n";
    }

    // A target's session given by label: the id when exactly one session of
    // that host has it (this host's own, or the app's `peers`); "" when
    // several do, so that a message does not land in the wrong one; the
    // target unchanged otherwise (an id, or one the app may know better).
    std::string resolveTarget(const std::string &to) const
    {
        const auto slash = to.find('/');
        if (slash == std::string::npos)
            return to;
        const std::string host = to.substr(0, slash), name = to.substr(slash + 1);
        const bool here = host == hostName() || (!peersSelf_.empty() && host == peersSelf_);
        std::vector<std::string> byLabel;
        if (here) {
            for (const Session *s : const_cast<Daemon *>(this)->sessions()) {
                if (s->id() == name)
                    return to;
                if (s->label() == name)
                    byLabel.push_back(s->id());
            }
        } else {
            for (const auto &other : otherHosts_) {
                if (str(other, "host") != host)
                    continue;
                if (str(other, "session") == name)
                    return to;
                if (str(other, "label") == name && !str(other, "session").empty())
                    byLabel.push_back(str(other, "session"));
            }
        }
        if (byLabel.size() == 1)
            return host + "/" + byLabel[0];
        return byLabel.empty() ? to : std::string();
    }

    // What the app's list says against a target (already resolved) on another
    // host: "host-offline", "unknown-session" or "". Nothing for this host's
    // own sessions, for a host not in the list or before any list came.
    std::string targetWarning(const std::string &to, long long &seenAt) const
    {
        seenAt = 0;
        const auto slash = to.find('/');
        const std::string host = to.substr(0, slash), name = slash == std::string::npos ? "" : to.substr(slash + 1);
        if (peersAt_ == 0 || host == hostName() || (!peersSelf_.empty() && host == peersSelf_))
            return "";
        bool listed = false, allOffline = true, found = false;
        for (const auto &other : otherHosts_) {
            if (str(other, "host") != host)
                continue;
            listed = true;
            if (!other.value("offline", false))
                allOffline = false;
            else if (other.contains("seenAt"))
                seenAt = std::max(seenAt, other["seenAt"].get<long long>());
            // A host listed as a whole: the app knows its sessions better.
            if (!name.empty() && (str(other, "session").empty() || str(other, "session") == name || str(other, "label") == name))
                found = true;
        }
        if (!listed)
            return "";
        if (allOffline)
            return "host-offline";
        return !name.empty() && !found ? "unknown-session" : "";
    }

    // ---- Agent messages: proposals wait for the owner in the app ----

    void onPropose(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        std::string to = message.value("to", "");
        const std::string replyTo = message.value("replyTo", "");
        const std::string text = trim(message.value("text", ""));
        const std::string from = oneLine(message.value("from", ""), 64);
        auto reject = [&](const char *reason) {
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
        };
        // `files`: absolute paths, checked here; the copies come after the other checks.
        std::vector<std::string> sources;
        if (message.contains("files") && !message["files"].is_null()) {
            const json &list = message["files"];
            if (!list.is_array() || list.size() > kMaxProposalFiles)
                return reject("bad-file");
            for (const auto &entry : list) {
                if (!entry.is_string())
                    return reject("bad-file");
                sources.push_back(entry.get<std::string>());
            }
        }
        if (text.empty() && sources.empty())
            return reject("empty");
        if (text.size() > kMaxPrompt)
            return reject("too-long");
        if (!goodTarget(to))
            return reject("bad-target");
        // A host alone names no session: the owner's Send would put it into
        // whichever tab the app has open, the sender's own among them.
        if (to.find('/') == std::string::npos)
            return reject("no-session");
        if (!replyTo.empty() && !goodProposalId(replyTo))
            return reject("bad-reply");
        to = resolveTarget(to);
        if (to.empty())
            return reject("ambiguous-target");
        const auto now = Clock::now();
        auto &recent = proposalTimes_[from];
        std::erase_if(recent, [&](const Clock::time_point &t) { return now - t >= std::chrono::minutes(1); });
        if (proposals_.size() >= kMaxProposals || recent.size() >= kProposalsPerMinute)
            return reject("too-many");
        const ProposalLimits &limits = proposalLimits();
        unsigned long long total = 0;
        for (const auto &source : sources) {
            unsigned long long size = 0;
            if (checkOutboxSource(source, size) != "")
                return reject("bad-file");
            if (size > limits.file)
                return reject("too-large");
            total += size;
        }
        if (total > limits.proposal)
            return reject("too-large");
        unsigned long long held = 0;
        for (const auto &other : proposals_)
            for (const auto &f : other.files)
                held += f.size;
        if (held + total > limits.outbox)
            return reject("quota");
        recent.push_back(now);
        Proposal proposal{"p" + randomHex(4), from, {}, to, text, replyTo, static_cast<long long>(std::time(nullptr)),
                          now + kProposalLife, {}};
        // The copies: a failure takes the whole proposal directory with it.
        unsigned long long copied = 0;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            OutboxFile copy;
            std::string reason = copyToOutbox(proposal.id, i + 1, sources[i], limits.file, copy);
            if (reason.empty() && (copied += copy.size) > limits.proposal)
                reason = "too-large";
            if (!reason.empty()) {
                removeOutbox(proposal.id);
                recent.pop_back();
                return reject(reason == "too-large" ? "too-large" : "bad-file");
            }
            proposal.files.push_back({"f" + std::to_string(i + 1), copy.name, copy.size, copy.sha256, sources[i], copy.path});
        }
        if (const Session *sender = session(from))
            proposal.fromLabel = sender->label();
        proposals_.push_back(proposal);
        int apps = 0;
        for (const auto &other : peers_)
            if (other->kind == Kind::App && !other->detached() && other->client != "pvai")
                ++apps;
        long long seenAt = 0;
        const std::string warning = targetWarning(to, seenAt);
        logLine("proposal " + proposal.id + " from " + (from.empty() ? "?" : from) + " to " + to
                + (replyTo.empty() ? "" : ", answering " + replyTo) + (warning.empty() ? "" : " (warning: " + warning + ")"));
        json proposed = {{"type", "proposed"}, {"ref", ref}, {"id", proposal.id}, {"to", to}, {"apps", apps}};
        if (!warning.empty())
            proposed["warning"] = warning;
        if (warning == "host-offline" && seenAt > 0)
            proposed["seenAt"] = seenAt;
        peer->conn->send(proposed);
        broadcast(proposalsMessage());
    }

    // ---- Stopping: `pvai daemon stop|restart` ----

    // Queued prompts live in memory only: with any queued the daemon does
    // not stop unless forced, and then it tells every app which went.
    void onShutdown(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        json waiting = json::array(), dropped = json::array();
        for (const Session *session : sessions()) {
            if (session->queued().empty())
                continue;
            waiting.push_back({{"session", session->id()}, {"label", session->label()}, {"queued", session->queued().size()}});
            for (const auto &p : session->queued())
                dropped.push_back({{"session", session->id()}, {"ref", p.ref}, {"text", p.text}});
        }
        if (!dropped.empty() && !message.value("force", false)) {
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", "queued"}, {"waiting", waiting}});
            return;
        }
        if (!dropped.empty()) {
            logLine("stopping with " + std::to_string(dropped.size()) + " queued prompt(s) dropped");
            broadcast({{"type", "dropped"}, {"prompts", dropped}});
        } else {
            logLine("stopping");
        }
        // Kept prompts do not outlive the daemon.
        dropKept([](const KeptPrompt &) { return true; });
        json wrappers = json::array();
        for (Session *session : sessions())
            if (const auto *claude = dynamic_cast<ClaudeSession *>(session))
                wrappers.push_back(claude->wrapperVersion());
        peer->conn->send({{"type", "shutting-down"}, {"ref", ref}, {"dropped", dropped}, {"wrappers", wrappers}});
        stopping_ = true;
    }

    // ---- Peers: the sessions on other hosts, as the app knows them ----

    // The app's whole list, replacing the last one; kept in memory only.
    void onPeers(const json &message)
    {
        otherHosts_ = json::array();
        const std::string self = oneLine(message.value("self", ""), 64);
        peersSelf_ = goodTarget(self) && self.find('/') == std::string::npos ? self : "";
        if (message.contains("peers") && message["peers"].is_array())
            for (const auto &entry : message["peers"]) {
                if (!entry.is_object() || otherHosts_.size() >= kMaxPeers)
                    continue;
                const std::string host = oneLine(entry.value("host", ""), 64);
                const std::string id = oneLine(entry.value("session", ""), 128);
                if (host.find('/') != std::string::npos || !goodTarget(host + "/" + (id.empty() ? "-" : id)))
                    continue;
                json other = {{"host", host}, {"session", id}};
                for (const auto &[key, max] : {std::pair{"label", 64}, {"assistant", 16}, {"state", 16}, {"cwd", 512}})
                    if (entry.contains(key) && entry[key].is_string())
                        other[key] = oneLine(entry[key].get<std::string>(), max);
                // What the app knows of reachability; a field of the wrong type is dropped.
                if (entry.contains("offline") && entry["offline"].is_boolean())
                    other["offline"] = entry["offline"].get<bool>();
                if (entry.contains("seenAt") && entry["seenAt"].is_number() && entry["seenAt"].get<double>() > 0)
                    other["seenAt"] = static_cast<long long>(entry["seenAt"].get<double>());
                otherHosts_.push_back(other);
            }
        peersAt_ = static_cast<long long>(std::time(nullptr));
    }

    json peerListMessage()
    {
        json local = json::array();
        for (const Session *session : sessions())
            local.push_back({{"session", session->id()}, {"label", session->label()}, {"assistant", session->assistant()},
                             {"state", session->state()}, {"cwd", session->cwd()}});
        return {{"type", "peerList"}, {"self", peersSelf_.empty() ? hostName() : peersSelf_}, {"local", local},
                {"peers", otherHosts_}, {"at", peersAt_ ? json(peersAt_) : json()}};
    }

    void onDismiss(Peer *peer, const json &message)
    {
        const std::string id = message.value("proposal", "");
        if (std::erase_if(proposals_, [&](const Proposal &p) { return p.id == id; })) {
            removeOutbox(id); // a download already streaming goes on from its open descriptor
            broadcast(proposalsMessage());
        }
        peer->conn->send({{"type", "dismissed"}, {"ref", message.value("ref", json())}, {"proposal", id}});
    }

    void expireProposals()
    {
        const auto now = Clock::now();
        const std::vector<Proposal> before = proposals_;
        if (std::erase_if(proposals_, [&](const Proposal &p) { return now >= p.expires; })) {
            for (const auto &p : before)
                if (now >= p.expires)
                    removeOutbox(p.id);
            broadcast(proposalsMessage());
        }
        std::erase_if(proposalTimes_, [&](const auto &entry) {
            return entry.second.empty() || now - entry.second.back() >= std::chrono::minutes(1);
        });
    }

    json proposalsMessage() const
    {
        json list = json::array();
        for (const auto &p : proposals_) {
            json files = json::array();
            for (const auto &f : p.files)
                files.push_back({{"id", f.id}, {"name", f.name}, {"size", f.size}, {"sha256", f.sha256}, {"source", f.source}});
            list.push_back({{"id", p.id}, {"from", p.from}, {"fromLabel", p.fromLabel}, {"to", p.to}, {"text", p.text},
                            {"replyTo", p.replyTo.empty() ? json() : json(p.replyTo)}, {"at", p.at}, {"files", files}});
        }
        return {{"type", "proposals"}, {"proposals", list}};
    }


    int nextTimeout() const
    {
        std::optional<Clock::time_point> next;
        auto take = [&](std::optional<Clock::time_point> at) {
            if (at && (!next || *at < *next))
                next = at;
        };
        for (const auto &[id, session] : sessions_)
            take(session->deadline());
        for (const auto &backend : backends_)
            take(backend->deadline());
        for (const auto &job : jobs_)
            take(job.ended ? Clock::now() + std::chrono::milliseconds(20) : job.deadline);
        take(setup_->deadline());
        take(directDeadline());
        take(shareDeadline());
        if (!kept_.empty())
            take(kept_.front().expires);
        take(nextSweep_);
        if (!next)
            return -1;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*next - Clock::now()).count();
        return static_cast<int>(std::max<long long>(0, ms));
    }

    Peer *peerBySerial(unsigned long long serial)
    {
        for (auto &peer : peers_)
            if (peer->serial == serial)
                return peer.get();
        return nullptr;
    }

    json sessionsMessage()
    {
        json list = json::array();
        for (Session *session : sessions())
            list.push_back(session->describe());
        return {{"type", "sessions"}, {"sessions", list}};
    }

    // ---- Direct access: TLS listener, devices, pairing (PROTOCOL.md) ----

    static void logLine(const std::string &text)
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        ::localtime_r(&now, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
        std::fprintf(stderr, "%s pvai daemon: %s\n", stamp, text.c_str());
        std::fflush(stderr);
    }

    static long long unixNow()
    {
        return static_cast<long long>(std::time(nullptr));
    }

    // ~/.config/pvai/listen: "ADDR:PORT" (IPv6 as [ADDR]:PORT), or absent.
    void loadListen()
    {
        std::string spec;
        std::ifstream in(configDir() + "/listen");
        std::getline(in, spec);
        spec = trim(spec);
        if (spec == listenSpec_ && (spec.empty() || direct_.fd >= 0))
            return;
        listenError_.clear();
        if (direct_.fd >= 0) {
            ::close(direct_.fd);
            direct_.fd = -1;
            logLine("direct access off" + (listenSpec_.empty() ? "" : " (was " + listenSpec_ + ")"));
        }
        listenSpec_ = spec;
        if (spec.empty())
            return;
        std::string address;
        int port = 0;
        if (!parseListen(spec, address, port)) {
            listenError_ = "cannot read the setting";
            logLine("direct access: cannot read listen setting \"" + spec + "\"");
            return;
        }
        if (!tls_) {
            tls_ = std::make_unique<TlsServer>([this](const std::string &print) { return admit(print); });
            if (!tls_->ready()) {
                listenError_ = "no TLS identity in " + configDir();
                logLine("direct access: no TLS identity in " + configDir());
                tls_.reset();
                return;
            }
        }
        direct_.fd = listenAny(address, port);
        if (direct_.fd < 0) {
            if (errno == EADDRINUSE) {
                const std::string shown = address.find(':') == std::string::npos ? address : "[" + address + "]";
                listenError_ = "port " + std::to_string(port) + " on " + address + " is in use (another user's daemon?); choose another port with pvai listen "
                               + shown + ":PORT";
                logLine("direct access: " + listenError_);
            } else {
                listenError_ = std::strerror(errno);
                logLine("direct access: cannot listen on " + spec + ": " + listenError_);
            }
            return;
        }
        setNonBlocking(direct_.fd);
        direct_.address = address;
        direct_.port = port;
        logLine("direct access on " + spec + ", fingerprint " + tls_->fingerprint());
    }

    static bool parseListen(const std::string &spec, std::string &address, int &port)
    {
        std::string portText;
        if (!spec.empty() && spec[0] == '[') {
            const auto close = spec.find(']');
            if (close == std::string::npos)
                return false;
            address = spec.substr(1, close - 1);
            if (close + 1 < spec.size()) {
                if (spec[close + 1] != ':')
                    return false;
                portText = spec.substr(close + 2);
            }
        } else {
            const auto colon = spec.rfind(':');
            address = colon == std::string::npos ? spec : spec.substr(0, colon);
            if (colon != std::string::npos)
                portText = spec.substr(colon + 1);
        }
        port = kDirectPort;
        if (!portText.empty()) {
            char *end = nullptr;
            const long value = std::strtol(portText.c_str(), &end, 10);
            if (*end || value < 1 || value > 65535)
                return false;
            port = static_cast<int>(value);
        }
        in6_addr probe6;
        in_addr probe4;
        return ::inet_pton(AF_INET, address.c_str(), &probe4) == 1 || ::inet_pton(AF_INET6, address.c_str(), &probe6) == 1;
    }

    static int listenAny(const std::string &address, int port)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
        addrinfo *found = nullptr;
        if (::getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found)
            return -1;
        const int fd = ::socket(found->ai_family, SOCK_STREAM, 0);
        if (fd >= 0) {
            setCloseOnExec(fd);
            const int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            // At boot the address may not be up yet; bind to it anyway.
#ifdef IP_FREEBIND
            if (found->ai_family == AF_INET)
                ::setsockopt(fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof one);
#endif
#ifdef IPV6_FREEBIND
            if (found->ai_family == AF_INET6)
                ::setsockopt(fd, IPPROTO_IPV6, IPV6_FREEBIND, &one, sizeof one);
#endif
            if (::bind(fd, found->ai_addr, found->ai_addrlen) != 0 || ::listen(fd, 16) != 0) {
                const int saved = errno;
                ::close(fd);
                ::freeaddrinfo(found);
                errno = saved;
                return -1;
            }
        }
        ::freeaddrinfo(found);
        return fd;
    }

    struct Admitted {
        std::string print;
        Device device;
        Clock::time_point at;
    };
    std::optional<Admitted> admitted_;

    // In the handshake: a paired device, or anyone while a window is open.
    bool admit(const std::string &print)
    {
        if (const auto device = Devices(devicesPath()).find(print)) {
            admitted_ = {print, *device, Clock::now()};
            return true;
        }
        return (window_ && Clock::now() < window_->until) || invitesOpen();
    }

    static std::string peerAddress(const sockaddr_storage &address)
    {
        char text[INET6_ADDRSTRLEN] = "?";
        if (address.ss_family == AF_INET)
            ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in &>(address).sin_addr, text, sizeof text);
        else if (address.ss_family == AF_INET6)
            ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 &>(address).sin6_addr, text, sizeof text);
        return text;
    }

    void acceptDirect()
    {
        sockaddr_storage from{};
        socklen_t length = sizeof from;
        const int fd = ::accept(direct_.fd, reinterpret_cast<sockaddr *>(&from), &length);
        if (fd < 0)
            return;
        const std::string address = peerAddress(from);
        const auto now = Clock::now();
        if (lockout_.blocked(address, now) || !tls_ || handshakes_.size() >= kMaxHandshakes) {
            ::close(fd);
            return;
        }
        keepAlive(fd);
        handshakes_.push_back({std::make_unique<TlsAccept>(tls_->context(), fd), address, now + kPreAuth});
    }

    // A refused direct access: the daemon's log, and one line for fail2ban
    // in the system log (ident pvai, facility auth):
    // "rejected <address>: <reason>", the reason one of a fixed few words,
    // never a fingerprint or anything the client sent.
    void directFailure(const std::string &address, const std::string &reason, const std::string &detail = {})
    {
        logLine(address + ": rejected: " + reason + (detail.empty() ? "" : " (" + detail + ")"));
        ::syslog(LOG_NOTICE, "rejected %s: %s", address.c_str(), reason.c_str());
        if (lockout_.fail(address, Clock::now())) {
            logLine(address + ": blocked after repeated failures");
            ::syslog(LOG_NOTICE, "blocked %s: repeated failures", address.c_str());
        }
    }

    void stepHandshake(Handshake &h)
    {
        switch (h.accept->step()) {
        case TlsAccept::Step::Again:
            return;
        case TlsAccept::Step::Failed: {
            // Our verify callback refused the certificate: not a paired
            // device (and no window open); none at all; else TLS itself.
            const std::string error = h.accept->error();
            const char *reason = error.find("certificate verify failed") != std::string::npos ? "unknown device"
                               : error.find("did not return a certificate") != std::string::npos || error.find("certificate required") != std::string::npos
                                   ? "no client certificate"
                                   : "handshake failed";
            directFailure(h.address, reason, error);
            h.done = true;
            return;
        }
        case TlsAccept::Step::Done:
            break;
        }
        h.done = true;
        const std::string print = h.accept->peerFingerprint();
        auto peer = std::make_unique<Peer>();
        peer->serial = ++serial_;
        peer->tcp = true;
        peer->address = h.address;
        auto stream = h.accept->take();
        peer->ssl = stream->ssl();
        peer->conn = std::make_unique<Conn>(std::move(stream));
        // The handshake just looked the device up: that answer counts, so
        // the file is parsed once per login.
        std::optional<Device> device;
        if (admitted_ && admitted_->print == print && Clock::now() - admitted_->at < std::chrono::seconds(10))
            device = admitted_->device;
        else
            device = Devices(devicesPath()).find(print);
        admitted_.reset();
        if (device) {
            peer->device = print;
            peer->deviceName = device->name;
            peer->guest = device->guest.has_value();
            // lastSeen is written at most once a minute per device.
            if (unixNow() - device->lastSeen >= 60)
                Devices(devicesPath()).seen(print, unixNow());
            logLine(h.address + ": login of \"" + device->name + "\""
                    + (device->guest ? " (guest of " + device->guest->session + ")" : std::string()));
        } else if ((window_ && Clock::now() < window_->until) || invitesOpen()) {
            peer->pairing = print;
            peer->deadline = h.deadline;
            peer->conn->setMaxLine(kPairLineMax);
            logLine(h.address + ": pairing attempt");
        } else {
            directFailure(h.address, "unknown device", "the pairing window closed");
            return;
        }
        peers_.push_back(std::move(peer));
    }

    // The one line a client in a pairing window sends.
    void onPair(Peer *peer, const json &message)
    {
        peer->conn->closeAfterFlush();
        auto reject = [&](const char *reason) {
            peer->conn->send({{"type", "rejected"}, {"reason", reason}});
        };
        const bool windowOpen = window_ && Clock::now() < window_->until;
        if (!windowOpen && !invitesOpen()) {
            reject("no-window");
            directFailure(peer->address, "no pairing window");
            return;
        }
        const auto request = parsePair(message);
        const std::string exporter = peer->ssl ? pairExporter(peer->ssl) : std::string();
        // The proof is keyed with the 16 bytes the code encodes.
        auto proves = [&](const std::string &code) {
            const auto key = base32Decode(code);
            return request && key && !exporter.empty() && sameSecret(hmacHex(*key, exporter), request->proof);
        };
        // An invitation to a session (one device, one use).
        for (auto it = invites_.begin(); it != invites_.end(); ++it)
            if (proves(it->code)) {
                const Invite invite = *it;
                invites_.erase(it);
                pairGuest(peer, *request, invite);
                return;
            }
        if (!windowOpen) {
            // Only invitations are open: a wrong proof costs the sender its
            // address, and the invitations stay (a passer-by must not spend them).
            reject("bad-proof");
            directFailure(peer->address, "bad proof");
            return;
        }
        const bool good = proves(window_->code);
        const unsigned long long waiter = window_->waiter;
        window_.reset();
        if (!good) {
            reject("bad-proof");
            directFailure(peer->address, "bad proof");
            notifyWaiter(waiter, {{"type", "pair-closed"}, {"reason", "bad-proof"}});
            return;
        }
        Devices(devicesPath()).add({peer->pairing, request->name, unixNow(), 0, std::nullopt});
        peer->conn->send({{"type", "paired"}, {"host", hostName()}, {"fingerprint", peer->pairing}});
        logLine(peer->address + ": paired \"" + request->name + "\" " + peer->pairing);
        notifyWaiter(waiter, {{"type", "pair-done"}, {"device", request->name}, {"fingerprint", peer->pairing}});
    }

    void notifyWaiter(unsigned long long serial, const json &message)
    {
        if (Peer *waiter = serial ? peerBySerial(serial) : nullptr) {
            waiter->conn->send(message);
            waiter->conn->closeAfterFlush();
        }
    }

    // pvai pair: a window for one device. Local only.
    void onPairOpen(Peer *peer, const json &message)
    {
        if (direct_.fd < 0 || !tls_) {
            peer->conn->send({{"type", "error"}, {"code", "not-listening"}, {"text", "pvai listen first"}});
            peer->conn->closeAfterFlush();
            return;
        }
        if (window_)
            notifyWaiter(window_->waiter, {{"type", "pair-closed"}, {"reason", "replaced"}});
        const int ttl = std::clamp(message.value("ttl", kPairWindowSeconds), 1, kPairWindowSeconds);
        std::string host = message.value("host", "");
        if (host.empty())
            host = linkHost();
        const std::string name = message.value("name", "").empty() ? hostName() : message.value("name", "");
        window_ = PairWindow{pairCode(), Clock::now() + std::chrono::seconds(ttl),
                             message.value("wait", false) ? peer->serial : 0};
        peer->conn->send({{"type", "pair-window"},
                          {"link", pairLink(host, direct_.port, tls_->fingerprint(), window_->code, name)},
                          {"seconds", ttl}});
        if (!window_->waiter)
            peer->conn->closeAfterFlush();
        logLine("pairing window open for " + std::to_string(ttl) + " s");
    }

    // The address a device should dial: the listening one, or for a
    // wildcard the host's first non-loopback address.
    std::string linkHost() const
    {
        if (direct_.address != "0.0.0.0" && direct_.address != "::")
            return direct_.address;
        std::string found;
        ifaddrs *list = nullptr;
        if (::getifaddrs(&list) != 0)
            return hostName();
        for (ifaddrs *i = list; i && found.empty(); i = i->ifa_next) {
            if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)
                || i->ifa_addr->sa_family != AF_INET)
                continue;
            char text[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr, text, sizeof text))
                found = text;
        }
        ::freeifaddrs(list);
        return found.empty() ? hostName() : found;
    }

    // pvai revoke: drops the device and its connections. Local only.
    void onRevoke(Peer *peer, const json &message)
    {
        bool saved = true;
        const auto removed = Devices(devicesPath()).remove(message.value("device", ""), &saved);
        if (!saved) {
            logLine("revoke: could not write " + devicesPath());
            peer->conn->send({{"type", "error"}, {"code", "save-failed"}, {"text", "the device list could not be written"}});
            peer->conn->closeAfterFlush();
            return;
        }
        json names = json::array();
        for (const auto &device : removed) {
            names.push_back(device.name);
            logLine("revoked \"" + device.name + "\" " + device.fingerprint + (device.guest ? " (guest of " + device.guest->session + ")" : std::string()));
            for (auto &other : peers_)
                if (other->device == device.fingerprint) {
                    other->conn->closeAfterFlush();
                    dead_.push_back(other.get());
                }
            expiredSeen_.erase(device.fingerprint);
        }
        refreshGuests(true);
        peer->conn->send({{"type", "revoked"}, {"devices", names}});
        peer->conn->closeAfterFlush();
    }

    // ---- Sharing a session (PROTOCOL.md, "Sharing a session") ----

    static bool expiredShare(const Guest &g) { return g.expires > 0 && unixNow() >= g.expires; }

    FileSig devicesSig() const
    {
        struct stat st;
        if (::stat(devicesFile_.c_str(), &st) != 0)
            return {};
        return {static_cast<long long>(st.st_mtime), static_cast<long long>(st.st_size), static_cast<long long>(st.st_ino)};
    }

    // The guests of devices.json, read again when the file changed.
    void refreshGuests(bool force = false)
    {
        const FileSig sig = devicesSig();
        if (!force && sig == guestsSig_)
            return;
        guestsSig_ = sig;
        guests_.clear();
        for (const Device &device : Devices(devicesPath()).load())
            if (device.guest)
                guests_[device.fingerprint] = device;
    }

    // The assistant's own id of a session's conversation: Claude Code's, else
    // the session's id (a Codex thread and a Kimi session are their conversation).
    static std::string conversationOf(Session &session)
    {
        std::string id = session.conversationId();
        if (id.empty() && session.assistant() != "claude")
            id = session.id();
        return id;
    }

    // The peer's guest entry, with its scope checked against the session's
    // conversation as it is now.
    Device *guestDevice(const Peer &peer)
    {
        const auto it = guests_.find(peer.device);
        if (it == guests_.end())
            return nullptr;
        checkScope(it->second);
        return &it->second;
    }

    void checkScope(Device &device)
    {
        if (device.guest->state != "active")
            return;
        Session *live = session(device.guest->session);
        if (!live)
            return;
        const std::string now = conversationOf(*live);
        if (!now.empty() && now != device.guest->conversation)
            setGuestState(device, "scope-moved", "the conversation changed");
    }

    void persistGuest(const Device &device)
    {
        if (!Devices(devicesPath()).setGuest(device.fingerprint, *device.guest))
            logLine("share: cannot write " + devicesPath());
        guestsSig_ = devicesSig(); // our own write: no need to read it back
    }

    void tellGuest(const Device &device, const std::string &state)
    {
        for (auto &peer : peers_)
            if (peer->guest && peer->device == device.fingerprint && !peer->conn->closing())
                peer->conn->send({{"type", "share-state"}, {"state", state}, {"session", device.guest->session}});
    }

    void setGuestState(Device &device, const std::string &state, const std::string &why)
    {
        if (device.guest->state == state)
            return;
        device.guest->state = state;
        persistGuest(device);
        sessionsDirty_ = true;
        logLine("share of \"" + device.name + "\" (" + device.guest->session + "): " + state + " (" + why + ")");
        tellGuest(device, state);
    }

    // A session is gone for good (its wrapper ended, stop, an assistant that
    // closed the thread); a restart that replaces it does not come here.
    void sessionEndedForShares(const std::string &id)
    {
        for (auto &[fingerprint, device] : guests_)
            if (device.guest->session == id && device.guest->state != "ended")
                setGuestState(device, "ended", "the session ended");
    }

    // A conversation was forgotten: its guest prompts go, and a share on it is over.
    void forgetConversation(const std::string &id)
    {
        prompts_->forget(id);
        for (auto &[fingerprint, device] : guests_)
            if (device.guest->conversation == id && device.guest->state != "ended")
                setGuestState(device, "ended", "the conversation was forgotten");
    }

    // Attention of these kinds is a dialog (the others are about the account, the process, the model).
    static bool dialogKind(const std::string &kind)
    {
        return kind != "login" && kind != "ended" && kind != "model" && kind != "limited" && kind != "stuck";
    }

    // What a guest is told of something sent to all apps: its session's
    // events only, and nothing at all while the share is not active.
    std::optional<json> guestView(Peer &peer, const json &message)
    {
        const Device *device = guestDevice(peer);
        if (!device)
            return std::nullopt;
        const Guest &g = *device->guest;
        const bool open = g.state == "active" && !expiredShare(g);
        const std::string type = str(message, "type");
        if (type == "sessions") {
            json list = json::array();
            if (open && message.contains("sessions"))
                for (const json &entry : message["sessions"])
                    if (str(entry, "id") == g.session) {
                        json copy = entry;
                        if (!g.dialogs) { // the dialog is the owner's to see unless the share has dialogs on
                            copy["dialog"] = nullptr;
                            copy["dialogInfo"] = nullptr;
                        }
                        list.push_back(std::move(copy));
                    }
            return json({{"type", "sessions"}, {"sessions", list}});
        }
        if (!open)
            return std::nullopt;
        if (type == "dropped") {
            json mine = json::array();
            for (const json &prompt : message["prompts"])
                if (str(prompt, "session") == g.session)
                    mine.push_back(prompt);
            if (mine.empty())
                return std::nullopt;
            return json({{"type", "dropped"}, {"prompts", mine}});
        }
        static const std::set<std::string> events = {"history", "entry", "say", "attention", "usage", "delivered", "uploaded",
                                                     "model", "effort", "mode", "dialog-answered"};
        if (!events.count(type) || str(message, "session") != g.session)
            return std::nullopt;
        if (type == "attention" && !g.dialogs && dialogKind(str(message, "kind")))
            return std::nullopt;
        return message;
    }

    // A guest's request: false when it was refused (and answered). Expiry and
    // the scope are checked on every request; `ping` always goes through.
    bool guestAllows(Peer *peer, const std::string &type, const json &message)
    {
        const json ref = message.contains("ref") ? message["ref"] : json();
        const json sessionField = message.contains("session") && message["session"].is_string() ? message["session"] : json();
        auto refuse = [&](const std::string &reason) {
            logLine("guest \"" + peer->deviceName + "\": " + type + " refused: " + reason);
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"session", sessionField}, {"reason", reason}});
            if (type == "upload" || type == "download" || type == "fetch")
                peer->conn->closeAfterFlush(); // what follows is not lines, or not for it
            return false;
        };
        if (type == "ping")
            return true;
        const Device *device = guestDevice(*peer);
        if (!device)
            return refuse("ended"); // its entry is gone (the connection is closing)
        const Guest &g = *device->guest;
        if (expiredShare(g))
            return refuse("expired");
        if (g.state != "active")
            return refuse(g.state);
        static const std::set<std::string> allowed = {"hello", "subscribe", "unsubscribe", "prompt", "interrupt", "upload", "fetch", "answer"};
        if (!allowed.count(type))
            return refuse("guest-denied");
        if (type == "hello")
            return true;
        if (!sessionField.is_string() || sessionField.get<std::string>() != g.session)
            return refuse("guest-denied");
        if (type == "answer" && !g.dialogs)
            return refuse("guest-denied");
        return true;
    }

    // Who answered: a guest by its device's name (never by what it sent), an
    // owner device by its name, an owner on this host (loopback, app socket, CLI) as "owner".
    static std::string answerer(const Peer &peer)
    {
        return peer.deviceName.empty() ? "owner" : peer.deviceName;
    }

    // A dialog was answered: every app hears who did, and the history says so.
    void noteAnswer(Peer *peer, Session &target, const std::string &choice)
    {
        const std::string by = answerer(*peer);
        const std::string shown = cut(oneLine(choice, 400), 200);
        logLine(target.id() + ": answer " + oneLine(choice, 40) + " by " + by);
        broadcast({{"type", "dialog-answered"}, {"session", target.id()}, {"choice", shown}, {"answeredBy", by}});
        json entry = {{"id", "a" + randomHex(6)}, {"time", nowIso()}, {"kind", "answer"}, {"text", shown}, {"answeredBy", by}};
        auto &own = daemonEntries_[target.id()];
        own.push_back(entry);
        while (own.size() > kMaxDaemonEntries)
            own.pop_front();
        toSubscribers(target.id(), {{"type", "entry"}, {"session", target.id()}, {"entry", entry}});
    }

    // A guest's prompt went into its session: the log (size, not the text) and
    // the record that lets the history say who sent it.
    void noteGuestPrompt(Peer *peer, Session &target, const std::string &text, const std::vector<std::string> &files)
    {
        const Device *device = guestDevice(*peer);
        if (!device)
            return;
        logLine("guest \"" + peer->deviceName + "\" prompt to " + target.id() + ": " + std::to_string(text.size()) + " bytes"
                + (files.empty() ? "" : ", " + std::to_string(files.size()) + " file(s)"));
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        prompts_->record(device->guest->conversation, text, files.empty() ? std::string() : withFiles(text, files), now, peer->deviceName);
    }

    bool invitesOpen()
    {
        const auto now = Clock::now();
        std::erase_if(invites_, [&](const Invite &i) { return i.until && now >= *i.until; });
        return !invites_.empty();
    }

    std::string uniqueDeviceName(const std::string &wanted)
    {
        std::set<std::string> taken = {"owner"};
        for (const Device &device : Devices(devicesPath()).load())
            taken.insert(device.name);
        if (!taken.count(wanted))
            return wanted;
        const std::string base = wanted.size() > 56 ? wanted.substr(0, utf8Boundary(wanted, 56)) : wanted;
        for (int n = 2;; ++n)
            if (const std::string candidate = base + " " + std::to_string(n); !taken.count(candidate))
                return candidate;
    }

    // The device that proved an invitation's code becomes a guest of its session.
    void pairGuest(Peer *peer, const PairRequest &request, const Invite &invite)
    {
        auto reject = [&](const char *reason) { peer->conn->send({{"type", "rejected"}, {"reason", reason}}); };
        Session *target = session(invite.session);
        if (!target) {
            reject("no-window");
            logLine(peer->address + ": invitation " + invite.id + ": the session " + invite.session + " is gone");
            return;
        }
        Device device{peer->pairing, uniqueDeviceName(request.name), unixNow(), 0, std::nullopt};
        Guest guest{invite.assistant, invite.session, invite.conversation, invite.expires, invite.dialogs, "active"};
        const std::string now = conversationOf(*target);
        if (!now.empty() && now != invite.conversation)
            guest.state = "scope-moved"; // the conversation moved on since the invitation: the owner confirms
        device.guest = guest;
        if (!Devices(devicesPath()).add(device)) {
            logLine("share: cannot write " + devicesPath());
            peer->conn->send({{"type", "error"}, {"code", "save-failed"}, {"text", "the device list could not be written"}});
            return;
        }
        refreshGuests(true);
        peer->conn->send({{"type", "paired"}, {"host", hostName()}, {"fingerprint", peer->pairing}, {"session", invite.session}, {"guest", true}});
        logLine(peer->address + ": paired guest \"" + device.name + "\" " + peer->pairing + " for session " + invite.session
                + (invite.dialogs ? " (dialogs on)" : "") + (guest.state != "active" ? ", " + guest.state : std::string()));
        sessionsDirty_ = true;
    }

    // A session by id, or by label when exactly one has it.
    Session *sessionFor(const std::string &key)
    {
        if (Session *exact = session(key))
            return exact;
        Session *found = nullptr;
        for (Session *candidate : sessions())
            if (candidate->label() == key) {
                if (found)
                    return nullptr;
                found = candidate;
            }
        return found;
    }

    Device *findGuest(const std::string &key)
    {
        for (auto &[fingerprint, device] : guests_)
            if (fingerprint == key || device.name == key)
                return &device;
        return nullptr;
    }

    json shareJson(const Device &device)
    {
        const Guest &g = *device.guest;
        const Session *live = session(g.session);
        return {{"name", device.name}, {"fingerprint", device.fingerprint}, {"added", device.added}, {"lastSeen", device.lastSeen},
                {"session", g.session}, {"assistant", g.assistant}, {"label", live ? json(live->label()) : json()},
                {"conversation", g.conversation}, {"expires", g.expires > 0 ? json(g.expires) : json()},
                {"expired", expiredShare(g)}, {"dialogs", g.dialogs}, {"state", g.state}};
    }

    // The requests of the owner (share, shares, share-revoke, share-confirm,
    // share-move): from the CLI on the daemon's socket, or from an app that
    // is not a guest (a guest never gets this far).
    void onShareRequest(Peer *peer, const std::string &type, const json &message)
    {
        const json ref = message.contains("ref") ? message["ref"] : json();
        auto answer = [&](const json &reply) {
            peer->conn->send(reply);
            if (peer->kind == Kind::Unknown)
                peer->conn->closeAfterFlush();
        };
        auto reject = [&](const std::string &reason, const std::string &text = {}) {
            json reply = {{"type", "rejected"}, {"ref", ref}, {"reason", reason}};
            if (!text.empty())
                reply["text"] = text;
            logLine(type + " refused: " + reason);
            answer(reply);
        };
        auto field = [&](const char *key) {
            return message.contains(key) && message[key].is_string() ? message[key].get<std::string>() : std::string();
        };
        refreshGuests();
        if (type == "shares") {
            json list = json::array(), open = json::array();
            for (auto &[fingerprint, device] : guests_) {
                checkScope(device);
                list.push_back(shareJson(device));
            }
            invitesOpen();
            for (const Invite &invite : invites_) {
                const Session *live = session(invite.session);
                open.push_back({{"id", invite.id}, {"session", invite.session}, {"assistant", invite.assistant},
                                {"label", live ? json(live->label()) : json()}, {"dialogs", invite.dialogs},
                                {"expires", invite.expires > 0 ? json(invite.expires) : json()}});
            }
            return answer({{"type", "shares"}, {"ref", ref}, {"shares", list}, {"invites", open}});
        }
        if (type == "share") {
            const std::string key = field("session");
            Session *target = key.empty() ? nullptr : sessionFor(key);
            if (!target)
                return reject("unknown-session");
            if (direct_.fd < 0 || !tls_)
                return reject("not-listening", "direct access is off; turn it on with pvai listen ADDR");
            long long ttl = kShareDefaultSeconds; // 0: never
            if (message.contains("ttl") && !message["ttl"].is_null()) {
                const json &value = message["ttl"];
                if (value.is_string() && value.get<std::string>() == "none") {
                    ttl = 0;
                } else if (value.is_string()) {
                    const auto parsed = parseDuration(value.get<std::string>());
                    if (!parsed)
                        return reject("bad-ttl");
                    ttl = *parsed;
                } else if (value.is_number_integer() && value.get<long long>() >= 1 && value.get<long long>() <= kShareMaxSeconds) {
                    ttl = value.get<long long>();
                } else {
                    return reject("bad-ttl");
                }
            }
            if (message.contains("dialogs") && !message["dialogs"].is_null() && !message["dialogs"].is_boolean())
                return reject("bad-request");
            const std::string conversation = conversationOf(*target);
            if (conversation.empty())
                return reject("not-ready", "the session has not reported its conversation yet");
            invitesOpen();
            if (invites_.size() >= kMaxInvites)
                return reject("too-many");
            Invite invite;
            invite.id = "i" + randomHex(3);
            invite.code = pairCode();
            invite.session = target->id();
            invite.assistant = target->assistant();
            invite.conversation = conversation;
            invite.dialogs = message.value("dialogs", false);
            if (ttl > 0) {
                invite.expires = unixNow() + ttl;
                invite.until = Clock::now() + std::chrono::seconds(ttl);
            }
            std::string host = field("host");
            if (host.empty())
                host = linkHost();
            const std::string name = field("name").empty() ? hostName() : oneLine(field("name"), kDeviceNameMax);
            const std::string link = shareLink(host, direct_.port, tls_->fingerprint(), invite.code, name, invite.session);
            invites_.push_back(invite);
            logLine("share created: session " + invite.session + " (" + invite.assistant + "), invitation " + invite.id
                    + (invite.dialogs ? ", dialogs on" : "") + (ttl > 0 ? ", " + std::to_string(ttl) + " s" : ", no expiry"));
            return answer({{"type", "shared"}, {"ref", ref}, {"id", invite.id}, {"link", link}, {"session", invite.session},
                           {"assistant", invite.assistant}, {"label", target->label()}, {"dialogs", invite.dialogs},
                           {"expires", invite.expires > 0 ? json(invite.expires) : json()}, {"warning", kShareWarning}});
        }
        const std::string key = field("name");
        if (type == "share-revoke") {
            if (key.empty())
                return reject("bad-request");
            bool saved = true;
            const auto removed = Devices(devicesPath()).removeGuests(key, &saved);
            if (!saved)
                return reject("save-failed", "the device list could not be written");
            json names = json::array(), cancelled = json::array();
            for (const Device &device : removed) {
                names.push_back(device.name);
                logLine("share revoked: \"" + device.name + "\" " + device.fingerprint + " (" + device.guest->session + ")");
                for (auto &other : peers_)
                    if (other->device == device.fingerprint) {
                        other->conn->closeAfterFlush();
                        dead_.push_back(other.get());
                    }
                expiredSeen_.erase(device.fingerprint);
            }
            refreshGuests(true);
            for (auto it = invites_.begin(); it != invites_.end();) {
                if (it->id == key) {
                    cancelled.push_back(it->id);
                    logLine("share invitation cancelled: " + it->id + " (" + it->session + ")");
                    it = invites_.erase(it);
                } else {
                    ++it;
                }
            }
            if (names.empty() && cancelled.empty())
                return reject("unknown-share");
            return answer({{"type", "share-revoked"}, {"ref", ref}, {"shares", names}, {"invites", cancelled}});
        }
        Device *device = key.empty() ? nullptr : findGuest(key);
        if (!device)
            return reject(key.empty() ? "bad-request" : "unknown-share");
        checkScope(*device);
        Guest &g = *device->guest;
        if (type == "share-confirm") {
            if (g.state != "scope-moved")
                return reject("not-scope-moved");
            Session *live = session(g.session);
            if (!live)
                return reject("unknown-session");
            const std::string conversation = conversationOf(*live);
            if (conversation.empty())
                return reject("not-ready", "the session has not reported its conversation yet");
            g.conversation = conversation;
            g.state = "active";
            persistGuest(*device);
            logLine("share of \"" + device->name + "\" (" + g.session + ") confirmed for conversation " + conversation);
            reopenGuests(*device);
            return answer({{"type", "share-confirmed"}, {"ref", ref}, {"name", device->name}, {"session", g.session}, {"conversation", conversation}});
        }
        // share-move: the same conversation under another session id (a Claude `start.resume`).
        Session *to = sessionFor(field("session"));
        if (!to)
            return reject("unknown-session");
        if (to->assistant() != g.assistant || conversationOf(*to) != g.conversation || g.conversation.empty())
            return reject("conversation-mismatch");
        const std::string from = g.session;
        g.session = to->id();
        g.state = "active";
        persistGuest(*device);
        missing_.erase(from);
        logLine("share of \"" + device->name + "\" moved from " + from + " to " + g.session);
        reopenGuests(*device);
        return answer({{"type", "share-moved"}, {"ref", ref}, {"name", device->name}, {"session", g.session}, {"from", from}});
    }

    // The share is active again (confirmed, or moved): the guest's connections
    // are told, and get the session list; they subscribe again.
    void reopenGuests(const Device &device)
    {
        sessionsDirty_ = true;
        for (auto &peer : peers_)
            if (peer->guest && peer->device == device.fingerprint && !peer->conn->closing()) {
                peer->subscriptions.clear();
                peer->conn->send({{"type", "share-state"}, {"state", "active"}, {"session", device.guest->session}});
            }
    }

    // Once per loop: invitations that ran out, shares that expired (their
    // connections close), conversations that moved, sessions that did not come back.
    void shareTick()
    {
        invitesOpen();
        const auto now = Clock::now();
        for (auto &[fingerprint, device] : guests_) {
            Guest &g = *device.guest;
            if (expiredShare(g)) {
                if (expiredSeen_.insert(fingerprint).second)
                    logLine("share of \"" + device.name + "\" (" + g.session + ") expired");
                for (auto &peer : peers_)
                    if (peer->guest && peer->kind == Kind::App && peer->device == fingerprint && !peer->conn->closing()) {
                        peer->conn->send({{"type", "share-state"}, {"state", "expired"}, {"session", g.session}});
                        peer->conn->closeAfterFlush();
                        dead_.push_back(peer.get());
                    }
                continue;
            }
            if (g.state == "ended")
                continue;
            checkScope(device);
            if (session(g.session)) {
                missing_.erase(g.session);
            } else if (const auto since = missing_.find(g.session); since == missing_.end()) {
                missing_[g.session] = now;
            } else if (now - since->second >= shareGrace()) {
                setGuestState(device, "ended", "the session did not come back");
            }
        }
    }

    // When shareTick is due: the next expiry of a connected guest, the end of
    // a grace period, an invitation's end; every 5 s while a share is open.
    std::optional<Clock::time_point> shareDeadline() const
    {
        std::optional<Clock::time_point> next;
        auto take = [&](Clock::time_point at) {
            if (!next || at < *next)
                next = at;
        };
        const auto now = Clock::now();
        for (const Invite &invite : invites_)
            if (invite.until)
                take(*invite.until);
        for (const auto &[fingerprint, device] : guests_) {
            const Guest &g = *device.guest;
            if (g.state != "ended")
                take(now + std::chrono::seconds(5));
            if (g.expires > 0 && !expiredSeen_.count(fingerprint))
                take(now + std::chrono::seconds(std::max<long long>(0, g.expires - unixNow())));
            if (const auto since = missing_.find(g.session); since != missing_.end())
                take(since->second + shareGrace());
        }
        return next;
    }

    // Deadlines before login, the pairing window, the lockout's memory.
    void directTick()
    {
        const auto now = Clock::now();
        for (auto &h : handshakes_)
            if (!h.done && now >= h.deadline) {
                directFailure(h.address, "handshake timed out");
                h.done = true;
            }
        std::erase_if(handshakes_, [](const Handshake &h) { return h.done; });
        for (auto &peer : peers_) {
            if (!peer->pairing.empty() && peer->deadline && now >= *peer->deadline) {
                peer->deadline.reset();
                directFailure(peer->address, "pairing timed out");
                dead_.push_back(peer.get());
            } else if (peer->pairing.empty() && peer->deadline && now >= *peer->deadline) {
                // No hello, register or attach in time.
                peer->deadline.reset();
                if (peer->kind == Kind::Unknown)
                    dead_.push_back(peer.get());
            } else if (peer->upload && !peer->conn->closing() && now - peer->lastReceived >= uploadIdle()) {
                uploadEnded(peer.get(), "short"); // nothing came for too long
            } else if (peer->download && !peer->conn->closing() && now - peer->download->progress >= downloadIdle()) {
                logLine("download: no progress, the peer is gone");
                dead_.push_back(peer.get()); // 60 s without a byte leaving
            } else if (silent(*peer, now) || peer->conn->pending() > kMaxOut) {
                logLine((peer->address.empty() ? std::string("a peer") : peer->address) + ": dropped, "
                        + (peer->conn->pending() > kMaxOut ? "too much unsent" : "silent"));
                dead_.push_back(peer.get());
            }
        }
        if (window_ && now >= window_->until) {
            notifyWaiter(window_->waiter, {{"type", "pair-closed"}, {"reason", "expired"}});
            window_.reset();
            logLine("pairing window closed");
        }
        lockout_.prune(now);
    }

    // An app on TCP (loopback or direct) pings every 15 s. Unix-socket peers
    // (CLI, hooks, wrappers, processes) may wait as long as they like.
    static bool silent(const Peer &peer, Clock::time_point now)
    {
        return peer.tcp && peer.kind == Kind::App && !peer.detached() && now - peer.lastReceived >= kAppSilence;
    }

    std::optional<Clock::time_point> directDeadline() const
    {
        std::optional<Clock::time_point> next;
        auto take = [&](Clock::time_point at) {
            if (!next || at < *next)
                next = at;
        };
        for (const auto &h : handshakes_)
            take(h.deadline);
        for (const auto &peer : peers_)
            if (peer->deadline)
                take(*peer->deadline);
            else if (peer->upload)
                take(peer->lastReceived + uploadIdle());
            else if (peer->download)
                take(peer->download->progress + downloadIdle());
            else if (peer->tcp && peer->kind == Kind::App)
                take(peer->lastReceived + kAppSilence);
        if (window_)
            take(window_->until);
        return next;
    }


    int unixFd_;
    int appFd_;                 // the app socket; -1 without
    int tcpFd_;                 // 127.0.0.1:port; -1 when taken
    int port_;
    std::string appSocket_;     // its path; empty without
    std::string listenError_;   // why direct access is not up
    std::string token_ = token();
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<Peer *> dead_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    json savedLabels_ = json::object(); // "<assistant>/<id>" -> the owner's label (labels.json)
    // Prompts of ended Claude sessions, by conversation and folder (see
    // sessionGone); in memory only, oldest first.
    std::deque<KeptPrompt> kept_;
    unsigned long long keptSeq_ = 0;
    bool sessionsDirty_ = false;
    Clock::time_point nextSweep_; // the next pass over the inbox
    unsigned long long serial_ = 0;
    std::vector<std::unique_ptr<Backend>> backends_;
    ClaudeBackend *claude_ = nullptr; // its sessions come from wrappers
    Clock::time_point versionsRead_;
    std::vector<Job> jobs_;

    // Direct access
    std::string listenSpec_;
    DirectListener direct_;
    std::unique_ptr<TlsServer> tls_;
    std::vector<Handshake> handshakes_;
    std::optional<PairWindow> window_;
    Lockout lockout_;

    // Sharing a session: open invitations, the guests (the devices that have
    // a share, as devices.json holds them, read again when it changes), what
    // guests sent, and the daemon's own history entries.
    std::vector<Invite> invites_;
    std::map<std::string, Device> guests_;
    std::string devicesFile_; // devicesPath(), looked up once
    FileSig guestsSig_;
    std::unique_ptr<GuestPrompts> prompts_;
    std::map<std::string, std::deque<json>> daemonEntries_; // per session: dialog answers
    std::map<std::string, Clock::time_point> missing_;      // a share's session that is not there, since
    std::set<std::string> expiredSeen_;                     // guests whose expiry was logged
    static constexpr std::size_t kMaxDaemonEntries = 100;

    // Agent messages
    std::vector<Proposal> proposals_;
    json otherHosts_ = json::array(); // sessions on other hosts, from the app's last `peers`
    bool stopping_ = false;           // `shutdown` accepted: the loop ends after this round
    std::unique_ptr<Setup> setup_;
    std::string peersSelf_;      // the app's name for this host
    long long peersAt_ = 0;      // when it came, Unix time
    std::map<std::string, std::vector<Clock::time_point>> proposalTimes_; // per sender, the last minute

};

} // namespace

int runDaemon(int port)
{
    ::openlog("pvai", LOG_PID, LOG_AUTH);
    // One daemon per user and host: whoever holds the lock owns the socket.
    const std::string lockPath = runtimeDir() + "/daemon.lock";
    const int lock = ::open(lockPath.c_str(), O_RDWR | O_CREAT, 0600);
    if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "pvai daemon: already running\n");
        return 1;
    }
    setCloseOnExec(lock);
    const int unixFd = listenUnix(daemonSocket());
    if (unixFd < 0) {
        std::fprintf(stderr, "pvai daemon: cannot listen on %s\n", daemonSocket().c_str());
        return 1;
    }
    // The app socket: the same apps as on 127.0.0.1, for a host with several
    // users. The lock above makes this daemon its only owner, so an old file is stale.
    const std::string appPath = appSocket();
    int appFd = -1;
    if (appPath.size() >= sizeof(sockaddr_un::sun_path)) {
        std::fprintf(stderr, "pvai daemon: app socket path too long, no app socket: %s\n", appPath.c_str());
    } else if ((appFd = listenUnix(appPath)) < 0) {
        std::fprintf(stderr, "pvai daemon: cannot listen on %s: %s; no app socket\n", appPath.c_str(), std::strerror(errno));
    }
    const int tcpFd = listenTcp("127.0.0.1", port);
    if (tcpFd < 0) {
        const int saved = errno;
        std::fprintf(stderr, "pvai daemon: app port 127.0.0.1:%d is %s%s; apps reach this daemon over %s only\n", port,
                     saved == EADDRINUSE ? "taken (another user's daemon?)" : "not available: ",
                     saved == EADDRINUSE ? "" : std::strerror(saved), appFd >= 0 ? appPath.c_str() : daemonSocket().c_str());
    }
    std::fprintf(stderr, "pvai daemon %s: %s", kVersion, daemonSocket().c_str());
    if (appFd >= 0)
        std::fprintf(stderr, ", %s", appPath.c_str());
    if (tcpFd >= 0)
        std::fprintf(stderr, ", 127.0.0.1:%d", port);
    std::fprintf(stderr, "\n");
    Daemon daemon(unixFd, appFd, tcpFd, port);
    daemon.setAppSocket(appFd >= 0 ? appPath : std::string());
    return daemon.run();
}

} // namespace pvai
