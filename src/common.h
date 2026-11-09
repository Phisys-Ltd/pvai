// Paths, the token, sockets and the line-framed connection every part of pvai
// shares. Everything here works on Linux and macOS.
#pragma once

#include <nlohmann/json.hpp>

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pvai {

using json = nlohmann::json;

constexpr int kDefaultPort = 7470;
extern const char *const kVersion; // version.cpp, generated from version.cpp.in
constexpr std::size_t kMaxLine = 256 * 1024;
// Peers on the unix socket are the hooks and wrappers of this host: a hook
// event with a big tool input may be long.
constexpr std::size_t kMaxLocalLine = 4 * 1024 * 1024;
// What the hook sends of one string (a tool input, the last answer).
constexpr std::size_t kHookStringMax = 64 * 1024;
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
/// Claude Code's program: PVAI_CLAUDE (an empty value: none), else
/// findBinary("claude"). The daemon and `pvai run` both use it, so a program
/// the app installed under ~/.local/bin runs from a PATH without it.
std::string claudeBinary();
// The Claude token login (PROTOCOL.md, "Claude token login"): a long-lived
// token in the file `claude-token` of configDir(), mode 0600, which is only
// a fallback for a host where the account login is missing. The daemon
// decides (from `claude auth status --json` without the token) and keeps
// the decision in the empty marker file `claude-token-active` beside it; a
// `pvai run` in a terminal reads the marker, not the daemon. A
// CLAUDE_CODE_OAUTH_TOKEN that is already in the environment always wins and
// is never touched. No function here logs or prints a token.
constexpr const char *kEnvClaudeToken = "CLAUDE_CODE_OAUTH_TOKEN";
std::string claudeTokenPath();
std::string claudeTokenMarkerPath();
// Empty when the token is fine, else why not ("empty", "too-long",
// "bad-characters"): local syntax only, nothing is started.
std::string claudeTokenProblem(const std::string &token);
// The stored token, empty when there is no file or it holds nothing usable.
std::string readClaudeToken();
// Written through a 0600 temp file and renamed, so that nobody reads it
// half written or with a wider mode. False when it could not be stored.
bool storeClaudeToken(const std::string &token);
void removeClaudeToken();
// The fallback marker: created empty (0600) or removed; a no-op when it is
// that way already.
void setClaudeTokenMarker(bool active);
// The process environment holds CLAUDE_CODE_OAUTH_TOKEN (not empty).
bool inheritedClaudeToken();
// What `pvai run` adds to claude's environment at every (re)launch: the
// stored token when the marker and the file exist and the environment has
// no token of its own, else nothing.
std::vector<std::pair<std::string, std::string>> claudeTokenEnv();
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

// $HOME, "/" when it is not set.
std::string homeDir();

// Where one of a child's standard streams goes.
struct Stdio {
    enum class Kind { Inherit, Null, Fd, Append, SameAsOut };
    Kind kind = Kind::Inherit;
    int fd = -1;        // Kind::Fd: a descriptor of ours, duplicated onto the stream
    std::string path;   // Kind::Append: a file opened for appending, mode 0600
    static Stdio null() { return {Kind::Null, -1, {}}; }
    static Stdio descriptor(int fd) { return {Kind::Fd, fd, {}}; }
    static Stdio append(const std::string &path) { return {Kind::Append, -1, path}; }
    static Stdio sameAsOut() { return {Kind::SameAsOut, -1, {}}; }
};

struct ChildOptions {
    std::vector<std::string> argv;
    std::string exe;      // the program; empty: argv[0]
    bool search = false;  // look `exe` up in PATH (execvp)
    std::string cwd;      // empty: stay where we are
    int cwdFailure = 1;   // exit status of a child that cannot change there
    bool detach = false;  // fork twice, setsid: the child belongs to nobody; spawnChild waits for the middle one
    bool newSession = false; // setsid
    bool ownGroup = false;   // setpgid(0, 0), so that kill(-pid) reaches the child and its children
    bool dropEnv = true;     // dropClaudeEnvironment() first
    std::vector<std::pair<std::string, std::string>> env; // set after that
    Stdio in, out, err;
    // The child's terminal: a pseudo-terminal of this size, its master in
    // *ptyMaster; the streams above are not touched then.
    int ptyRows = 0, ptyCols = 0;
    int *ptyMaster = nullptr;
    const char *execFailure = nullptr; // perror() label of a child whose exec failed
};
// Forks and starts the child. The pid, 0 for a detached child that started,
// -1 when the fork failed (errno as fork left it). Does not wait, except for
// the middle process of a detached child.
pid_t spawnChild(const ChildOptions &options);
// exec of `exe` with `argv`; returns only when that failed, errno kept.
void execChild(const std::string &exe, const std::vector<std::string> &argv, bool search);
// Reads what a non-blocking `fd` has and appends it to `out` while that is
// below `cap` (the rest is dropped). True on end of file or a read error.
bool readAvailable(int fd, std::string &out, std::size_t cap = std::string::npos);

int connectUnix(const std::string &path); // blocking; -1 on failure
int listenUnix(const std::string &path);  // -1 on failure
int listenTcp(const std::string &address, int port);
void keepAlive(int fd); // SO_KEEPALIVE: a dead network path ends the connection
void setNonBlocking(int fd);
void setCloseOnExec(int fd);
bool writeAll(int fd, const std::string &data);
bool writeAll(int fd, const char *data, std::size_t size);
void sendLine(int fd, const json &message); // blocking, errors ignored
// A log above 8 MB moves to <path>.old before it is opened for appending
// (the runtime directory is RAM on Linux).
void rotateLog(const std::string &path);
enum class Written { Ok, WriteFailed, RenameFailed };
struct AtomicOptions {
    mode_t mode = 0600;           // of a new temp file (before the umask)
    std::string suffix = ".new";  // the temp file is path + suffix
    bool sync = true;             // fsync before the rename
    std::optional<mode_t> chmod;  // set on the temp file before the rename
};
// `data` to path + suffix, then renamed over `path`, so that nobody reads half
// a file. errno is the rename's after RenameFailed.
Written writeAtomically(const std::string &path, const std::string &data, const AtomicOptions &options = {});
// Cuts every string of a hook event's tool_input and its last_assistant_message
// to kHookStringMax (with a marker), so the event stays far below the line limit.
void cutHookEvent(json &event);

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
    std::size_t pending() const { return out_.size(); } // bytes not yet written
    void send(const json &message);
    // The wire form of a message (JSON and a newline), to dump once for many peers.
    static std::string line(const json &message);
    void sendRaw(const std::string &line);
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
