// The link to Codex's shared app server (`codex app-server`, started on
// demand as a per-user daemon): JSON-RPC over a WebSocket on its control
// socket, ~/.codex/app-server-control/app-server-control.sock. The Codex
// TUI runs its threads in the same server, so the daemon sees them too.
#pragma once

#include "common.h"
#include "rpc_backend.h"

#include <string>
#include <vector>

namespace pvai {

// The `codex` binary on this host, or empty.
std::string codexBinary();
std::string codexSocket();

class CodexLink : public Link {
public:
    CodexLink() = default;
    ~CodexLink() override;
    CodexLink(const CodexLink &) = delete;
    CodexLink &operator=(const CodexLink &) = delete;

    // Connects and upgrades to a WebSocket (blocking, a local socket);
    // false when no app server listens.
    bool open();
    void close() override;
    bool isOpen() const override { return fd_ >= 0; }
    int fd() const override { return fd_; }
    bool wantsWrite() const override { return !out_.empty(); }

    // Returns the request's id.
    long long request(const std::string &method, const json &params) override;
    void notify(const std::string &method, const json &params) override;
    void respond(const json &id, const json &result) override;
    void respondError(const json &id, int code, const std::string &message) override;

    // Reads what is there and appends whole messages. False on EOF or error.
    bool read(std::vector<json> &messages) override;
    bool flush() override;

private:
    void sendText(const std::string &payload);
    void sendFrame(unsigned char opcode, const std::string &payload);

    int fd_ = -1;
    long long nextId_ = 1;
    std::string in_;
    std::string out_;
    std::string message_; // a fragmented message so far
};

} // namespace pvai
