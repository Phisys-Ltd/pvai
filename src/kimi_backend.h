// Kimi Code: `kimi acp`, one child process of the daemon that speaks the
// Agent Client Protocol (JSON-RPC 2.0, one message per line) over a socket
// pair as its stdin and stdout. Its sessions are the daemon's (detached):
// prompts become session/prompt calls, dialogs its permission requests,
// and session/update notifications report back. Sessions a `kimi` TUI has
// open are locked by Kimi and not shared.
#pragma once

#include "rpc_backend.h"
#include "session.h"

#include <chrono>
#include <map>
#include <memory>
#include <sys/types.h>

#include <vector>

namespace pvai {

class KimiBackend;

// `kimi acp` on a socket pair: its stdin and stdout are one end, the daemon
// holds the other. Its stderr goes to kimi.log beside the daemon's log.
class KimiLink : public Link {
public:
    KimiLink() = default;
    ~KimiLink() override;

    bool open(const std::string &program);
    void close() override;
    bool isOpen() const override { return conn_ != nullptr; }
    int fd() const override { return conn_ ? conn_->fd() : -1; }
    bool wantsWrite() const override { return conn_ && conn_->wantsWrite(); }
    long long request(const std::string &method, const json &params) override;
    void notify(const std::string &method, const json &params) override;
    void respond(const json &id, const json &result) override;
    void respondError(const json &id, int code, const std::string &message) override;
    bool read(std::vector<json> &messages) override;
    bool flush() override { return !conn_ || conn_->flush(); }
    // Reaps the agents that were closed (SIGTERM sent), without waiting;
    // SIGKILL for one that stays more than 2 s.
    void reapClosed();
    bool hasClosed() const { return !closed_.empty(); }

private:
    struct Closed {
        pid_t pid;
        std::chrono::steady_clock::time_point killAt;
        bool killed = false;
    };
    std::vector<Closed> closed_;
    std::unique_ptr<Conn> conn_;
    pid_t child_ = -1;
    long long nextId_ = 1;
};

class KimiSession : public BufferedSession {
public:
    KimiSession(Host &host, KimiBackend &backend, const std::string &id, const std::string &cwd);

    std::string assistant() const override { return "kimi"; }
    bool detached() const override { return true; }

    std::string answer(const std::string &choice) override;
    std::string setMode(const std::string &mode, const json &ref) override;
    std::string setModel(const std::string &model, const json &ref) override;
    std::string stop() override;
    void interrupt() override;

    // From the agent.
    void onUpdate(const json &update);
    void onPermission(const json &requestId, const json &params);
    // A new or loaded session's model and context (session/new, load).
    void setConfig(const json &result);
    // session/load has played the history back: what follows is live.
    void loaded();

protected:
    bool canSubmit() const override;
    void submit(const Prompt &prompt) override;
    bool quiet() const override { return replaying_; }

private:
    void flushText();
    void reply(const std::string &optionId); // empty: cancelled
    void turnEnded(const json &result, const json &error);

    KimiBackend &backend_;
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
    std::string promptId_; // the entry id of the turn's prompt
};

class KimiBackend : public RpcBackend {
public:
    explicit KimiBackend(Host &host);

    std::string name() const override { return "kimi"; }
    std::string title() const override { return "Kimi Code"; }
    json models() const override { return models_; }
    json modes() const override { return modes_; }
    void update(std::function<void(const UpdateResult &)> done) override;
    // A login just went through: the next call tells for sure.
    void setLoggedIn(bool on) { loggedIn_ = on; }

    void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) override;
    void start(const StartRequest &request, Started done) override;

    void service() override;
    void tick(Clock::time_point now) override;
    std::optional<Clock::time_point> deadline() const override;

    // For its sessions.
    const std::string &agentVersion() const { return agentVersion_; }
    // A call failed with authRequired (-32000), or one went through.
    void noteAuth(const json &error);

protected:
    Link &link() override { return link_; }
    const Link &link() const override { return link_; }
    std::string locate() const override;
    std::string endedText() const override { return "Kimi Code ended"; }
    void onLost() override;
    void initializeFailed(const json &error) override;
    void initialized(const json &result) override;

private:
    void onRequest(const json &id, const std::string &method, const json &params);
    KimiSession *addSession(const std::string &id, const std::string &cwd);
    void setModels(const json &result);
    void setModes(const json &result);

    KimiLink link_;
    json models_ = json::array();
    // Kimi's modes until a session tells better (modes.ts in kimi-code).
    json modes_ = json::array({{{"id", "default"}, {"name", "Ask"}}, {{"id", "plan"}, {"name", "Plan"}},
                               {{"id", "auto"}, {"name", "Auto"}}, {{"id", "yolo"}, {"name", "YOLO"}}});
    std::string agentVersion_;
};

} // namespace pvai
