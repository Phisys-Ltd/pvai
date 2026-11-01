// Direct access against a real daemon: pairing, login, a wrong device, a
// revoked one, an expired and a used code, a bad proof, and the lockout.
// Runs in its own HOME and runtime directory; each case dials from its own
// loopback address so the lockout of one does not spill into the next.
// Where there are no such addresses (macOS has only 127.0.0.1 on lo0), the
// daemon is restarted instead, which forgets the lockout, and the one check
// that needs two addresses is skipped.
//
// usage: pvai-direct-test /path/to/pvai
#include "pairing.h"
#include "tls.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

using namespace pvai;

namespace {

std::string g_pvai;
int g_port = 0;
int g_failed = 0;
bool g_aliases = true; // 127.0.0.2 and up can be bound
pid_t g_daemon = -1;

void startDaemon()
{
    g_daemon = ::fork();
    if (g_daemon == 0) {
        const std::string port = std::to_string(g_port + 1);
        ::execl(g_pvai.c_str(), "pvai", "daemon", "--port", port.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void stopDaemon()
{
    ::kill(g_daemon, SIGTERM);
    ::waitpid(g_daemon, nullptr, 0);
}

// Before a case that needs an address without failures behind it.
void freshAddress()
{
    if (g_aliases)
        return;
    stopDaemon();
    startDaemon();
}

bool canBind(unsigned source)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in from{};
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = htonl(0x7f000000u | source);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr *>(&from), sizeof from) == 0;
    ::close(fd);
    return ok;
}

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

std::string run(const std::string &args)
{
    std::string out;
    FILE *pipe = ::popen((g_pvai + " " + args + " 2>&1").c_str(), "r");
    if (!pipe)
        return out;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

struct Link {
    std::string fp, code;
    int port = 0;
};

Link parseLink(const std::string &link)
{
    Link out;
    auto field = [&](const std::string &key) {
        const auto at = link.find(key + "=");
        if (at == std::string::npos)
            return std::string();
        const auto end = link.find('&', at);
        return link.substr(at + key.size() + 1, end == std::string::npos ? std::string::npos : end - at - key.size() - 1);
    };
    out.fp = field("fp");
    out.code = field("code");
    out.port = std::atoi(field("port").c_str());
    return out;
}

struct TestDevice {
    std::string key, cert, fingerprint;
};

TestDevice newDevice(const std::string &name)
{
    TestDevice d;
    makeIdentity(name, d.key, d.cert);
    BIO *bio = BIO_new_mem_buf(d.cert.data(), static_cast<int>(d.cert.size()));
    X509 *x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    d.fingerprint = fingerprint(x);
    X509_free(x);
    BIO_free(bio);
    return d;
}

// A TLS client connection from 127.0.0.<source>.
struct Client {
    int fd = -1;
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    std::string serverFp;
    bool handshake = false;

    Client(const TestDevice &device, int source)
    {
        ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        static const unsigned char alpn[] = {6, 'p', 'v', 'a', 'i', '/', '1'};
        SSL_CTX_set_alpn_protos(ctx, alpn, sizeof alpn);
        BIO *kb = BIO_new_mem_buf(device.key.data(), static_cast<int>(device.key.size()));
        BIO *cb = BIO_new_mem_buf(device.cert.data(), static_cast<int>(device.cert.size()));
        EVP_PKEY *key = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
        X509 *cert = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
        SSL_CTX_use_certificate(ctx, cert);
        SSL_CTX_use_PrivateKey(ctx, key);
        X509_free(cert);
        EVP_PKEY_free(key);
        BIO_free(kb);
        BIO_free(cb);

        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in from{};
        from.sin_family = AF_INET;
        from.sin_addr.s_addr = htonl(0x7f000000u | static_cast<unsigned>(g_aliases ? source : 1));
        ::bind(fd, reinterpret_cast<sockaddr *>(&from), sizeof from);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<uint16_t>(g_port));
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof to) != 0)
            return;
        timeval limit{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        handshake = SSL_connect(ssl) == 1;
        if (handshake)
            serverFp = fingerprint(SSL_get0_peer_certificate(ssl));
        ERR_clear_error();
    }

    ~Client()
    {
        if (ssl)
            SSL_free(ssl);
        if (ctx)
            SSL_CTX_free(ctx);
        if (fd >= 0)
            ::close(fd);
    }

    bool sendLine(const std::string &line)
    {
        const std::string data = line + "\n";
        std::size_t n = 0;
        return handshake && SSL_write_ex(ssl, data.data(), data.size(), &n) == 1;
    }

    // One line, or empty after 3 s, an alert or the end.
    std::string readLine()
    {
        std::string line;
        char c;
        std::size_t n = 0;
        while (handshake && SSL_read_ex(ssl, &c, 1, &n) == 1) {
            if (c == '\n')
                return line;
            line += c;
        }
        ERR_clear_error();
        return {};
    }

    // Logged in: hello is answered with welcome.
    bool login()
    {
        if (!sendLine(R"({"type":"hello","client":"test","version":"0"})"))
            return false;
        const json answer = json::parse(readLine(), nullptr, false);
        if (!answer.is_object() || answer.value("type", "") != "welcome")
            return false;
        if (answer.value("user", "").empty()) // the user the daemon runs as
            return false;
        readLine(); // the session list and the proposals that follow
        readLine();
        return true;
    }

    std::string pair(const std::string &code, const std::string &name, bool spoil = false)
    {
        std::string proof = hmacHex(base32Decode(code).value_or(""), pairExporter(ssl));
        if (spoil)
            proof[0] = proof[0] == '0' ? '1' : '0';
        sendLine(json({{"type", "pair"}, {"name", name}, {"proof", proof}}).dump());
        return readLine();
    }
};

std::string typeOf(const std::string &line)
{
    const json message = json::parse(line, nullptr, false);
    return message.is_object() ? message.value("type", "") + (message.contains("reason") ? ":" + message.value("reason", "") : "")
                               : "(none)";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    g_pvai = std::filesystem::absolute(argv[1]).string();
    char home[] = "/tmp/pvai-test-XXXXXX";
    if (!::mkdtemp(home))
        return 2;
    const std::string runtime = std::string(home) + "/run";
    std::filesystem::create_directories(runtime);
    std::filesystem::permissions(runtime, std::filesystem::perms::owner_all);
    ::setenv("HOME", home, 1);
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    ::setenv("PVAI_CODEX", "", 1); // no Codex app server in the test's home
    g_port = 20000 + static_cast<int>(::getpid() % 20000);
    // PVAI_TEST_NO_ALIASES=1 takes the macOS way on Linux too.
    g_aliases = canBind(2) && !std::getenv("PVAI_TEST_NO_ALIASES");
    if (!g_aliases)
        std::printf("note: no 127.0.0.N aliases; the daemon is restarted between cases\n");
    startDaemon();

    const std::string listen = run("listen 127.0.0.1:" + std::to_string(g_port));
    check(listen.find("direct access on") != std::string::npos, "listen: " + listen.substr(0, 40));

    // A device pairs; the link's fingerprint is the server's.
    const TestDevice a = newDevice("phone-a"), b = newDevice("phone-b"), c = newDevice("phone-c");
    Link link = parseLink(run("pair --link --name testhost"));
    check(link.code.size() == 26 && link.port == g_port, "pair --link gives a code and the port");
    {
        Client client(a, 2);
        check(client.handshake && client.serverFp == link.fp, "the server's fingerprint is the link's");
        check(typeOf(client.pair(link.code, "Phone A")) == "paired", "pairing with the right proof");
    }
    {
        Client client(a, 2);
        check(client.login(), "the paired device logs in without a token");
        // Agent messages come only from this host, never over TLS.
        client.sendLine(R"({"type":"propose","ref":1,"to":"x","text":"t"})");
        client.sendLine(R"({"type":"ping"})");
        const std::string answer = client.readLine();
        check(typeOf(answer) == "pong", "propose over TLS is ignored (" + typeOf(answer) + ")");
    }
    check(run("devices").find("Phone A") != std::string::npos, "devices lists it");

    // The code is spent; another device with it gets nowhere.
    {
        Client client(b, 3);
        check(!client.login() && typeOf(client.pair(link.code, "B")) != "paired", "a used code does not pair again");
    }
    // An unknown device without a window fails in the handshake.
    {
        Client client(b, 4);
        check(!client.login(), "a device that is not paired is refused");
    }
    // A wrong proof closes the window.
    link = parseLink(run("pair --link"));
    {
        Client client(b, 5);
        check(typeOf(client.pair(link.code, "B", true)) == "rejected:bad-proof", "a wrong proof is rejected");
    }
    {
        Client client(b, 5);
        check(typeOf(client.pair(link.code, "B")) != "paired", "the window is closed after a wrong proof");
    }
    // A code that ran out.
    link = parseLink(run("pair --link --seconds 1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    {
        Client client(c, 6);
        check(typeOf(client.pair(link.code, "C")) != "paired", "an expired code does not pair");
    }
    // Revoked: refused from now on, and an open connection ends.
    freshAddress();
    {
        Client open(a, 7);
        check(open.login(), "logged in before the revoke");
        check(run("revoke \"Phone A\"").find("revoked: Phone A") != std::string::npos, "revoke by name");
        open.sendLine(R"({"type":"ping"})");
        const std::string after = open.readLine();
        check(after.empty(), "the revoked device's connection is closed" + (after.empty() ? std::string() : " (got " + after + ")"));
        Client again(a, 8);
        check(!again.login(), "the revoked device is refused");
    }
    // Lockout: five failures from one address, then even a good device from
    // there is dropped; from another address it gets in.
    freshAddress();
    link = parseLink(run("pair --link"));
    {
        Client client(c, 9);
        check(typeOf(client.pair(link.code, "C")) == "paired", "a second device pairs");
    }
    for (int i = 0; i < 5; ++i) {
        Client client(b, 50);
        client.login();
    }
    {
        Client client(c, 50);
        check(!client.login(), "an address with five failures is blocked");
    }
    if (g_aliases) {
        Client client(c, 51);
        check(client.login(), "another address is not");
    } else {
        std::printf("skip another address is not (no second loopback address)\n");
    }
    // Before login only one short line: a long one ends the connection.
    freshAddress();
    link = parseLink(run("pair --link"));
    {
        Client client(b, 60);
        client.sendLine(std::string(5000, 'x'));
        check(client.readLine().empty(), "an overlong line before login is dropped");
    }

    stopDaemon();
    std::filesystem::remove_all(home);
    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
