#include "tls.h"

#include "pairing.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace pvai {

namespace {

const unsigned char kAlpn[] = {6, 'p', 'v', 'a', 'i', '/', '1'};
const char *kPairLabel = "EXPORTER-pvai-pair";

std::string lastError()
{
    char text[256];
    const unsigned long code = ERR_get_error();
    if (code == 0)
        return "unknown";
    ERR_error_string_n(code, text, sizeof text);
    ERR_clear_error();
    return text;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

bool writeSecret(const std::string &path, const std::string &data, mode_t mode)
{
    const std::string temp = path + ".new";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    const bool ok = writeAll(fd, data) && ::fsync(fd) == 0;
    ::close(fd);
    return ok && ::rename(temp.c_str(), path.c_str()) == 0;
}

X509 *certFromPem(const std::string &pem)
{
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509 *cert = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(bio);
    return cert;
}

int selectAlpn(SSL *, const unsigned char **out, unsigned char *outLength, const unsigned char *in,
               unsigned int inLength, void *)
{
    unsigned char *chosen = nullptr;
    if (SSL_select_next_proto(&chosen, outLength, kAlpn, sizeof kAlpn, in, inLength) != OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    *out = chosen;
    return SSL_TLSEXT_ERR_OK;
}

} // namespace

std::string fingerprint(X509 *certificate)
{
    if (!certificate)
        return {};
    unsigned char *der = nullptr;
    const int length = i2d_PUBKEY(X509_get0_pubkey(certificate), &der);
    if (length <= 0)
        return {};
    unsigned char digest[32];
    unsigned int digestLength = 0;
    const bool ok = EVP_Digest(der, static_cast<std::size_t>(length), digest, &digestLength, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    return ok ? base32(digest, digestLength) : std::string();
}

bool makeIdentity(const std::string &commonName, std::string &keyPem, std::string &certPem)
{
    EVP_PKEY *key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    X509 *cert = X509_new();
    bool ok = key && cert;
    if (ok) {
        unsigned char serial[16];
        RAND_bytes(serial, sizeof serial);
        serial[0] &= 0x7f;
        BIGNUM *number = BN_bin2bn(serial, sizeof serial, nullptr);
        ok = X509_set_version(cert, 2) && number && BN_to_ASN1_INTEGER(number, X509_get_serialNumber(cert))
            && X509_gmtime_adj(X509_getm_notBefore(cert), -86400)
            && X509_gmtime_adj(X509_getm_notAfter(cert), 100L * 365 * 86400);
        BN_free(number);
        X509_NAME *name = X509_get_subject_name(cert);
        ok = ok
            && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                          reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0)
            && X509_set_issuer_name(cert, name) && X509_set_pubkey(cert, key) && X509_sign(cert, key, nullptr) > 0;
    }
    if (ok) {
        BIO *k = BIO_new(BIO_s_mem()), *c = BIO_new(BIO_s_mem());
        ok = k && c && PEM_write_bio_PrivateKey(k, key, nullptr, nullptr, 0, nullptr, nullptr)
            && PEM_write_bio_X509(c, cert);
        if (ok) {
            char *data = nullptr;
            long n = BIO_get_mem_data(k, &data);
            keyPem.assign(data, static_cast<std::size_t>(n));
            n = BIO_get_mem_data(c, &data);
            certPem.assign(data, static_cast<std::size_t>(n));
        }
        BIO_free(k);
        BIO_free(c);
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

Identity serverIdentity()
{
    Identity identity{configDir() + "/server.key", configDir() + "/server.crt", {}};
    struct stat st;
    if (::stat(identity.keyPath.c_str(), &st) != 0 || ::stat(identity.certPath.c_str(), &st) != 0) {
        std::string key, cert;
        if (!makeIdentity("pvai " + hostName(), key, cert) || !writeSecret(identity.keyPath, key, 0600)
            || !writeSecret(identity.certPath, cert, 0644))
            return identity;
    }
    X509 *cert = certFromPem(readFile(identity.certPath));
    identity.fingerprint = fingerprint(cert);
    X509_free(cert);
    return identity;
}

TlsStream::TlsStream(int fd, SSL *ssl)
    : fd_(fd)
    , ssl_(ssl)
{
}

TlsStream::~TlsStream()
{
    SSL_shutdown(ssl_); // a close_notify if it fits; no waiting for the answer
    SSL_free(ssl_);
    ::close(fd_);
}

long TlsStream::read(char *buffer, std::size_t size)
{
    std::size_t n = 0;
    if (SSL_read_ex(ssl_, buffer, size, &n) == 1)
        return static_cast<long>(n);
    switch (SSL_get_error(ssl_, 0)) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        return kWouldBlock;
    case SSL_ERROR_ZERO_RETURN:
        return 0;
    default:
        ERR_clear_error();
        return kError;
    }
}

long TlsStream::write(const char *data, std::size_t size)
{
    std::size_t n = 0;
    if (SSL_write_ex(ssl_, data, size, &n) == 1)
        return static_cast<long>(n);
    switch (SSL_get_error(ssl_, 0)) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        return kWouldBlock;
    default:
        ERR_clear_error();
        return kError;
    }
}

TlsAccept::TlsAccept(SSL_CTX *context, int fd)
    : fd_(fd)
    , ssl_(SSL_new(context))
    , events_(POLLIN)
{
    setNonBlocking(fd_);
    setCloseOnExec(fd_);
    if (ssl_)
        SSL_set_fd(ssl_, fd_);
}

TlsAccept::~TlsAccept()
{
    if (ssl_)
        SSL_free(ssl_);
    if (fd_ >= 0)
        ::close(fd_);
}

TlsAccept::Step TlsAccept::step()
{
    if (!ssl_) {
        error_ = "no TLS session";
        return Step::Failed;
    }
    const int result = SSL_accept(ssl_);
    if (result == 1) {
        peer_ = fingerprint(SSL_get0_peer_certificate(ssl_));
        if (peer_.empty()) {
            error_ = "no client certificate";
            return Step::Failed;
        }
        return Step::Done;
    }
    switch (SSL_get_error(ssl_, result)) {
    case SSL_ERROR_WANT_READ:
        events_ = POLLIN;
        return Step::Again;
    case SSL_ERROR_WANT_WRITE:
        events_ = POLLOUT;
        return Step::Again;
    default:
        error_ = lastError();
        return Step::Failed;
    }
}

std::unique_ptr<TlsStream> TlsAccept::take()
{
    auto stream = std::make_unique<TlsStream>(fd_, ssl_);
    fd_ = -1;
    ssl_ = nullptr;
    return stream;
}

TlsServer::TlsServer(std::function<bool(const std::string &)> admit)
    : identity_(serverIdentity())
    , admit_(std::move(admit))
{
    if (identity_.fingerprint.empty())
        return;
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    if (!context)
        return;
    const bool ok = SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION)
        && SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION)
        && SSL_CTX_use_certificate_chain_file(context, identity_.certPath.c_str()) == 1
        && SSL_CTX_use_PrivateKey_file(context, identity_.keyPath.c_str(), SSL_FILETYPE_PEM) == 1
        && SSL_CTX_check_private_key(context) == 1;
    if (!ok) {
        SSL_CTX_free(context);
        return;
    }
    // No resumption: every connection shows its certificate again.
    SSL_CTX_set_num_tickets(context, 0);
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_alpn_select_cb(context, selectAlpn, nullptr);
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify);
    SSL_CTX_set_app_data(context, this);
    context_ = context;
}

