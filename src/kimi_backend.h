// Kimi Code: `kimi acp`, one child process of the daemon that speaks the
// Agent Client Protocol (JSON-RPC 2.0, one message per line) over a socket
// pair as its stdin and stdout. Its sessions are the daemon's (detached):
// prompts become session/prompt calls, dialogs its permission requests,
// and session/update notifications report back. Sessions a `kimi` TUI has
// open are locked by Kimi and not shared.
#pragma once

#include "session.h"

#include <map>
#include <memory>
#include <sys/types.h>

namespace pvai {

class KimiBackend;

class KimiSession : public Session {
public:
    KimiSession(Host &host, KimiBackend &backend, const std::string &id, const std::string &cwd);

    std::string assistant() const override { return "kimi"; }
    bool detached() const override { return true; }

    std::string answer(const std::string &choice) override;
    std::string stop() override;
    void interrupt() override;
    json history(int limit) override;

    // From the agent.
    void onUpdate(const json &update);
    void onPermission(const json &requestId, const json &params);
    // A new or loaded session's model and context (session/new, load).
    void setConfig(const json &result);
    void setVersion(const std::string &version);
    // session/load has played the history back: what follows is live.
    void loaded();

protected:
    bool canSubmit() const override;
    void submit(const Prompt &prompt) override;

private:
    void flushText();
    void pushEntries(std::vector<json> fresh);
    void reply(const std::string &optionId); // empty: cancelled
    void turnEnded(const json &result, const json &error);
    void usageChanged();

    KimiBackend &backend_;
    std::deque<json> entries_;
    bool replaying_ = true;   // session/load is playing the history back
    bool voiceSent_ = false;  // the voice context went with a prompt
    // The message being streamed: who writes it, its text so far, when.
    std::string chunkKind_; // "prompt" or "text"
    std::string chunk_;
    std::string chunkTime_;
    int chunkCount_ = 0;
    std::string answer_; // the turn's last agent message
    std::map<std::string, std::string> tools_; // toolCallId -> tool name, until it ends
    json request_;     // the permission request the session waits on: id, params
    bool turnRunning_ = false;
};

class KimiBackend : public Backend {
public:
    explicit KimiBackend(Host &host);
    ~KimiBackend() override;

    std::string name() const override { return "kimi"; }
    std::string title() const override { return "Kimi Code"; }
    json models() const override { return models_; }
    bool available() const override { return !bin_.empty(); }
    json describe() const override;
    void update(std::function<void(const UpdateResult &)> done) override;

    void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) override;
    void start(const StartRequest &request, Started done) override;

    int fd() const override;
    short events() const override;
    void service() override;
    void tick(Clock::time_point now) override;
    std::optional<Clock::time_point> deadline() const override;

    // For its sessions.
    using Done = std::function<void(const json &result, const json &error)>;
    bool ready() const { return ready_; }
    void call(const std::string &method, const json &params, Done done = {});
    void notify(const std::string &method, const json &params);
    void respond(const json &id, const json &result);
    const std::string &agentVersion() const { return agentVersion_; }
    // A call failed with authRequired (-32000), or one went through.
    void noteAuth(const json &error);

protected:
    std::string binary() const override { return bin_; }

private:
    bool spawn();
    void lost();
    void onRequest(const json &id, const std::string &method, const json &params);
    KimiSession *addSession(const std::string &id, const std::string &cwd);
    void setModels(const json &result);

    std::string bin_;
    std::unique_ptr<Conn> conn_;
    pid_t child_ = -1;
    bool ready_ = false;
    long long nextId_ = 1;
    std::map<long long, Done> pending_;
    Clock::time_point nextTry_ = Clock::now();
    json models_ = json::array();
    std::string agentVersion_;
    json loggedIn_; // null until known
};

} // namespace pvai
