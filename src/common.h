// Paths, the token, sockets and the line-framed connection every part of pvai
// shares. Everything here works on Linux and macOS.
#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace pvai {

using json = nlohmann::json;

constexpr int kDefaultPort = 7470;
constexpr const char *kVersion = PVAI_VERSION;
constexpr std::size_t kMaxLine = 256 * 1024;
constexpr std::size_t kMaxPrompt = 32 * 1024;

// The wrapper's environment for Claude Code and so for its hooks.
constexpr const char *kEnvId = "PVAI_ID";
constexpr const char *kEnvSocket = "PVAI_SOCK";

std::string runtimeDir(); // created with mode 0700 when missing
std::string daemonSocket();
std::string configDir();  // ~/.config/pvai, created when missing
std::string token();      // created on first use, mode 0600
std::string selfExe();
std::string hostName();   // short, without the domain
std::string userName();   // the user the process runs as
std::string randomHex(std::size_t bytes);
// `name` in PATH, ~/.local/bin, /usr/local/bin or /opt/homebrew/bin (the
// daemon may run with a bare PATH); empty when there is none.
std::string findBinary(const std::string &name);
// The convention for spoken turns, given to every session as context:
// Claude Code gets it from the SessionStart hook, Codex as developer
// instructions.
extern const char *const kVoiceContext;

// Before a detached process starts: it is no child of whatever Claude Code
// session ran the command that started it.
void dropClaudeEnvironment();
// Starts `argv` in `cwd` as its own session, stdin and output on /dev/null,
// without the Claude Code environment; does not wait for it.
void spawnDetached(const std::string &cwd, const std::vector<std::string> &argv);

int connectUnix(const std::string &path); // blocking; -1 on failure
int listenUnix(const std::string &path);  // -1 on failure
int listenTcp(const std::string &address, int port);
void setNonBlocking(int fd);
void setCloseOnExec(int fd);
bool writeAll(int fd, const std::string &data);
void sendLine(int fd, const json &message); // blocking, errors ignored

// The bytes under a connection: a plain socket, or TLS on one (tls.h).
// How a connection is set up (accept, handshake, pairing) is not its
// business; it only moves bytes.
class Stream {
public:
    static constexpr long kWouldBlock = -1;
    static constexpr long kError = -2;

    virtual ~Stream() = default;
    virtual int fd() const = 0;
    // > 0 bytes moved, 0 end of stream (read only), kWouldBlock, kError.
    virtual long read(char *buffer, std::size_t size) = 0;
    virtual long write(const char *data, std::size_t size) = 0;
};

class PlainStream : public Stream {
public:
    explicit PlainStream(int fd); // takes the fd
    ~PlainStream() override;
    int fd() const override { return fd_; }
    long read(char *buffer, std::size_t size) override;
    long write(const char *data, std::size_t size) override;

private:
    int fd_;
};

// A non-blocking stream that speaks one JSON object per line.
class Conn {
public:
    explicit Conn(int fd);
    explicit Conn(std::unique_ptr<Stream> stream);
    Conn(const Conn &) = delete;
    Conn &operator=(const Conn &) = delete;

    int fd() const { return stream_->fd(); }
    // Lines longer than this end the connection (kMaxLine by default).
    void setMaxLine(std::size_t limit) { maxLine_ = limit; }
    bool wantsWrite() const { return !out_.empty(); }
    void send(const json &message);
    // Reads what is there and appends whole lines; a line that is not JSON
    // becomes a null value. False on EOF, error or an overlong line.
    bool readMessages(std::vector<json> &messages);
    bool flush(); // false on error
    void closeAfterFlush() { closing_ = true; }
    bool closing() const { return closing_; }

private:
    std::unique_ptr<Stream> stream_;
    std::size_t maxLine_ = kMaxLine;
    std::string in_;
    std::string out_;
    bool closing_ = false;
};

} // namespace pvai