TlsServer::~TlsServer()
{
    if (context_)
        SSL_CTX_free(context_);
}

// Client certificates are self-signed: what counts is only whether the
// leaf's key is one the daemon knows (or a pairing window is open).
int TlsServer::verify(int, X509_STORE_CTX *store)
{
    if (X509_STORE_CTX_get_error_depth(store) > 0)
        return 1;
    SSL *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    const auto *server = ssl ? static_cast<TlsServer *>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl))) : nullptr;
    const std::string print = pvai::fingerprint(X509_STORE_CTX_get_current_cert(store));
    return server && !print.empty() && server->admit_(print) ? 1 : 0;
}

std::string pairExporter(SSL *ssl)
{
    unsigned char out[32];
    if (SSL_export_keying_material(ssl, out, sizeof out, kPairLabel, std::strlen(kPairLabel), nullptr, 0, 0) != 1)
        return {};
    return std::string(reinterpret_cast<const char *>(out), sizeof out);
}

std::string hmacHex(const std::string &key, const std::string &message)
{
    unsigned char out[32];
    std::size_t length = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(),
                   reinterpret_cast<const unsigned char *>(message.data()), message.size(), out, sizeof out, &length))
        return {};
    static const char *hex = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < length; ++i) {
        text += hex[out[i] >> 4];
        text += hex[out[i] & 15];
    }
    return text;
}

} // namespace pvai
