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
#include "pairing.h"
#include "setup.h"
#include "session.h"
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
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
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

namespace {

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
    SSL *ssl = nullptr; // owned by conn's stream
    std::optional<std::chrono::steady_clock::time_point> deadline; // before login
    std::string client;                 // apps: `hello.client`
    std::string sessionId;              // wrappers
    std::string assistant;              // processes: whose
    std::set<std::string> subscriptions; // apps: sessions whose transcript they follow
};

constexpr int kDirectPort = 7471;
constexpr int kPairWindowSeconds = 600;
constexpr auto kPreAuth = std::chrono::seconds(10);
constexpr std::size_t kMaxHandshakes = 32;

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

// An agent message waiting for the owner (PROTOCOL.md, "Agent messages").
struct Proposal {
    std::string id;
    std::string from; // the sender's session id, empty from a Codex thread
    std::string fromLabel;
    std::string to;   // host or host/session
    std::string text;
    std::string replyTo; // the proposal this one answers, or empty
    long long at = 0; // Unix time
    Clock::time_point expires;
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
};

constexpr std::size_t kMaxJobOutput = 64 * 1024;
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
    Daemon(int unixFd, int tcpFd)
        : unixFd_(unixFd)
        , tcpFd_(tcpFd)
    {
        // The assistants this daemon knows, in the order the app lists them.
        auto claude = std::make_unique<ClaudeBackend>(*this);
        claude_ = claude.get();
        backends_.push_back(std::move(claude));
        backends_.push_back(std::make_unique<CodexBackend>(*this));
        backends_.push_back(std::make_unique<KimiBackend>(*this));
        setup_ = std::make_unique<Setup>([this](const json &event) { broadcast(event); },
                                         [this](const std::string &name, bool install, bool ok) { setupFinished(name, install, ok); });
        loadListen();
        refreshVersions();
    }

    int run()
    {
        for (;;) {
            std::vector<pollfd> fds;
            fds.push_back({unixFd_, POLLIN, 0});
            fds.push_back({tcpFd_, POLLIN, 0});
            fds.push_back({direct_.fd, POLLIN, 0});
            const std::size_t firstBackend = fds.size();
            for (const auto &backend : backends_)
                fds.push_back({backend->fd(), backend->events(), 0});
            const std::size_t firstJob = fds.size();
            for (const auto &job : jobs_)
                fds.push_back({job.fd, POLLIN, 0});
            const std::size_t firstSetup = fds.size();
            const std::vector<int> setupFds = setup_->fds();
            for (const int fd : setupFds)
                fds.push_back({fd, POLLIN, 0});
            const std::size_t firstHandshake = fds.size();
            for (const auto &h : handshakes_)
                fds.push_back({h.accept->fd(), h.accept->events(), 0});
            const std::size_t firstPeer = fds.size();
            for (const auto &peer : peers_)
                fds.push_back({peer->conn->fd(), static_cast<short>(POLLIN | (peer->conn->wantsWrite() ? POLLOUT : 0)), 0});
            if (::poll(fds.data(), fds.size(), nextTimeout()) < 0 && errno != EINTR)
                return 1;
            const auto now = Clock::now();
            for (Session *session : sessions())
                session->tick(now);
            for (std::size_t i = 0; i < backends_.size(); ++i)
                if (fds[firstBackend + i].fd >= 0 && fds[firstBackend + i].revents)
                    backends_[i]->service();
            if (fds[0].revents & POLLIN)
                accept(unixFd_, false);
            if (fds[1].revents & POLLIN)
                accept(tcpFd_, true);
            if (direct_.fd >= 0 && fds[2].fd == direct_.fd && (fds[2].revents & POLLIN))
                acceptDirect();
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
                backend->tick(Clock::now());
            directTick();
            expireProposals();
            sweepSessions();
            reap();
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
        for (auto &peer : peers_)
            if (peer->kind == Kind::App)
                peer->conn->send(message);
    }

    void sessionsChanged() override { sessionsDirty_ = true; }

    void toSubscribers(const std::string &id, const json &message) override
    {
        for (auto &peer : peers_)
            if (peer->subscriptions.count(id))
                peer->conn->send(message);
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
        sessions_[id] = std::move(session);
        sessionsDirty_ = true;
    }

    void runProcess(const std::vector<std::string> &argv, std::chrono::seconds timeout,
                    std::function<void(int status, const std::string &output)> done) override
    {
        int out[2];
        if (argv.empty() || ::pipe(out) != 0) {
            done(127, "cannot start");
            return;
        }
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::setpgid(0, 0);
            const int null = ::open("/dev/null", O_RDONLY);
            ::dup2(null, STDIN_FILENO);
            ::dup2(out[1], STDOUT_FILENO);
            ::dup2(out[1], STDERR_FILENO);
            dropClaudeEnvironment();
            std::vector<char *> args;
            for (const auto &arg : argv)
                args.push_back(const_cast<char *>(arg.c_str()));
            args.push_back(nullptr);
            ::execvp(args[0], args.data());
            ::_exit(127);
        }
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

private:
    void accept(int listenFd, bool tcp)
    {
        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0)
            return;
        auto peer = std::make_unique<Peer>();
        peer->conn = std::make_unique<Conn>(fd);
        peer->serial = ++serial_;
        peer->tcp = tcp;
        peers_.push_back(std::move(peer));
    }

    void service(Peer *peer)
    {
        std::vector<json> messages;
        if (!peer->conn->readMessages(messages))
            dead_.push_back(peer);
        for (const auto &message : messages) {
            if (peer->conn->closing())
                break;
            handle(peer, message);
        }
        if (!peer->conn->flush())
            dead_.push_back(peer);
        else if (peer->conn->closing() && !peer->conn->wantsWrite())
            dead_.push_back(peer);
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
            if (peer->kind == Kind::Wrapper && sessions_.erase(peer->sessionId))
                sessionsDirty_ = true;
            if (peer->kind == Kind::Process)
                if (Backend *owner = backend(peer->assistant))
                    owner->processEnded(peer->serial);
        }
        std::erase_if(peers_, [&](const auto &p) { return std::find(dead_.begin(), dead_.end(), p.get()) != dead_.end(); });
        dead_.clear();
    }

    void sweepSessions()
    {
        if (std::erase_if(sessions_, [](const auto &entry) { return entry.second->ended(); }))
            sessionsDirty_ = true;
    }

    void serviceJobs(const std::vector<pid_t> &readable)
    {
        const auto now = Clock::now();
        std::vector<std::pair<std::function<void(int, const std::string &)>, std::pair<int, std::string>>> finished;
        for (auto &job : jobs_) {
            bool eof = false;
            if (std::find(readable.begin(), readable.end(), job.pid) != readable.end()) {
                char buffer[4096];
                for (;;) {
                    const ssize_t n = ::read(job.fd, buffer, sizeof buffer);
                    if (n > 0) {
                        if (job.output.size() < kMaxJobOutput)
                            job.output.append(buffer, static_cast<std::size_t>(n));
                        continue;
                    }
                    eof = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
                    break;
                }
            }
            int status = 0;
            if (eof) {
                ::waitpid(job.pid, &status, 0);
                status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            } else if (now >= job.deadline) {
                ::kill(-job.pid, SIGKILL);
                ::waitpid(job.pid, nullptr, 0);
                status = -1;
            } else {
                continue;
            }
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
            reason = !chosen->installedHere() ? "not-installed" : setup_->login(chosen->name(), chosen->program(), ref);
        else if (reason.empty() && type == "login-code")
            reason = setup_->loginCode(chosen->name(), message.value("code", ""));
        else if (reason.empty())
            reason = setup_->loginCancel(chosen->name());
        if (!reason.empty())
            peer->conn->send({{"type", "rejected"}, {"ref", ref}, {"reason", reason}});
        else if (type == "login")
            logLine("login of " + chosen->name() + " started");
    }

    // An install or login ended: the program, its version and its login
    // are read again, and every app hears the new state.
    void setupFinished(const std::string &name, bool install, bool ok)
    {
        logLine(std::string(install ? "install" : "login") + " of " + name + (ok ? " done" : " failed"));
        Backend *chosen = backend(name);
        if (!chosen)
            return;
        if (install)
            chosen->findProgram();
        if (!install && ok)
            if (auto *kimi = dynamic_cast<KimiBackend *>(chosen))
                kimi->setLoggedIn(true);
        broadcast(assistantsMessage());
        chosen->refreshVersion([this] { broadcast(assistantsMessage()); });
        chosen->refreshLogin([this](bool changed) {
            if (changed)
                broadcast(assistantsMessage());
        });
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
        return {{"type", "welcome"}, {"host", hostName()},       {"version", kVersion},
                {"assistants", assistants}, {"installed", installed}, {"conversations", true},
                {"peers", true}};
    }

    void handle(Peer *peer, const json &message)
    {
        const std::string type = message.is_object() ? message.value("type", "") : "";
        if (type.empty()) {
            peer->conn->send({{"type", "error"}, {"code", "bad-message"}, {"text", "not a JSON object with a type"}});
            return;
        }
        if (!peer->pairing.empty()) {
            onPair(peer, message);
            return;
        }
        if (peer->kind == Kind::Unknown) {
            if (!peer->tcp && (type == "pair-open" || type == "revoke" || type == "listen")) {
                if (type == "pair-open")
                    onPairOpen(peer, message);
                else if (type == "revoke")
                    onRevoke(peer, message);
                else {
                    loadListen();
                    peer->conn->send({{"type", "listening"}, {"spec", listenSpec_}, {"ok", direct_.fd >= 0 || listenSpec_.empty()},
                                      {"fingerprint", tls_ ? tls_->fingerprint() : ""}});
                    peer->conn->closeAfterFlush();
                }
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
                peer->conn->send(welcomeMessage());
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
                session->onInput(message.value("escape", false));
            else if (type == "draft")
                session->onDraft(message.value("dirty", false));
            else if (type == "dialog")
                session->onDialog(message.value("kind", ""));
            else if (type == "exited")
                session->onExited(message.value("status", 0), message.value("screen", json()).is_string() ? message["screen"].get<std::string>() : "");
            else if (type == "screen")
                session->onScreen(message.value("dialog", json()).is_string() ? message["dialog"].get<std::string>() : "",
                                  message.value("prompt", false));
            else if (type == "typed")
                session->onTyped(message.value("ok", false), message.value("screen", json()).is_string() ? message["screen"].get<std::string>() : "");
            else if (type == "snapshot")
                session->onSnapshot(message.value("screen", json()).is_string() ? message["screen"].get<std::string>() : "");
            return;
        }
        if (type == "ping")
            peer->conn->send({{"type", "pong"}});
        else if (type == "subscribe")
            onSubscribe(peer, message);
        else if (type == "start")
            onStart(peer, message);
        else if (type == "conversations")
            onConversations(peer, message);
        else if (type == "install")
            onInstall(peer, message);
        else if (type == "login" || type == "login-code" || type == "login-cancel")
            onLogin(peer, message);
        else if (type == "update")
            onUpdate(peer, message);
        else if (type == "projects")
            peer->conn->send({{"type", "projects"}, {"dirs", knownProjects()}});
        else if (type == "browse")
            peer->conn->send(browse(message.value("path", "")));
        else if (type == "answer")
            onAnswer(peer, message);
        else if (type == "stop")
            onStop(peer, message);
        else if (type == "interrupt")
            onInterrupt(peer, message);
        else if (type == "restart")
            onRestart(peer, message);
        else if (type == "unsubscribe")
            peer->subscriptions.erase(message.value("session", ""));
        else if (type == "prompt")
            onPrompt(peer, message);
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
        const char *home = std::getenv("HOME");
        const std::filesystem::path root = std::filesystem::path(home ? home : "/") / ".claude" / "projects";
        std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
        std::error_code ec;
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
            std::ifstream in(newest);
            std::string line;
            for (int i = 0; i < 50 && std::getline(in, line); ++i) {
                const json entry = json::parse(line, nullptr, false);
                const std::string cwd = entry.is_object() ? entry.value("cwd", "") : "";
                if (cwd.empty())
                    continue;
                if (std::filesystem::is_directory(cwd, ec))
                    found.emplace_back(newestTime, cwd);
                break;
            }
        }
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

    void onAnswer(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        if (const std::string reason = session->answer(message.value("choice", "")); !reason.empty())
            return reject(peer, ref, id, reason);
        peer->conn->send({{"type", "answered"}, {"ref", ref}, {"session", id}});
    }

    void onStop(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        if (const std::string reason = session->stop(); !reason.empty())
            return reject(peer, ref, id, reason);
        peer->conn->send({{"type", "stopping"}, {"ref", ref}, {"session", id}});
    }

    void onInterrupt(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        session->interrupt();
        peer->conn->send({{"type", "interrupting"}, {"ref", ref}, {"session", id}});
    }

    void onRestart(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        if (const std::string reason = session->restart(); !reason.empty())
            return reject(peer, ref, id, reason);
        peer->conn->send({{"type", "restarting"}, {"ref", ref}, {"session", id}});
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
        peer->conn->send({{"type", "history"}, {"session", id}, {"entries", session->history(message.value("limit", 200))}});
    }

    void onPrompt(Peer *peer, const json &message)
    {
        const json ref = message.value("ref", json());
        const std::string id = message.value("session", "");
        const std::string text = trim(message.value("text", ""));
        Session *session = this->session(id);
        if (!session)
            return reject(peer, ref, id, "unknown-session");
        if (text.empty())
            return reject(peer, ref, id, "empty");
        if (text.size() > kMaxPrompt)
            return reject(peer, ref, id, "too-long");
        const bool typed = session->prompt({fromLine(message) + text, message.value("voice", false), ref});
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
        if (text.empty())
            return reject("empty");
        if (text.size() > kMaxPrompt)
            return reject("too-long");
        if (!goodTarget(to))
            return reject("bad-target");
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
        recent.push_back(now);
        Proposal proposal{"p" + randomHex(4), from, {}, to, text, replyTo, static_cast<long long>(std::time(nullptr)),
                          now + kProposalLife};
        if (const Session *sender = session(from))
            proposal.fromLabel = sender->label();
        proposals_.push_back(proposal);
        int apps = 0;
        for (const auto &other : peers_)
            if (other->kind == Kind::App && other->client != "pvai")
                ++apps;
        logLine("proposal " + proposal.id + " from " + (from.empty() ? "?" : from) + " to " + to
                + (replyTo.empty() ? "" : ", answering " + replyTo));
        peer->conn->send({{"type", "proposed"}, {"ref", ref}, {"id", proposal.id}, {"to", to}, {"apps", apps}});
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
                dropped.push_back({{"session", session->id()}, {"ref", p.ref}, {"text", cut(oneLine(p.text, 400), 80)}});
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
        peer->conn->send({{"type", "shutting-down"}, {"ref", ref}, {"dropped", dropped}});
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
        if (std::erase_if(proposals_, [&](const Proposal &p) { return p.id == id; }))
            broadcast(proposalsMessage());
        peer->conn->send({{"type", "dismissed"}, {"ref", message.value("ref", json())}, {"proposal", id}});
    }

    void expireProposals()
    {
        const auto now = Clock::now();
        if (std::erase_if(proposals_, [&](const Proposal &p) { return now >= p.expires; }))
            broadcast(proposalsMessage());
        std::erase_if(proposalTimes_, [&](const auto &entry) {
            return entry.second.empty() || now - entry.second.back() >= std::chrono::minutes(1);
        });
    }

    json proposalsMessage() const
    {
        json list = json::array();
        for (const auto &p : proposals_)
            list.push_back({{"id", p.id}, {"from", p.from}, {"fromLabel", p.fromLabel}, {"to", p.to}, {"text", p.text},
                            {"replyTo", p.replyTo.empty() ? json() : json(p.replyTo)}, {"at", p.at}});
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
            take(job.deadline);
        take(setup_->deadline());
        take(directDeadline());
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
            logLine("direct access: cannot read listen setting \"" + spec + "\"");
            return;
        }
        if (!tls_) {
            tls_ = std::make_unique<TlsServer>([this](const std::string &print) { return admit(print); });
            if (!tls_->ready()) {
                logLine("direct access: no TLS identity in " + configDir());
                tls_.reset();
                return;
            }
        }
        direct_.fd = listenAny(address, port);
        if (direct_.fd < 0) {
            logLine("direct access: cannot listen on " + spec + ": " + std::strerror(errno));
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

    // In the handshake: a paired device, or anyone while a window is open.
    bool admit(const std::string &print)
    {
        if (Devices(devicesPath()).find(print))
            return true;
        return window_ && Clock::now() < window_->until;
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
        if (const auto device = Devices(devicesPath()).find(print)) {
            peer->device = print;
            peer->deviceName = device->name;
            Devices(devicesPath()).seen(print, unixNow());
            logLine(h.address + ": login of \"" + device->name + "\"");
        } else if (window_ && Clock::now() < window_->until) {
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
        if (!window_ || Clock::now() >= window_->until) {
            reject("no-window");
            directFailure(peer->address, "no pairing window");
            return;
        }
        const auto request = parsePair(message);
        const std::string exporter = peer->ssl ? pairExporter(peer->ssl) : std::string();
        // The proof is keyed with the 16 bytes the code encodes.
        const auto key = base32Decode(window_->code);
        const bool good = request && key && !exporter.empty() && sameSecret(hmacHex(*key, exporter), request->proof);
        const unsigned long long waiter = window_->waiter;
        window_.reset();
        if (!good) {
            reject("bad-proof");
            directFailure(peer->address, "bad proof");
            notifyWaiter(waiter, {{"type", "pair-closed"}, {"reason", "bad-proof"}});
            return;
        }
        Devices(devicesPath()).add({peer->pairing, request->name, unixNow(), 0});
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
        const auto removed = Devices(devicesPath()).remove(message.value("device", ""));
        json names = json::array();
        for (const auto &device : removed) {
            names.push_back(device.name);
            logLine("revoked \"" + device.name + "\" " + device.fingerprint);
            for (auto &other : peers_)
                if (other->device == device.fingerprint) {
                    other->conn->closeAfterFlush();
                    dead_.push_back(other.get());
                }
        }
        peer->conn->send({{"type", "revoked"}, {"devices", names}});
        peer->conn->closeAfterFlush();
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
        for (auto &peer : peers_)
            if (!peer->pairing.empty() && peer->deadline && now >= *peer->deadline) {
                peer->deadline.reset();
                directFailure(peer->address, "pairing timed out");
                dead_.push_back(peer.get());
            }
        if (window_ && now >= window_->until) {
            notifyWaiter(window_->waiter, {{"type", "pair-closed"}, {"reason", "expired"}});
            window_.reset();
            logLine("pairing window closed");
        }
        lockout_.prune(now);
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
        if (window_)
            take(window_->until);
        return next;
    }


    int unixFd_;
    int tcpFd_;
    std::string token_ = token();
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<Peer *> dead_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    bool sessionsDirty_ = false;
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
    const int tcpFd = listenTcp("127.0.0.1", port);
    if (tcpFd < 0) {
        std::fprintf(stderr, "pvai daemon: cannot listen on 127.0.0.1:%d\n", port);
        return 1;
    }
    std::fprintf(stderr, "pvai daemon %s: %s and 127.0.0.1:%d\n", kVersion, daemonSocket().c_str(), port);
    return Daemon(unixFd, tcpFd).run();
}

} // namespace pvai
