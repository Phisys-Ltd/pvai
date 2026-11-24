// Sharing one session with a guest (docs/PROTOCOL.md, "Sharing a session")
// against a real daemon: the invitation link, a guest's own device pairing,
// its welcome, what it may and may not ask, the events it gets, `from` on
// its prompts (also after a daemon restart), dialogs on and off with the
// first answer winning, revoke, expiry, a conversation that moved (confirm),
// a session that ended (move to the same conversation under another id), and
// the clear message when direct access is off. Fake wrappers stand in for
// Claude Code sessions; the guests are TLS clients with their own certificates.
//
// usage: pvai-share-test /path/to/pvai
#include "inbox.h"
#include "pairing.h"
#include "session.h"
#include "support.h"
#include "tls.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>

using namespace pvai;
using namespace pvai::test;
namespace fs = std::filesystem;

namespace {

std::string g_pvai;
int g_port = 0;
bool g_aliases = true;

bool canBind(unsigned source)
{
    const int fd = cloexecSocket(AF_INET);
    sockaddr_in from{};
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = htonl(0x7f000000u | source);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr *>(&from), sizeof from) == 0;
    ::close(fd);
    return ok;
}

void writeFile(const std::string &path, const std::string &text)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

void appendFile(const std::string &path, const std::string &text) { std::ofstream(path, std::ios::app | std::ios::binary) << text; }

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A transcript line of Claude Code: a prompt of the user, stamped `ms` (now when 0).
std::string userLine(const std::string &text, long long ms = 0, const std::string &uuid = "")
{
    return json({{"type", "user"}, {"uuid", uuid.empty() ? "u" + randomHex(4) : uuid}, {"timestamp", isoTime(ms ? ms : nowMs())},
                 {"message", {{"role", "user"}, {"content", text}}}}).dump() + "\n";
}

std::string pvaiRun(const std::string &args, int *status = nullptr)
{
    const RunResult r = runPvai(g_pvai, args);
    if (status)
        *status = r.status;
    return r.out;
}

struct Link {
    std::string scheme, host, fp, code, name, session;
    int port = 0;
};

std::string percentDecode(const std::string &text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// The pvai:// link anywhere in a command's output.
Link parseLink(const std::string &out)
{
    Link link;
    const auto start = out.find("pvai://");
    if (start == std::string::npos)
        return link;
    auto end = out.find_first_of(" \r\n", start);
    const std::string text = out.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const auto colon = text.find("://"), question = text.find('?');
    link.scheme = text.substr(colon + 3, question - colon - 3);
    auto field = [&](const std::string &key) {
        const auto at = text.find((question != std::string::npos && text[question + 1] != 0 ? "?" : "&") + key + "=");
        const auto alt = text.find("&" + key + "=");
        const auto from = at != std::string::npos ? at : alt;
        if (from == std::string::npos)
            return std::string();
        const auto valueAt = from + key.size() + 2;
        const auto stop = text.find('&', valueAt);
        return percentDecode(text.substr(valueAt, stop == std::string::npos ? std::string::npos : stop - valueAt));
    };
    link.host = field("host");
    link.fp = field("fp");
    link.code = field("code");
    link.name = field("name");
    link.session = field("session");
    link.port = std::atoi(field("port").c_str());
    return link;
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

// A guest's TLS connection (non-blocking after the handshake, read with poll).
struct Tls {
    int fd = -1;
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    bool handshake = false;
    bool closed = false;
    std::string buffer;
    std::string serverFp;

    Tls(const TestDevice &device, int source)
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

        fd = cloexecSocket(AF_INET);
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
        if (handshake) {
            serverFp = fingerprint(SSL_get0_peer_certificate(ssl));
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
        ERR_clear_error();
    }
    ~Tls()
    {
        if (ssl)
            SSL_free(ssl);
        if (ctx)
            SSL_CTX_free(ctx);
        if (fd >= 0)
            ::close(fd);
    }
    Tls(const Tls &) = delete;
    Tls &operator=(const Tls &) = delete;

    bool writeRaw(const std::string &data)
    {
        std::size_t at = 0;
        while (handshake && at < data.size()) {
            std::size_t n = 0;
            if (SSL_write_ex(ssl, data.data() + at, data.size() - at, &n) == 1) {
                at += n;
                continue;
            }
            const int error = SSL_get_error(ssl, 0);
            if (error != SSL_ERROR_WANT_WRITE && error != SSL_ERROR_WANT_READ)
                return false;
            pollfd p{fd, static_cast<short>(error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
            ::poll(&p, 1, 2000);
        }
        return handshake;
    }
    bool send(const json &message) { return writeRaw(message.dump() + "\n"); }

    // Reads what is there until `want` returns true for the buffer or `ms` is over.
    template <class Done> bool fill(Done done, int ms)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (done())
                return true;
            if (closed || !handshake)
                return false;
            if (!SSL_pending(ssl)) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
                pollfd p{fd, POLLIN, 0};
                if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0)
                    return done();
            }
            char chunk[16384];
            std::size_t n = 0;
            if (SSL_read_ex(ssl, chunk, sizeof chunk, &n) == 1) {
                buffer.append(chunk, n);
            } else {
                const int error = SSL_get_error(ssl, 0);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                    continue;
                closed = true;
                ERR_clear_error();
            }
        }
    }

    // The next message within `ms`; an empty object when none came.
    json next(int ms = 3000)
    {
        if (!fill([&] { return buffer.find('\n') != std::string::npos; }, ms))
            return json::object();
        const std::string line = buffer.substr(0, buffer.find('\n'));
        buffer.erase(0, line.size() + 1);
        return json::parse(line, nullptr, false);
    }
    json next(const std::string &type, const std::function<bool(const json &)> &pred = {}, int ms = 3000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            const json m = next(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()));
            if (m.empty())
                break;
            if (m.is_object() && m.value("type", "") == type && (!pred || pred(m)))
                return m;
        }
        return json::object();
    }
    // Everything that arrives within `ms`.
    std::vector<json> collect(int ms)
    {
        std::vector<json> out;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            const json m = next(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()));
            if (m.empty())
                break;
            out.push_back(m);
        }
        return out;
    }
    void drain(int ms = 200) { collect(ms); }
    std::string readBytes(std::size_t n, int ms = 5000)
    {
        fill([&] { return buffer.size() >= n; }, ms);
        const std::string out = buffer.substr(0, std::min(n, buffer.size()));
        buffer.erase(0, out.size());
        return out;
    }
    // The other end has closed (within `ms`); what is still unread is dropped.
    bool atEnd(int ms = 3000)
    {
        buffer.clear();
        fill([&] { return false; }, ms);
        return closed;
    }

    // hello; the welcome, or an empty object. The sessions and proposals that follow are read too.
    json hello(json *sessions = nullptr)
    {
        send({{"type", "hello"}, {"client", "share-test"}, {"version", "0"}});
        const json welcome = next("welcome");
        const json list = next("sessions");
        if (sessions)
            *sessions = list;
        next("proposals", {}, 500);
        return welcome;
    }

    std::string pair(const std::string &code, const std::string &name, bool spoil = false)
    {
        std::string proof = hmacHex(base32Decode(code).value_or(""), pairExporter(ssl));
        if (spoil)
            proof[0] = proof[0] == '0' ? '1' : '0';
        send({{"type", "pair"}, {"name", name}, {"proof", proof}});
        const json answer = next(3000);
        return answer.is_object() && !answer.empty() ? answer.value("type", "") + (answer.contains("reason") ? ":" + answer.value("reason", "") : "") : "(none)";
    }
};

