// What Codex and Kimi share: a backend that talks JSON-RPC to one program.
// Codex speaks WebSocket to the app server, Kimi ACP (one message per line)
// to a child it runs; each has its own Link. The bookkeeping is here: calls
// and their answers, the retry timer, what happens when the link is lost.
#pragma once

#include "session.h"

#include <map>

namespace pvai {

// The transport of one backend; the backend owns it.
class Link {
public:
    virtual ~Link() = default;
    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    virtual int fd() const = 0; // -1 when closed
    virtual bool wantsWrite() const = 0;
    // Returns the request's id.
    virtual long long request(const std::string &method, const json &params) = 0;
    virtual void notify(const std::string &method, const json &params) = 0;
    virtual void respond(const json &id, const json &result) = 0;
    virtual void respondError(const json &id, int code, const std::string &message) = 0;
    // Reads what is there and appends whole messages. False on EOF or error.
    virtual bool read(std::vector<json> &messages) = 0;
    virtual bool flush() = 0; // false on error
};

class RpcBackend : public Backend {
public:
    using Done = std::function<void(const json &result, const json &error)>;

    bool available() const override { return !bin_.empty(); }
    void findProgram() override;
    int fd() const override { return link().fd(); }
    short events() const override;

    // For its sessions.
    bool ready() const { return ready_; }
    // `done` gets the answer; at once with an error when the link is closed,
    // and with one when the link is lost before the answer comes.
    void call(const std::string &method, const json &params, Done done = {});
    void notify(const std::string &method, const json &params) { link().notify(method, params); }
    void respond(const json &id, const json &result) { link().respond(id, result); }
    void respondError(const json &id, int code, const std::string &message) { link().respondError(id, code, message); }

protected:
    explicit RpcBackend(Host &host);

    virtual Link &link() = 0;
    virtual const Link &link() const = 0;
    // The program, or empty.
    virtual std::string locate() const = 0;
    // For the calls that fail with the link: "Codex app server ended".
    virtual std::string endedText() const = 0;
    // The link is lost and its calls have been answered: what else goes,
    // the backend's sessions among it.
    virtual void onLost() {}
    // `initialize` was answered with an error (the link is dropped after).
    virtual void initializeFailed(const json &) {}
    // `initialize` went through; `ready()` is true.
    virtual void initialized(const json &) {}

    std::string binary() const override { return bin_; }
    // The link is gone (or is dropped): fails the open calls, lets
    // onLost() clear and end its sessions, and tries again in 2 s.
    void lost();
    // The answer to a call, when `message` is one (it has no `method`).
    void dispatchResponse(const json &message);
    // False until the next try is due; then true, and the next is a retry
    // interval away.
    bool retryDue(Clock::time_point now);
    // Sends `initialize`; ready() and initialized() follow its answer.
    void initialize(const json &params);
    // `message[key]` when it is an object, else an empty one: what an
    // assistant sends is its own to shape, a surprise costs that message.
    static const json &objectIn(const json &message, const char *key);

    template <class S>
    std::vector<S *> sessionsOf()
    {
        std::vector<S *> out;
        for (Session *session : host_.sessions())
            if (auto *mine = dynamic_cast<S *>(session))
                out.push_back(mine);
        return out;
    }

    static constexpr auto kRetry = std::chrono::seconds(10);

    std::string bin_;
    bool ready_ = false; // initialized
    std::map<long long, Done> pending_;
    Clock::time_point nextTry_ = Clock::now();
};

} // namespace pvai
