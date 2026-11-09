// Codex: its shared app server's threads are the sessions. Prompts become
// turns, answers to dialogs replies to the server's approval requests, and
// the server's notifications report back. No wrapper, no transcript file.
#pragma once

#include "codex.h"
#include "rpc_backend.h"
#include "session.h"

#include <deque>
#include <map>
#include <set>

namespace pvai {

class CodexBackend;

class CodexSession : public BufferedSession {
public:
    CodexSession(Host &host, CodexBackend &backend, const json &thread, bool detached);

    std::string assistant() const override { return "codex"; }
    // Threads this daemon started (or resumed) can be stopped, those of a
    // Codex TUI cannot. (The thread's originator is no help: it names
    // whichever client loaded it last.)
    bool detached() const override { return detached_; }
    void setDetached() { detached_ = true; changed(); }
    json describe() const override;
    // Not while a `pvai codex` TUI has it open: that is the owner's to end.
    bool stoppable() const override;

    std::string answer(const std::string &choice) override;
    std::string setMode(const std::string &mode, const json &ref) override;
    std::string setModel(const std::string &model, const json &ref) override;
    // The thread's settings (thread/start and resume answers, settings
    // updates): its mode from approval policy and sandbox, its model.
    void setSettings(const json &approval, const json &sandbox, const json &model);
    std::string stop() override;
    void interrupt() override;

    // From the app server.
    void onNotification(const std::string &method, const json &params);
    void onRequest(const json &requestId, const std::string &method, const json &params);
    void setOlderItems(const json &data);
    void setLimits(const json &limits);

protected:
    bool canSubmit() const override;
    void submit(const Prompt &prompt) override;

private:
    std::vector<json> entries(const json &item, long long ms);
    void reply(bool allow);
    void show(const json &request);
    void nextRequest();
    // A turn failed (turn/completed with an error, an `error` that is not a
    // retry, a rejected turn/start): a text entry, `say` and, for a login
    // problem, the "login" attention. `turn` may be empty (a rejection).
    void failTurn(const std::string &turn, const std::string &message, bool login);
    // The turn is over for good: nothing runs, the prompt is free again.
    void finishTurn();

    CodexBackend &backend_;
    bool detached_;
    std::string turnId_;       // the running turn
    bool turnPending_ = false; // turn/start sent, turn/started not seen yet
    json request_;             // the approval the thread waits on: id, method, params
    std::deque<json> queued_;  // approvals that came while one is shown, shown in turn
    std::map<std::string, std::string> outputs_; // command output by item id, while it runs
    std::map<std::string, std::string> files_;   // a file change's paths by item id, while it runs
    std::string failedTurn_;   // the turn failTurn reported last: its other report is no news
    std::string endedTurn_;    // the turn that completed last
    std::string answer_;       // the turn's last agent message so far
    std::string approval_;     // a new approval policy for the next turn/start
    json sandbox_;             // a new sandbox policy for it
    std::string model_;        // a new model for it
};

class CodexBackend : public RpcBackend {
public:
    explicit CodexBackend(Host &host);

    std::string name() const override { return "codex"; }
    std::string title() const override { return "Codex"; }
    json models() const override { return models_; }
    json modes() const override;
    void refreshLogin(std::function<void(bool changed)> done) override;
    void authChanged(const std::string &kind, bool ok) override;

    void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) override;
    void start(const StartRequest &request, Started done) override;
    void processStarted(unsigned long long id, const json &hello) override;
    void processEnded(unsigned long long id) override;

    void service() override;
    void tick(Clock::time_point now) override;
    std::optional<Clock::time_point> deadline() const override;

    // For its sessions.
    void markStopped(const std::string &thread) { stopped_.insert(thread); }
    bool heldByTui(const std::string &thread) const;
    // Lets go of a thread that is not ours (unsubscribes; not taken on again
    // while it stays loaded).
    void release(const std::string &thread);
    const json &limits() const { return limits_; }
    const std::string &serverVersion() const { return serverVersion_; }
    // The subagent threads of `thread` that are working now, oldest first.
    json subagentsOf(const std::string &thread) const;

protected:
    Link &link() override { return link_; }
    const Link &link() const override { return link_; }
    std::string locate() const override { return codexBinary(); }
    std::string endedText() const override { return "Codex app server ended"; }
    void onLost() override;
    void initialized(const json &result) override;

private:
    void rescan();
    // The account the app server says is logged in (account/read): the
    // email of a ChatGPT login, else null (an API key has no name).
    void readAccount(std::function<void(bool changed)> done);
    // Restarts the shared app server when an account change is waiting for
    // it and no thread works.
    void maybeRestart();
    void adopt(const std::string &id);
    CodexSession *addSession(const json &thread, bool detached);
    void setLimits(const json &limits);
    void listThreads(const std::string &cwd, bool archived, std::function<void(json)> done);
    void noteSubagent(const json &thread);
    bool subagentEvent(const std::string &method, const json &params);

    CodexLink link_;
    int openFailures_ = 0;
    Clock::time_point nextRescan_ = Clock::now();
    std::optional<Clock::time_point> lastStart_;
    json limits_ = {{"fiveHour", nullptr}, {"sevenDay", nullptr}};
    json models_ = json::array();
    std::string serverVersion_;
    bool serverAccount_ = false;   // the app server holds an account (it keeps it in memory)
    bool restartWanted_ = false;   // a login or logout changed what the server holds
    bool restarting_ = false;
    std::set<std::string> stopped_;  // threads stopped here: not adopted again
    std::set<std::string> adopting_; // thread/resume sent
    // Loaded subagent threads (parentThreadId set) by id: their parent, the
    // protocol entry and whether they work now.
    struct Subagent {
        std::string parent;
        json entry;
        bool active = false;
    };
    std::map<std::string, Subagent> subagents_;
    // TUIs started by `pvai codex`, and the threads that are theirs: the one
    // `codex resume` named, and those taken on while it was the only TUI in
    // the thread's folder. When a TUI ends, its threads leave.
    struct Tui {
        unsigned long long id = 0;
        std::string cwd;
        std::set<std::string> threads;
    };
    std::vector<Tui> tuis_;
    // Threads left when their TUI ended: not taken on again while they stay
    // loaded (another client may hold them); once unloaded, forgotten.
    std::set<std::string> released_;
};

} // namespace pvai