json devicesFile() { return json::parse(readFile(configDir() + "/devices.json"), nullptr, false); }

json deviceNamed(const std::string &name)
{
    for (const auto &d : devicesFile())
        if (d.value("name", "") == name)
            return d;
    return json();
}

std::vector<LineClient *> g_wrappers; // fake wrappers, alive to the end (or until reset)

LineClient *startSession(const std::string &id, const std::string &cwd, const std::string &conversation, const std::string &transcript,
                         bool running = false)
{
    auto *wrapper = new LineClient;
    json registration = {{"type", "register"}, {"version", kVersion}, {"id", id}, {"cwd", cwd}, {"label", id}};
    if (running)
        registration["running"] = true;
    wrapper->send(registration);
    json event = {{"hook_event_name", "SessionStart"}, {"session_id", conversation}, {"source", "startup"}, {"transcript_path", transcript}};
    LineClient hookClient;
    hookClient.send({{"type", "hook"}, {"id", id}, {"event", event}});
    hookClient.next(300);
    g_wrappers.push_back(wrapper);
    return wrapper;
}

void hook(const std::string &id, json event)
{
    LineClient c;
    c.send({{"type", "hook"}, {"id", id}, {"event", std::move(event)}});
    c.next(300);
}

json sessionIn(const json &message, const std::string &id)
{
    if (message.is_object() && message.contains("sessions"))
        for (const auto &s : message["sessions"])
            if (s.value("id", "") == id)
                return s;
    return json::object();
}

