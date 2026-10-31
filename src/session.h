// What every kind of session and every assistant has in common. The daemon
// (daemon.cpp) holds the peers and routes the protocol; a Backend is one
// assistant on this host (Claude Code, Codex, ...), a Session one running
// conversation of it. A new assistant is one Backend subclass and its
// Session subclasses, registered in Daemon's constructor; no handler in
// the daemon names an assistant.
#pragma once

#include "common.h"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pvai {

using Clock = std::chrono::steady_clock;

struct Prompt {
    std::string text;
    bool voice = false;
    json ref; // the app's `ref`, for `delivered`
};

class Session;

// What backends and sessions need from the daemon.
class Host {
public:
    virtual ~Host() = default;
    virtual void broadcast(const json &message) = 0; // to every app
    virtual void sessionsChanged() = 0;              // a `sessions` list is due
    virtual void toSubscribers(const std::string &session, const json &message) = 0;
    // Live sessions only: an ended one is gone from here at once.
    virtual Session *session(const std::string &id) = 0;
    virtual std::vector<Session *> sessions() = 0;
    virtual void addSession(std::unique_ptr<Session> session) = 0;
    // A child process with stdin from /dev/null; `done` gets its exit status
    // (-1 when killed after `timeout`) and stdout and stderr together.
    virtual void runProcess(const std::vector<std::string> &argv, std::chrono::seconds timeout,
                            std::function<void(int status, const std::string &output)> done) = 0;
    virtual void log(const std::string &text) = 0;
};

class Session {
public:
    Session(Host &host, std::string id, std::string cwd, std::string label);
    virtual ~Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    const std::string &id() const { return id_; }
    const std::string &label() const { return label_; }
    const std::string &cwd() const { return cwd_; }
    const std::string &state() const { return state_; }
    void setLabel(const std::string &label);
    virtual std::string assistant() const = 0;
    // Started by the daemon (stop ends it), not in a terminal or TUI of the
    // owner's.
    virtual bool detached() const = 0;
    // `stop` can end it: what the daemon started, and what nobody else holds.
    virtual bool stoppable() const { return detached(); }
    // The entry in `sessions`.
    virtual json describe() const;

    // The app's requests. Each returns the reason for a `rejected`, or empty.
    // A prompt is queued and goes in when the session's prompt is free;
    // true when it went in at once.
    bool prompt(const Prompt &prompt);
    const std::deque<Prompt> &queued() const { return queue_; }
    // A typed prompt counts only once the assistant confirms it (`delivered`).
    virtual bool confirmsDelivery() const { return false; }
    virtual std::string answer(const std::string &choice) = 0;
    virtual std::string stop() = 0;
    virtual void interrupt() = 0;
    // Ends the session's process and starts it again, going on with the
    // same conversation (a newer installed version takes over); the session
    // keeps its id.
    virtual bool restartable() const { return false; }
    virtual std::string restart() { return "not-restartable"; }
    // Up to `limit` of the last conversation entries, oldest first.
    virtual json history(int limit) = 0;

    // Time-driven work (escape checks, transcript polling) and when it is
    // next due.
    virtual void tick(Clock::time_point) {}
    virtual std::optional<Clock::time_point> deadline() const { return std::nullopt; }

    // Gone from `sessions` at the end of the daemon's loop.
    void end();
    bool ended() const { return ended_; }

protected:
    // Types the oldest queued prompt when the session's prompt is free.
    bool deliver();
    virtual bool canSubmit() const { return true; }
    virtual void submit(const Prompt &prompt) = 0;

    // A turn ended: what to speak, to every app.
    void say(const std::string &answer);
    void attention(const std::string &kind, const std::string &text);
    // The whole history again to every subscriber (the conversation moved).
    void historyReset();
    void sendEntries(std::vector<json> entries);
    // The session list moved; a new state goes into the daemon's log.
    void changed();

    Host &host_;
    std::string id_;
    std::string cwd_;
    std::string label_;
    std::string state_ = "starting";
    std::string dialog_; // while waiting: "permission", "question" or "trust"
    json dialogInfo_;    // what the dialog is about (dialogInfo()); shown while dialog_ is set
    std::deque<Prompt> queue_;
    bool draft_ = false;     // the owner has typed into the prompt and not sent it
    bool voiceTurn_ = false; // the running turn's prompt was spoken
    json usage_;             // context window, rate limits, model, cost, version

private:
    bool ended_ = false;
    std::string loggedState_ = "starting";
};

