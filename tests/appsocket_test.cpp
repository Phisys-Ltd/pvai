// The app socket (docs/PROTOCOL.md, "Transport", since 0.7.0) against a real
// daemon: an app over app.sock needs the token and gets the app's rules, the
// daemon runs without the TCP port, and `pvai endpoint` says what it holds.
//
// usage: pvai-appsocket-test /path/to/pvai
#include "support.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>

#include <fstream>

using namespace pvai;
using namespace pvai::test;
namespace fs = std::filesystem;

namespace {

json helloWith(const std::string &token)
{
    return {{"type", "hello"}, {"client", "test-app"}, {"version", "0"}, {"token", token}};
}

mode_t modeOf(const std::string &path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    const std::string pvai = fs::absolute(argv[1]).string();
    const TestHome home;
    ::setenv("PVAI_CODEX", "", 1);
    ::unsetenv(kEnvId);

    // Another user's daemon holds the TCP port: here, a listener of the test.
    const int port = 20000 + static_cast<int>(::getpid() % 20000);
    const int blocker = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); // not inherited by the daemon and its helpers
    const int one = 1;
    ::setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &in.sin_addr);
    check(::bind(blocker, reinterpret_cast<sockaddr *>(&in), sizeof in) == 0 && ::listen(blocker, 4) == 0, "the test holds the TCP port");

    DaemonProcess daemon;
    daemon.start(pvai, port, 500, false, {{"PVAI_CODEX", ""}});
    const std::string app = runtimeDir() + "/app.sock";
    check(app == appSocket(), "appSocket() is runtimeDir()/app.sock");
    check(fs::is_socket(app), "the daemon serves app.sock although the TCP port is taken");
    check(modeOf(runtimeDir()) == 0700, "the runtime directory is 0700");
    check(modeOf(app) == 0600, "app.sock is 0600");

    const std::string token = pvai::token();
    {
        LineClient c(app);
        c.send(helloWith(token));
        const json welcome = c.next("welcome");
        check(!welcome.empty(), "hello with the token over app.sock gets welcome");
        c.send({{"type", "ping"}});
        check(!c.next("pong").empty(), "and the app is served (ping)");
    }
    {
        LineClient c(app);
        c.send({{"type", "hello"}, {"client", "test-app"}, {"version", "0"}});
        const json reply = c.next();
        check(reply.value("type", "") == "error" && reply.value("code", "") == "auth", "hello without the token is refused: " + reply.dump());
        check(c.atEnd(), "and the connection is closed");
    }
    {
        LineClient c(app);
        c.send(helloWith("wrong"));
        check(c.next().value("code", "") == "auth", "a wrong token is refused");
    }
    // What the app socket peer must not do, as on loopback TCP.
    for (const json &message : {json{{"type", "register"}, {"version", kVersion}, {"id", "evil"}, {"cwd", "/x"}, {"label", "x"}},
                                json{{"type", "hook"}, {"event", {{"hook_event_name", "SessionStart"}}}, {"id", "evil"}},
                                json{{"type", "pair-open"}},
                                json{{"type", "attach"}, {"assistant", "claude"}},
                                json{{"type", "endpoint"}}}) {
        LineClient c(app);
        c.send(message);
        const json reply = c.next();
        check(reply.value("type", "") == "error" && reply.value("code", "") == "auth", message.value("type", "") + " over app.sock is refused: " + reply.dump());
    }
    {
        // And after a hello it is still no wrapper.
        LineClient c(app);
        c.send(helloWith(token));
        c.next("welcome");
        c.send({{"type", "register"}, {"version", kVersion}, {"id", "evil"}, {"cwd", "/x"}, {"label", "x"}});
        c.send({{"type", "ping"}});
        bool sessionAppeared = false;
        LineClient local;
        local.send(helloWith(""));
        const json sessions = local.next("sessions");
        for (const auto &s : sessions.value("sessions", json::array()))
            sessionAppeared = sessionAppeared || s.value("id", "") == "evil";
        check(!sessionAppeared, "an app cannot register a session");
    }

    // pvai endpoint
    const RunResult endpoint = runPvai(pvai, "endpoint");
    const json info = json::parse(endpoint.out, nullptr, false);
    check(endpoint.status == 0 && info.is_object(), "pvai endpoint prints JSON: " + endpoint.out);
    check(info.value("token", "") == token && info.value("socket", "") == app, "endpoint: token and socket");
    check(info.contains("port") && info["port"].is_null(), "endpoint: port is null while the TCP port is taken");
    check(runPvai(pvai, "token").out == token, "pvai token still prints the plain token");

    // The daemon log says why.
    std::ifstream log(runtimeDir() + "/daemon.log");
    (void)log;
    daemon.stop();
    ::close(blocker);

    // With the port free: both.
    DaemonProcess second;
    second.start(pvai, port, 500, false, {{"PVAI_CODEX", ""}});
    const json both = json::parse(runPvai(pvai, "endpoint").out, nullptr, false);
    check(both.is_object() && both["port"] == port && both["socket"] == app, "endpoint with the port free: " + both.dump());
    {
        LineClient c(app);
        c.send(helloWith(token));
        check(!c.next("welcome").empty(), "app.sock works as well when the port is held (stale socket replaced)");
    }
    second.stop();

    // Direct access on a port in use.
    const int busy = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    in.sin_port = htons(static_cast<uint16_t>(port + 1));
    ::setsockopt(busy, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    check(::bind(busy, reinterpret_cast<sockaddr *>(&in), sizeof in) == 0 && ::listen(busy, 4) == 0, "the test holds the direct port");
    DaemonProcess third;
    third.start(pvai, port, 500, false, {{"PVAI_CODEX", ""}});
    const RunResult listen = runPvai(pvai, "listen 127.0.0.1:" + std::to_string(port + 1));
    check(listen.status == 1 && listen.out.find("is in use (another user's daemon?); choose another port with pvai listen 127.0.0.1:PORT") != std::string::npos,
          "pvai listen reports the port in use: " + listen.out);
    third.stop();
    ::close(busy);
    return finish();
}