std::string reasonOf(const json &m) { return m.value("type", "") == "rejected" ? m.value("reason", "") : "(" + m.dump() + ")"; }

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    g_pvai = fs::absolute(argv[1]).string();
    const TestHome testHome;
    char realBuf[PATH_MAX];
    const std::string home = ::realpath(testHome.path.c_str(), realBuf) ? realBuf : testHome.path;
    ::setenv("PVAI_CODEX", "", 1);
    ::unsetenv(kEnvId);
    g_port = 20000 + static_cast<int>(::getpid() % 20000);
    g_aliases = canBind(2) && !std::getenv("PVAI_TEST_NO_ALIASES");

    const std::string proj = home + "/proj";
    fs::create_directories(proj);
    writeFile(proj + "/notes.txt", "notes of the project");
    writeFile(proj + "/outside-for-wb.txt", "wb only");
    const std::string trA = home + "/tr-wa.jsonl", trA2 = home + "/tr-wa2.jsonl", trB = home + "/tr-wb.jsonl";
    for (const auto &t : {trA, trA2, trB, home + "/tr-wm1.jsonl", home + "/tr-wm2.jsonl", home + "/tr-wend.jsonl", home + "/tr-wgone.jsonl"})
        writeFile(t, "");

    DaemonProcess daemon;
    auto startDaemon = [&] { daemon.start(g_pvai, g_port + 1, 500, true, {{"PVAI_CODEX", ""}, {"PVAI_SHARE_GRACE", "4"}}); };
    startDaemon();

    // The owner's app, on the unix socket (an owner).
    LineClient app;
    app.send({{"type", "hello"}, {"client", "share-test-owner"}, {"version", "0"}});
    const json ownerWelcome = app.next("welcome");
    check(ownerWelcome.value("share", false), "an owner's welcome has share: true: " + ownerWelcome.dump().substr(0, 120));

    startSession("wa", proj, "c-wa", trA);
    startSession("wb", proj, "c-wb", trB);
    app.next("sessions", [](const json &m) { return !sessionIn(m, "wa").empty() && !sessionIn(m, "wb").empty(); }, 4000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    LineClient *wrapperA = g_wrappers[0], *wrapperB = g_wrappers[1];

    // ---- Without direct access there is no dead link.
    {
        int status = 0;
        const std::string out = pvaiRun("share wa", &status);
        check(status != 0 && out.find("direct access is off; turn it on with pvai listen ADDR") != std::string::npos
                  && out.find("pvai://") == std::string::npos,
              "share without direct access fails clearly: " + out);
        LineClient c;
        c.send({{"type", "share"}, {"ref", 7}, {"session", "wa"}});
        const json answer = c.next("rejected");
        check(answer.value("reason", "") == "not-listening" && answer.value("ref", 0) == 7, "the request: rejected not-listening: " + answer.dump());
    }
    const std::string listen = pvaiRun("listen 127.0.0.1:" + std::to_string(g_port));
    check(listen.find("direct access on") != std::string::npos, "listen: " + listen.substr(0, 40));

    // ---- The invitation.
    {
        int status = 0;
        const std::string out = pvaiRun("share nosuch --link", &status);
        check(status != 0 && out.find("unknown-session") != std::string::npos, "an unknown session: " + out);
        check(pvaiRun("share wa --ttl banana", &status).find("--ttl") != std::string::npos && status == 2, "a bad --ttl is refused");
    }
    const std::string full = pvaiRun("share wa --name Hausbox");
    check(full.find("Warning:") != std::string::npos && full.find("would trust with your account") != std::string::npos
              && full.find("run as your user") != std::string::npos,
          "the CLI output carries the warning about the owner's rights");
    Link link = parseLink(pvaiRun("share wa --link --name Hausbox"));
    check(link.scheme == "share" && link.host == "127.0.0.1" && link.port == g_port && link.fp.size() > 40 && link.code.size() == 26
              && link.name == "Hausbox" && link.session == "wa",
          "the link: pvai://share with host, port, fp, code, name, session: " + link.host + " " + std::to_string(link.port) + " " + link.name
              + " " + link.session);
    check(pvaiRun("share wa --link").find("pvai: warning: ") != std::string::npos, "--link still prints the warning (on stderr)");
    {
        int status = 0;
        const std::string list = pvaiRun("share list", &status);
        check(status == 0 && list.find("invitation") != std::string::npos && list.find("wa") != std::string::npos
                  && list.find("expires in 7 days") != std::string::npos,
              "share list shows the open invitations with the default 7 days: " + list.substr(0, 200));
    }
    // The three invitations above are all open; the one with the link parsed is used below.

    // ---- A guest pairs with its own certificate.
    const TestDevice devA = newDevice("guest-a"), devB = newDevice("guest-b"), devC = newDevice("guest-c"), devD = newDevice("guest-d"),
                     devE = newDevice("guest-e"), devX = newDevice("passer-by");
    {
        Tls wrong(devX, 3);
        check(wrong.handshake && wrong.serverFp == link.fp, "the daemon's fingerprint is the link's");
        check(wrong.pair(link.code, "x", true) == "rejected:bad-proof", "a wrong proof is rejected");
    }
    {
        Tls guest(devA, 4);
        check(guest.pair(link.code, "Anna phone") == "paired", "the guest pairs (a wrong proof before did not spend the invitation)");
    }
    {
        Tls again(devX, 5);
        check(again.pair(link.code, "late") != "paired", "a used code does not pair again");
    }
    const json entry = deviceNamed("Anna phone");
    check(!entry.empty() && entry.value("fingerprint", "") == devA.fingerprint, "devices.json holds the guest by its device's name");
    const json stored = entry.value("guest", json());
    check(stored.value("session", "") == "wa" && stored.value("assistant", "") == "claude" && stored.value("conversation", "") == "c-wa"
              && stored.value("dialogs", true) == false && stored.value("state", "") == "active" && stored["expires"].is_number()
              && std::llabs(stored["expires"].get<long long>() - (std::time(nullptr) + 7 * 86400)) < 60,
          "the guest entry: session, assistant, conversation at grant, expiry 7 days, dialogs off, active: " + stored.dump());
    {
        struct stat st;
        check(::stat((configDir() + "/devices.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "devices.json is 0600");
    }
    check(pvaiRun("devices").find("guest of session wa (active)") != std::string::npos, "pvai devices marks the guest");

    // ---- The guest's welcome and what it sees.
    Tls guestA(devA, 6);
    json sessionsA;
    const json welcomeA = guestA.hello(&sessionsA);
    const json g = welcomeA.value("guest", json());
    check(welcomeA.value("type", "") == "welcome" && !welcomeA.contains("share") && g.value("session", "") == "wa"
              && g.value("assistant", "") == "claude" && g.value("dialogs", true) == false && g.value("state", "") == "active"
              && g["expires"].is_number() && g["rights"].is_array(),
          "the guest welcome: guest {session, assistant, rights, dialogs, expires, state}: " + g.dump());
    check(std::find(g["rights"].begin(), g["rights"].end(), json("prompt")) != g["rights"].end()
              && std::find(g["rights"].begin(), g["rights"].end(), json("answer")) == g["rights"].end(),
          "rights name prompt, and no answer while dialogs are off: " + g["rights"].dump());
    check(welcomeA.value("conversations", true) == false && welcomeA.value("peers", true) == false && welcomeA.value("forget", true) == false,
          "no conversations, peers or forget for a guest");
    bool accountSeen = false;
    for (const auto &a : welcomeA["assistants"])
        accountSeen = accountSeen || !a.value("account", json()).is_null();
    check(!accountSeen, "the owner's accounts are not in a guest's welcome");
    check(sessionsA["sessions"].size() == 1 && !sessionIn(sessionsA, "wa").empty(), "sessions: only its session: " + sessionsA.dump().substr(0, 100));

    // ---- Allowed and denied.
    guestA.send({{"type", "ping"}});
    check(!guestA.next("pong").empty(), "ping");
    guestA.send({{"type", "subscribe"}, {"session", "wa"}, {"limit", 50}});
    check(!guestA.next("history").empty(), "subscribe to its session: history");
    guestA.send({{"type", "subscribe"}, {"ref", 1}, {"session", "wb"}});
    check(reasonOf(guestA.next("rejected")) == "guest-denied", "subscribe to another session: guest-denied");
    guestA.send({{"type", "prompt"}, {"ref", 2}, {"session", "wb"}, {"text", "hello"}});
    const json denied = guestA.next("rejected");
    check(reasonOf(denied) == "guest-denied" && denied.value("ref", 0) == 2 && denied.value("session", "") == "wb",
          "a prompt to another session: guest-denied, with ref and session: " + denied.dump());
    // A guest's prompt is plain text: no agent-message header, whatever it claims; no commands.
    guestA.send({{"type", "prompt"}, {"ref", 3}, {"session", "wa"}, {"text", "Wie spaet ist es?"}, {"from", "evil/w1"}, {"fromLabel", "x"},
                 {"proposal", "p1"}});
    check(!guestA.next("accepted").empty(), "a prompt to its session is accepted");
    const json typed = wrapperA->next("type");
    check(typed.value("text", "") == "Wie spaet ist es?", "typed as the guest wrote it, no [Message ...] header: " + typed.dump());
    guestA.send({{"type", "prompt"}, {"ref", 4}, {"session", "wa"}, {"text", "/exit"}});
    check(reasonOf(guestA.next("rejected")) == "guest-denied", "a slash command from a guest: guest-denied");
    guestA.send({{"type", "prompt"}, {"ref", 5}, {"session", "wa"}, {"text", "! rm -rf x"}});
    check(reasonOf(guestA.next("rejected")) == "guest-denied", "a shell command from a guest: guest-denied");
    {
        int refused = 0;
        std::string wrong;
        const json other = {{"session", "wa"}};
        const std::vector<const char *> types = {"start", "stop", "restart", "forget", "rename", "mode", "model", "effort", "clearInput", "install", "login",
                                 "logout", "set-token", "login-code", "login-cancel", "peers", "propose", "update", "projects",
                                 "browse", "conversations", "dismiss", "pair", "listen", "revoke", "share", "shares", "share-revoke",
                                 "share-confirm", "share-move", "answer", "shutdown", "listPeers", "something-new"};
        for (const char *type : types) {
            json request = other;
            request["type"] = type;
            request["ref"] = "d";
            request["cwd"] = "/";
            request["path"] = "/";
            guestA.send(request);
            const json answer = guestA.next("rejected");
            if (reasonOf(answer) == "guest-denied")
                ++refused;
            else
                wrong += std::string(type) + "=" + answer.dump() + " ";
        }
        check(refused == static_cast<int>(types.size()), "every other request: guest-denied (" + std::to_string(refused) + "/" + std::to_string(types.size()) + ") " + wrong);
    }
    // The turn of that prompt ends; then the session is idle again.
    hook("wa", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "c-wa"}, {"prompt", "Wie spaet ist es?"}});
    hook("wa", {{"hook_event_name", "Stop"}, {"session_id", "c-wa"}, {"last_assistant_message", "Done in wa.\n\xF0\x9F\x94\x8A Fertig in a."}});
    check(!guestA.next("say").empty(), "the guest gets its session's say");
    guestA.send({{"type", "interrupt"}, {"ref", 6}, {"session", "wa"}});
    check(!guestA.next("interrupting").empty(), "interrupt its session");

    // A prompt with a file: upload into the session's inbox, over a second connection.
    {
        const std::string body = std::string(20000, 'u') + "end";
        Tls up(devA, 6);
        up.hello();
        up.send({{"type", "upload"}, {"ref", "u1"}, {"session", "wb"}, {"name", "x.txt"}, {"size", body.size()}, {"sha256", bytesSha256(body)}});
        check(reasonOf(up.next("rejected")) == "guest-denied" && up.atEnd(), "upload into another session: guest-denied, connection closed");
        Tls up2(devA, 6);
        up2.hello();
        up2.send({{"type", "upload"}, {"ref", "u2"}, {"session", "wa"}, {"name", "x.txt"}, {"size", body.size()}, {"sha256", bytesSha256(body)}});
        up2.writeRaw(body);
        const json done = up2.next("uploaded");
        check(done.value("session", "") == "wa" && fs::exists(done.value("path", "")) && readFile(done.value("path", "")) == body,
              "upload into its session's inbox: " + done.dump());
        guestA.send({{"type", "prompt"}, {"ref", 7}, {"session", "wa"}, {"text", "see the file"}, {"files", json::array({done.value("path", "")})}});
        check(!guestA.next("accepted").empty(), "a prompt with that file");
        const json typedFile = wrapperA->next("type");
        check(typedFile.value("text", "").find("see the file") == 0 && typedFile.value("text", "").find(done.value("path", "")) != std::string::npos,
              "the file path follows the text: " + typedFile.dump());
        hook("wa", {{"hook_event_name", "UserPromptSubmit"}, {"session_id", "c-wa"}, {"prompt", typedFile.value("text", "")}});
        hook("wa", {{"hook_event_name", "Stop"}, {"session_id", "c-wa"}, {"last_assistant_message", "ok"}});
        guestA.drain();
    }
    // fetch: its session's reach, not another session's.
    {
        Tls f(devA, 6);
        f.hello();
        f.send({{"type", "fetch"}, {"ref", "f1"}, {"session", "wa"}, {"path", proj + "/notes.txt"}});
        const json header = f.next("fetch");
        check(header.value("size", 0) == 20 && f.readBytes(20) == "notes of the project", "fetch within its session: " + header.dump());
        Tls f2(devA, 6);
        f2.hello();
        f2.send({{"type", "fetch"}, {"ref", "f2"}, {"session", "wb"}, {"path", proj + "/notes.txt"}});
        check(reasonOf(f2.next("rejected")) == "guest-denied" && f2.atEnd(), "fetch for another session: guest-denied");
        Tls f3(devA, 6);
        f3.hello();
        f3.send({{"type", "fetch"}, {"ref", "f3"}, {"session", "wa"}, {"path", home + "/.config/pvai/devices.json"}});
        check(reasonOf(f3.next("rejected")) == "not-allowed", "pvai's own configuration stays protected for a guest");
        Tls d(devA, 6);
        d.hello();
        d.send({{"type", "download"}, {"ref", "d1"}, {"proposal", "p1"}, {"file", "f1"}});
        check(reasonOf(d.next("rejected")) == "guest-denied" && d.atEnd(), "download of a proposal: guest-denied");
    }

    // ---- The events a guest gets are its session's.
    for (const char *id : {"wb", "wa"}) {
        const std::string conversation = std::string("c-") + id;
        hook(id, {{"hook_event_name", "UserPromptSubmit"}, {"session_id", conversation}, {"prompt", "go"}});
        hook(id, {{"hook_event_name", "Stop"}, {"session_id", conversation}, {"last_assistant_message", std::string("Done in ") + id + ".\n\xF0\x9F\x94\x8A Fertig."}});
    }
    LineClient proposer;
    proposer.send({{"type", "propose"}, {"ref", 1}, {"to", "testhost/wa"}, {"text", "from b"}, {"from", "wb"}});
    proposer.next("proposed");
    {
        const std::vector<json> events = guestA.collect(1200);
        bool sayA = false, foreign = false, proposals = false, wbList = false;
        for (const auto &m : events) {
            const std::string type = m.value("type", "");
            if (type == "say" && m.value("session", "") == "wa")
                sayA = true;
            if (m.contains("session") && m["session"].is_string() && m["session"] != "wa")
                foreign = true;
            if (type == "proposals" || type == "assistants" || type == "attention" || type == "install" || type == "login")
                proposals = true;
            if (type == "sessions" && !sessionIn(m, "wb").empty())
                wbList = true;
            if (type == "sessions" && m["sessions"].size() > 1)
                wbList = true;
        }
        check(sayA, "the guest gets its session's say");
        check(!foreign && !proposals && !wbList, "and nothing of another session, no proposals, no assistants");
    }

    // ---- `from` on a guest's prompt, and not on an owner's with the same words.
    // The history so far: an owner's own prompt with the same words, typed earlier in the terminal.
    appendFile(trA, userLine("same words", nowMs() - 120000, "u-owner-1"));
    app.send({{"type", "subscribe"}, {"session", "wa"}, {"limit", 100}});
    app.next("history");
    guestA.send({{"type", "prompt"}, {"ref", 8}, {"session", "wa"}, {"text", "same words"}});
    guestA.next("accepted");
    wrapperA->next("type");
    appendFile(trA, userLine("Wie spaet ist es?", nowMs() - 1000, "u-guest-0")); // the first guest prompt, written after it was typed
    appendFile(trA, userLine("same words", nowMs() + 50, "u-guest-1"));
    hook("wa", {{"hook_event_name", "Stop"}, {"session_id", "c-wa"}, {"last_assistant_message", "ok"}});
    json fromEntry;
    {
        std::map<std::string, json> byId;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < until && !(byId.count("u-guest-1") && byId.count("u-guest-0"))) {
            const json m = app.next(500);
            if (m.value("type", "") == "entry" && m["entry"].value("kind", "") == "prompt")
                byId[m["entry"].value("id", "")] = m["entry"];
        }
        check(byId.count("u-guest-1") && byId["u-guest-1"].value("from", "") == "Anna phone",
              "the live entry of the guest's prompt has from = the device's name: " + (byId.count("u-guest-1") ? byId["u-guest-1"].dump() : "(none)"));
    }
    app.send({{"type", "subscribe"}, {"session", "wa"}, {"limit", 100}});
    {
        const json history = app.next("history");
        std::map<std::string, std::string> fromOf;
        for (const auto &e : history["entries"])
            if (e.value("kind", "") == "prompt")
                fromOf[e.value("id", "")] = e.value("from", "-");
        check(fromOf["u-owner-1"] == "-", "the owner's earlier prompt with the same words has no from: " + history.dump().substr(0, 300));
        check(fromOf["u-guest-1"] == "Anna phone", "history: the guest's prompt has from");
        check(fromOf["u-guest-0"] == "Anna phone", "history: the first guest prompt (matched by text and time) has from");
    }
    {
        // A guest's own history shows it too.
        guestA.send({{"type", "subscribe"}, {"session", "wa"}});
        const json history = guestA.next("history");
        bool found = false;
        for (const auto &e : history["entries"])
            found = found || (e.value("id", "") == "u-guest-1" && e.value("from", "") == "Anna phone");
        check(found, "the guest sees from in its history");
    }
    {
        const std::string file = configDir() + "/guest-prompts.json";
        struct stat st;
        const std::string content = readFile(file);
        check(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "guest-prompts.json is 0600");
        const json parsed = json::parse(content, nullptr, false);
        check(parsed.is_object() && parsed.contains("c-wa") && parsed["c-wa"].size() >= 3 && content.find("same words") == std::string::npos
                  && content.find("Wie spaet") == std::string::npos,
              "guest-prompts.json: keyed by conversation, hashes and no text: " + content.substr(0, 200));
    }

    // ---- Dialogs off: the guest sees none and cannot answer; the owner answers, the guest hears who.
    hook("wa", {{"hook_event_name", "PermissionRequest"}, {"session_id", "c-wa"}, {"tool_name", "Bash"}, {"tool_input", {{"command", "git push"}}}});
    {
        const json waiting = app.next("sessions", [](const json &m) { return sessionIn(m, "wa").value("dialog", json()) == "permission"; });
        check(!waiting.empty(), "the owner sees the dialog");
        const std::vector<json> events = guestA.collect(800);
        bool attention = false, dialogShown = false, listed = false;
        for (const auto &m : events) {
            if (m.value("type", "") == "attention")
                attention = true;
            if (m.value("type", "") == "sessions" && !sessionIn(m, "wa").empty()) {
                listed = true;
                dialogShown = dialogShown || !sessionIn(m, "wa").value("dialog", json()).is_null() || !sessionIn(m, "wa").value("dialogInfo", json()).is_null();
            }
        }
        check(!attention && listed && !dialogShown, "a guest without dialogs gets no attention and no dialog in the list");
        guestA.send({{"type", "answer"}, {"ref", 9}, {"session", "wa"}, {"choice", "allow"}});
        check(reasonOf(guestA.next("rejected")) == "guest-denied", "answer with dialogs off: guest-denied");
    }
    app.send({{"type", "answer"}, {"ref", 10}, {"session", "wa"}, {"choice", "allow"}});
    check(!app.next("answered").empty(), "the owner answers");
    {
        const json by = app.next("dialog-answered");
        check(by.value("session", "") == "wa" && by.value("answeredBy", "") == "owner" && by.value("choice", "") == "allow",
              "dialog-answered: answeredBy owner: " + by.dump());
        const json forGuest = guestA.next("dialog-answered");
        check(forGuest.value("answeredBy", "") == "owner", "the guest hears it too");
    }

    // ---- Dialogs on: a second guest; the first answer wins.
    link = parseLink(pvaiRun("share wa --dialogs --link"));
    {
        Tls guest(devB, 7);
        check(guest.pair(link.code, "Bert tablet") == "paired", "a second guest pairs (dialogs on)");
    }
    Tls guestB(devB, 8);
    const json welcomeB = guestB.hello();
    check(welcomeB["guest"].value("dialogs", false) && std::find(welcomeB["guest"]["rights"].begin(), welcomeB["guest"]["rights"].end(), json("answer"))
                                                          != welcomeB["guest"]["rights"].end(),
          "dialogs on: the welcome says so and rights include answer: " + welcomeB["guest"].dump());
    guestB.send({{"type", "subscribe"}, {"session", "wa"}});
    guestB.next("history");
    hook("wa", {{"hook_event_name", "PermissionRequest"}, {"session_id", "c-wa"}, {"tool_name", "Bash"}, {"tool_input", {{"command", "ls"}}}});
    check(!guestB.next("attention").empty() && !app.next("attention").empty(), "the dialog reaches owner and guest");
    guestB.drain();
    guestB.send({{"type", "answer"}, {"ref", 11}, {"session", "wa"}, {"choice", "allow"}});
    check(!guestB.next("answered").empty(), "the guest answers first");
    check(wrapperA->next("keys").value("data", "") == "\r", "its allow is typed");
    {
        const json by = app.next("dialog-answered");
        check(by.value("answeredBy", "") == "Bert tablet", "dialog-answered carries the guest's name: " + by.dump());
        app.send({{"type", "answer"}, {"ref", 12}, {"session", "wa"}, {"choice", "allow"}});
        check(reasonOf(app.next("rejected")) == "no-dialog", "the owner, second: the existing stale-dialog behaviour (no-dialog)");
    }
    app.send({{"type", "subscribe"}, {"session", "wa"}, {"limit", 100}});
    {
        const json history = app.next("history");
        bool answered = false;
        for (const auto &e : history["entries"])
            answered = answered || (e.value("kind", "") == "answer" && e.value("answeredBy", "") == "Bert tablet" && e.value("text", "") == "allow");
        check(answered, "the history has the answer entry (kind answer, answeredBy)");
    }

    // ---- Revoke closes the guest's connection at once.
    {
        int status = 0;
        const std::string out = pvaiRun("share revoke \"Bert tablet\"", &status);
        check(status == 0 && out.find("revoked: Bert tablet") != std::string::npos, "share revoke: " + out);
        check(guestB.atEnd(), "its connection is closed");
        Tls again(devB, 9);
        check(!again.hello().is_object() || again.hello().empty(), "and it cannot log in again");
        check(deviceNamed("Bert tablet").empty(), "the entry is gone from devices.json");
        check(pvaiRun("share revoke nobody", &status).find("unknown-share") != std::string::npos && status == 1, "revoking an unknown share fails");
        check(!deviceNamed("Anna phone").empty(), "the other guest stays");
    }

    // ---- A conversation that moved (/clear inside the running session) needs the owner's word.
    hook("wa", {{"hook_event_name", "SessionStart"}, {"session_id", "c-wa-2"}, {"source", "clear"}, {"transcript_path", trA2}});
    check(guestA.next("share-state", [](const json &m) { return m.value("state", "") == "scope-moved"; }, 3000).value("session", "") == "wa",
          "the live guest gets a notice: share-state scope-moved");
    guestA.send({{"type", "prompt"}, {"ref", 13}, {"session", "wa"}, {"text", "hello again"}});
    check(reasonOf(guestA.next("rejected")) == "scope-moved", "requests are refused: scope-moved");
    guestA.send({{"type", "subscribe"}, {"session", "wa"}});
    check(reasonOf(guestA.next("rejected")) == "scope-moved", "also a subscribe");
    guestA.drain();
    hook("wa", {{"hook_event_name", "Stop"}, {"session_id", "c-wa-2"}, {"last_assistant_message", "in the new conversation"}});
    check(guestA.next("say", {}, 800).empty(), "no events of the new conversation reach the guest");
    check(deviceNamed("Anna phone")["guest"].value("state", "") == "scope-moved", "the state is kept in devices.json");
    check(pvaiRun("share list").find("scope-moved") != std::string::npos, "share list shows it");
    {
        int status = 0;
        check(pvaiRun("share confirm \"Nobody\"", &status).find("unknown-share") != std::string::npos && status == 1, "confirm: unknown share");
        const std::string out = pvaiRun("share confirm \"Anna phone\"", &status);
        check(status == 0 && out.find("confirmed:") != std::string::npos, "share confirm: " + out);
        check(pvaiRun("share confirm \"Anna phone\"", &status).find("not-scope-moved") != std::string::npos && status == 1, "confirm needs scope-moved");
    }
    check(guestA.next("share-state", [](const json &m) { return m.value("state", "") == "active"; }, 3000).value("state", "") == "active",
          "the guest is told it is active again");
    check(deviceNamed("Anna phone")["guest"].value("conversation", "") == "c-wa-2", "the new conversation id is bound");
    guestA.send({{"type", "prompt"}, {"ref", 14}, {"session", "wa"}, {"text", "after the confirm"}});
    check(!guestA.next("accepted").empty(), "a prompt works again");
    wrapperA->next("type");

    // ---- Expiry: a timer closes the live connection; a later login is told.
    link = parseLink(pvaiRun("share wb --ttl 2s --link"));
    {
        Tls guest(devC, 10);
        check(guest.pair(link.code, "Cara") == "paired", "a short share pairs");
    }
    {
        Tls guestC(devC, 11);
        const json welcome = guestC.hello();
        check(welcome["guest"]["expires"].is_number() && welcome["guest"].value("session", "") == "wb", "its welcome carries expires");
        check(guestC.next("share-state", [](const json &m) { return m.value("state", "") == "expired"; }, 4500).value("session", "") == "wb",
              "at expiry the live connection gets share-state expired");
        check(guestC.atEnd(), "and is closed");
    }
    {
        Tls guestC(devC, 12);
        json sessions;
        const json welcome = guestC.hello(&sessions);
        check(welcome.value("type", "") == "welcome" && sessions["sessions"].empty(), "logging in after expiry: a welcome and no sessions: " + sessions.dump());
        check(guestC.next("share-state", [](const json &m) { return m.value("state", "") == "expired"; }, 2000).value("state", "") == "expired",
              "then share-state expired");
        check(guestC.atEnd(), "and the connection is closed");
    }
    link = parseLink(pvaiRun("share wb --ttl 1s --link"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    {
        Tls late(devX, 13);
        check(late.pair(link.code, "late") != "paired", "an invitation past its time does not pair");
    }

    // ---- A session that ended: the share is ended, the device stays; a move needs the same conversation.
    startSession("wm1", proj, "c-m", home + "/tr-wm1.jsonl");
    startSession("wm2", proj, "c-m", home + "/tr-wm2.jsonl");
    app.next("sessions", [](const json &m) { return !sessionIn(m, "wm1").empty() && !sessionIn(m, "wm2").empty(); }, 3000);
    // wm2 stands for a Claude Code that resumed the same conversation under a new session id. wm1 is the shared one.
    link = parseLink(pvaiRun("share wm1 --link"));
    {
        Tls guest(devD, 14);
        check(guest.pair(link.code, "Dora") == "paired", "a guest for wm1 pairs");
    }
    Tls guestD(devD, 15);
    guestD.hello();
    delete g_wrappers[2]; // the wrapper of wm1 goes
    g_wrappers[2] = nullptr;
    check(guestD.next("share-state", [](const json &m) { return m.value("state", "") == "ended"; }, 3000).value("session", "") == "wm1",
          "the wrapper ends: the live guest gets share-state ended");
    guestD.send({{"type", "prompt"}, {"ref", 15}, {"session", "wm1"}, {"text", "anyone?"}});
    check(reasonOf(guestD.next("rejected")) == "ended", "requests are refused: ended");
    check(!deviceNamed("Dora").empty() && deviceNamed("Dora")["guest"].value("state", "") == "ended", "the device stays paired, state ended");
    {
        int status = 0;
        check(pvaiRun("share move Dora wb", &status).find("conversation-mismatch") != std::string::npos && status == 1,
              "move to a session of another conversation: conversation-mismatch");
        check(pvaiRun("share move Dora nosuch", &status).find("unknown-session") != std::string::npos, "move to an unknown session");
        const std::string out = pvaiRun("share move Dora wm2", &status);
        check(status == 0 && out.find("moved: Dora from session wm1 to wm2") != std::string::npos, "move to wm2 (same conversation): " + out);
    }
    check(guestD.next("share-state", [](const json &m) { return m.value("state", "") == "active" && m.value("session", "") == "wm2"; }, 3000)
              .value("session", "") == "wm2",
          "the guest is told: active on wm2");
    guestD.send({{"type", "prompt"}, {"ref", 16}, {"session", "wm2"}, {"text", "now on the new one"}});
    check(!guestD.next("accepted").empty(), "its prompt reaches wm2");
    guestD.send({{"type", "prompt"}, {"ref", 17}, {"session", "wm1"}, {"text", "the old one"}});
    check(reasonOf(guestD.next("rejected")) == "guest-denied", "the old session id is not its session any more: guest-denied");

    // ---- A guest that never gets its session back after a daemon restart ends after the grace period;
    // one whose session comes back stays. Provenance and the guest list survive the restart.
    startSession("wgone", proj, "c-gone", home + "/tr-wgone.jsonl");
    app.next("sessions", [](const json &m) { return !sessionIn(m, "wgone").empty(); }, 3000);
    link = parseLink(pvaiRun("share wgone --link"));
    {
        Tls guest(devE, 16);
        check(guest.pair(link.code, "Emil") == "paired", "a guest for wgone pairs");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // lastSeen and the like
    for (auto *&w : g_wrappers) {
        delete w;
        w = nullptr;
    }
    g_wrappers.clear();
    daemon.stop();
    check(deviceNamed("Anna phone")["guest"].value("state", "") == "active", "after the daemon stopped, Anna's share is still active in the file");
    startDaemon();
    // wa and wb come back (a wrapper registers again while its Claude Code runs), wgone does not.
    startSession("wa", proj, "c-wa-2", trA2, true);
    startSession("wb", proj, "c-wb", trB, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    {
        Tls guest(devA, 17);
        json sessions;
        const json welcome = guest.hello(&sessions);
        check(welcome["guest"].value("state", "") == "active" && !sessionIn(sessions, "wa").empty(),
              "after the restart the guest logs in, active, its session is back: " + welcome.dump().substr(0, 200));
        guest.send({{"type", "subscribe"}, {"session", "wa"}, {"limit", 200}});
        const json history = guest.next("history");
        // trA2 holds nothing of the old conversation: read the first transcript through a resumed wb-style check instead.
        check(history.value("type", "") == "history", "subscribe after the restart");
    }
    // The old conversation's `from` survives: a session on the old transcript, the same conversation id as at grant.
    {
        // Point a fresh session at trA (conversation c-wa) and ask for its history as the owner.
        startSession("wa-old", proj, "c-wa", trA);
        LineClient owner;
        owner.send({{"type", "hello"}, {"client", "after-restart"}, {"version", "0"}});
        owner.next("sessions", [](const json &m) { return !sessionIn(m, "wa-old").empty(); }, 3000);
        owner.send({{"type", "subscribe"}, {"session", "wa-old"}, {"limit", 100}});
        const json history = owner.next("history", [](const json &m) { return m.value("session", "") == "wa-old"; });
        std::map<std::string, std::string> fromOf;
        for (const auto &e : history["entries"])
            if (e.value("kind", "") == "prompt")
                fromOf[e.value("id", "")] = e.value("from", "-");
        check(fromOf["u-guest-1"] == "Anna phone" && fromOf["u-owner-1"] == "-",
              "from survives a daemon restart (guest-prompts.json): u-guest-1=" + fromOf["u-guest-1"] + " u-owner-1=" + fromOf["u-owner-1"]);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4500)); // the grace period of 4 s
    {
        const std::string list = pvaiRun("share list");
        check(list.find("Emil") != std::string::npos && list.find("ended") != std::string::npos,
              "a share whose session did not come back within the grace period is ended: " + list);
        check(list.find("Anna phone") != std::string::npos && list.find("Dora") != std::string::npos, "the list names the other guests too");
        check(deviceNamed("Anna phone")["guest"].value("state", "") == "active", "while Anna's, whose session came back, is still active");
    }

    daemon.stop();
    return finish();
}