// An ended conversation that `start` can resume.
struct Conversation {
    std::string id;
    std::string title;
    long long at = 0; // last activity, Unix time
};

struct StartRequest {
    std::string cwd; // an existing directory
    std::string label;
    std::vector<std::string> args;
    std::string resume; // a Conversation's id, or empty for a new one
};

class Backend {
public:
    // `done` gets the new session's id, or the reason for a `rejected`.
    using Started = std::function<void(const std::string &session, const std::string &reason)>;
    struct UpdateResult {
        std::string before, after; // versions
        std::string error;         // why the update failed
    };

    explicit Backend(Host &host);
    virtual ~Backend() = default;
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;

    virtual std::string name() const = 0;  // "claude", "codex"; `start.assistant`
    virtual std::string title() const = 0; // for the app: "Claude Code"
    virtual json models() const = 0;       // what `start` can ask for
    virtual bool available() const = 0;    // installed on this host, or sessions of it can come
    // `start` can start it: its program is on this host.
    virtual bool installedHere() const { return available(); }
    virtual json describe() const;         // the entry in `welcome.assistants`

    // The installed version, empty when unknown; read in the background.
    const std::string &installed() const { return installed_; }
    // Logged in on this host: true, false, or null when unknown.
    const json &loggedIn() const { return loggedIn_; }
    // Reads it again where the assistant can say (`done` with true when it
    // changed); after an install, looks for the program again.
    virtual void refreshLogin(std::function<void(bool changed)> done) { done(false); }
    virtual void findProgram() {}
    // The program `start` and a login run, empty when not installed.
    std::string program() const { return binary(); }
    void refreshVersion(std::function<void()> done = {});
    virtual void update(std::function<void(const UpdateResult &)> done);

    virtual void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) = 0;
    virtual void start(const StartRequest &request, Started done) = 0;

    // A process of this assistant that pvai started in the owner's terminal
    // (`pvai codex`) said hello; `id` stands for it until processEnded.
    virtual void processStarted(unsigned long long, const json &) {}
    virtual void processEnded(unsigned long long) {}

    // The daemon's loop: an fd to watch, its events, time-driven work.
    virtual int fd() const { return -1; }
    virtual short events() const { return 0; }
    virtual void service() {}
    virtual void tick(Clock::time_point) {}
    virtual std::optional<Clock::time_point> deadline() const { return std::nullopt; }

protected:
    virtual std::string binary() const = 0;
    virtual std::vector<std::string> updateArgs() const { return {"update"}; }

    Host &host_;
    std::string installed_;
    json loggedIn_;
    bool updating_ = false;
};

// Shared text helpers.
std::string trim(const std::string &s);
// The last line that starts with 🔊, without the marker.
std::string speakLine(const std::string &answer);
// The first sentence of an answer as plain text, at most about 200 bytes.
std::string firstSentence(const std::string &answer);
// Cut to `limit` bytes at a character boundary, with … when cut.
std::string cut(std::string text, std::size_t limit);
// One line of at most `limit` bytes: control characters dropped, trimmed.
std::string oneLine(const std::string &text, std::size_t limit);
std::string str(const json &object, const char *key);
std::string basename(const std::string &path);
std::string isoTime(long long ms);
// The first word of a `--version` output that starts with a digit.
std::string versionIn(const std::string &output);

extern const char *const kMicMark;   // 🎙 + space
extern const char *const kSpeakMark; // 🔊
constexpr std::size_t kMaxResult = 4 * 1024;
constexpr std::size_t kMaxText = 64 * 1024;
constexpr std::size_t kMaxConversations = 20;
constexpr std::size_t kTitleMax = 80;
constexpr std::size_t kDialogTextMax = 2000;

// `dialogInfo`: the tool a dialog is about (empty: none), what it would do
// (a command, a path, a URL, a question), the assistant's own words about
// it, and for a shell command its description; each cut to kDialogTextMax.
json dialogInfo(const std::string &tool, const std::string &text, const std::string &message,
                const std::string &description = {});

} // namespace pvai
