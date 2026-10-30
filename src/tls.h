// Direct access over TLS 1.3 (docs/PROTOCOL.md, "Direct access"): the
// daemon's identity, fingerprints, the server context that checks client
// certificates in the handshake, and the stream a finished handshake hands
// on. OpenSSL 3.
#pragma once

#include "common.h"

#include <functional>
#include <memory>
#include <string>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct x509_st X509;
typedef struct x509_store_ctx_st X509_STORE_CTX;

namespace pvai {

// The SHA-256 of a certificate's SubjectPublicKeyInfo (DER), in base32.
std::string fingerprint(X509 *certificate);

// The daemon's Ed25519 key and self-signed certificate in ~/.config/pvai,
// made on first use. Empty fingerprint when they cannot be made.
struct Identity {
    std::string keyPath;
    std::string certPath;
    std::string fingerprint;
};
Identity serverIdentity();

// Makes a fresh Ed25519 key and self-signed certificate as PEM (for tests
// and for the server identity).
bool makeIdentity(const std::string &commonName, std::string &keyPem, std::string &certPem);

class TlsStream : public Stream {
public:
    TlsStream(int fd, SSL *ssl); // takes both
    ~TlsStream() override;
    int fd() const override { return fd_; }
    long read(char *buffer, std::size_t size) override;
    long write(const char *data, std::size_t size) override;
    SSL *ssl() const { return ssl_; }

private:
    int fd_;
    SSL *ssl_;
};

// What a handshake of an accepted socket comes to.
class TlsAccept {
public:
    enum class Step { Again, Done, Failed };

    TlsAccept(SSL_CTX *context, int fd); // takes the fd
    ~TlsAccept();
    int fd() const { return fd_; }
    // Poll events the handshake waits for.
    short events() const { return events_; }
    Step step();
    const std::string &error() const { return error_; }
    // After Done: the client's fingerprint, and the stream (taken once).
    const std::string &peerFingerprint() const { return peer_; }
    std::unique_ptr<TlsStream> take();

private:
    int fd_;
    SSL *ssl_;
    short events_;
    std::string peer_;
    std::string error_;
};

// The server side: TLS 1.3 only, ALPN pvai/1, a client certificate required.
// `admit` decides in the handshake whether a client fingerprint may connect.
class TlsServer {
public:
    explicit TlsServer(std::function<bool(const std::string &fingerprint)> admit);
    ~TlsServer();
    TlsServer(const TlsServer &) = delete;
    TlsServer &operator=(const TlsServer &) = delete;

    bool ready() const { return context_ != nullptr; }
    const std::string &fingerprint() const { return identity_.fingerprint; }
    SSL_CTX *context() const { return context_; }

private:
    static int verify(int ok, X509_STORE_CTX *store);

    Identity identity_;
    SSL_CTX *context_ = nullptr;
    std::function<bool(const std::string &)> admit_;
};

// RFC 8446 7.5 exporter output for pairing: 32 bytes, label
// EXPORTER-pvai-pair, empty context. Empty on failure.
std::string pairExporter(SSL *ssl);
// HMAC-SHA256 over `message` with `key`, as lowercase hex.
std::string hmacHex(const std::string &key, const std::string &message);

} // namespace pvai
