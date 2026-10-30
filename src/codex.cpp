#include "codex.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace pvai {

namespace {

constexpr std::size_t kMaxMessage = 64 * 1024 * 1024;

std::string home()
{
    const char *h = std::getenv("HOME");
    return h ? h : "/";
}

bool executable(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

} // namespace

std::string codexBinary()
{
    // PVAI_CODEX names the binary, or with an empty value turns Codex off.
    if (const char *chosen = std::getenv("PVAI_CODEX"))
        return *chosen && executable(chosen) ? chosen : std::string();
    return findBinary("codex");
}

std::string codexSocket()
{
    return home() + "/.codex/app-server-control/app-server-control.sock";
}

CodexLink::~CodexLink()
{
    close();
}

bool CodexLink::open()
{
    close();
    const int fd = connectUnix(codexSocket());
    if (fd < 0)
        return false;
    // The key is fixed: the server is local and only checks the form.
    const std::string upgrade = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    // A server that accepts and then hangs must not hang the daemon.
    timeval limit{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
    std::string reply;
    if (writeAll(fd, upgrade)) {
        char c;
        while (reply.size() < 8192 && reply.find("\r\n\r\n") == std::string::npos && ::read(fd, &c, 1) == 1)
            reply += c;
    }
    if (reply.rfind("HTTP/1.1 101", 0) != 0) {
        ::close(fd);
        return false;
    }
    setNonBlocking(fd);
    setCloseOnExec(fd);
    fd_ = fd;
    nextId_ = 1;
    return true;
}

void CodexLink::close()
{
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    in_.clear();
    out_.clear();
    message_.clear();
}

long long CodexLink::request(const std::string &method, const json &params)
{
    const long long id = nextId_++;
    sendText(json({{"id", id}, {"method", method}, {"params", params}}).dump());
    return id;
}

void CodexLink::notify(const std::string &method, const json &params)
{
    json message = {{"method", method}};
    if (!params.is_null())
        message["params"] = params;
    sendText(message.dump());
}

void CodexLink::respond(const json &id, const json &result)
{
    sendText(json({{"id", id}, {"result", result}}).dump());
}

void CodexLink::respondError(const json &id, int code, const std::string &message)
{
    sendText(json({{"id", id}, {"error", {{"code", code}, {"message", message}}}}).dump());
}

void CodexLink::sendText(const std::string &payload)
{
    sendFrame(0x1, payload);
}

// A client frame: final, masked.
void CodexLink::sendFrame(unsigned char opcode, const std::string &payload)
{
    if (fd_ < 0)
        return;
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    const std::size_t n = payload.size();
    if (n < 126) {
        frame += static_cast<char>(0x80 | n);
    } else if (n < 65536) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>(n >> 8);
        frame += static_cast<char>(n & 0xff);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame += static_cast<char>((static_cast<unsigned long long>(n) >> shift) & 0xff);
    }
    const std::string mask = randomHex(4);
    unsigned char key[4];
    for (int i = 0; i < 4; ++i)
        key[i] = static_cast<unsigned char>(std::strtoul(mask.substr(2 * i, 2).c_str(), nullptr, 16));
    frame.append(reinterpret_cast<const char *>(key), 4);
    const std::size_t start = frame.size();
    frame += payload;
    for (std::size_t i = 0; i < n; ++i)
        frame[start + i] = static_cast<char>(frame[start + i] ^ key[i % 4]);
    out_ += frame;
}

bool CodexLink::read(std::vector<json> &messages)
{
    if (fd_ < 0)
        return false;
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(fd_, buffer, sizeof buffer);
        if (n > 0) {
            in_.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        return false;
    }
    for (;;) {
        if (in_.size() < 2)
            break;
        const auto b0 = static_cast<unsigned char>(in_[0]), b1 = static_cast<unsigned char>(in_[1]);
        std::size_t length = b1 & 0x7f, at = 2;
        if (length == 126) {
            if (in_.size() < 4)
                break;
            length = (static_cast<unsigned char>(in_[2]) << 8) | static_cast<unsigned char>(in_[3]);
            at = 4;
        } else if (length == 127) {
            if (in_.size() < 10)
                break;
            length = 0;
            for (int i = 2; i < 10; ++i)
                length = (length << 8) | static_cast<unsigned char>(in_[i]);
            at = 10;
        }
        const bool masked = b1 & 0x80;
        if (length > kMaxMessage)
            return false;
        if (in_.size() < at + (masked ? 4 : 0) + length)
            break;
        std::string payload = in_.substr(at + (masked ? 4 : 0), length);
        if (masked)
            for (std::size_t i = 0; i < length; ++i)
                payload[i] = static_cast<char>(payload[i] ^ in_[at + i % 4]);
        in_.erase(0, at + (masked ? 4 : 0) + length);
        const unsigned opcode = b0 & 0x0f;
        if (opcode == 0x8)
            return false;
        if (opcode == 0x9) {
            sendFrame(0xA, payload);
            continue;
        }
        if (opcode == 0xA)
            continue;
        message_ += payload;
        if (message_.size() > kMaxMessage)
            return false;
        if (!(b0 & 0x80))
            continue;
        json message = json::parse(message_, nullptr, false);
        message_.clear();
        if (!message.is_discarded())
            messages.push_back(std::move(message));
    }
    return true;
}

bool CodexLink::flush()
{
    while (!out_.empty() && fd_ >= 0) {
        const ssize_t n = ::write(fd_, out_.data(), out_.size());
        if (n > 0) {
            out_.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        return false;
    }
    return true;
}

} // namespace pvai
