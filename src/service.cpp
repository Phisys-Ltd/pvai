#include "service.h"

#include "common.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pvai {

namespace {

constexpr const char *kUnit = "pvai.service";

std::string unitPath()
{
    const char *home = std::getenv("HOME");
    const char *config = std::getenv("XDG_CONFIG_HOME");
    const std::string base = config && *config ? config : std::string(home ? home : "") + "/.config";
    return base + "/systemd/user/" + kUnit;
}

// Runs a command and returns its exit status (-1 when it did not run).
int runQuiet(const std::vector<std::string> &argv, bool quiet)
{
    const pid_t pid = ::fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (quiet) {
            const int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) {
                ::dup2(null, STDOUT_FILENO);
                ::dup2(null, STDERR_FILENO);
            }
        }
        std::vector<char *> args;
        for (const auto &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// systemd reads % as a specifier and needs quotes around values with blanks.
std::string unitValue(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c == '%')
            out += "%%";
        else if (c == '"' || c == '\\')
            out += std::string("\\") + c;
        else
            out += c;
    }
    return out;
}

std::string unitText()
{
    std::string text =
        "# Written by pvai install-service; pvai uninstall-service removes it.\n"
        "[Unit]\n"
        "Description=pvai: Claude Code and Codex sessions reachable by voice\n"
        "\n"
        "[Service]\n"
        "ExecStart=\"" + unitValue(selfExe()) + "\" daemon\n";
    // The user manager's environment is bare; sessions the daemon starts
    // need the PATH and locale the owner works with.
    for (const char *name : {"PATH", "LANG", "LC_ALL"})
        if (const char *value = std::getenv(name); value && *value)
            text += "Environment=\"" + std::string(name) + "=" + unitValue(value) + "\"\n";
    text +=
        "Restart=on-failure\n"
        "RestartSec=3\n"
        "# Sessions outlive their daemon, as they do with pvai daemon restart.\n"
        "KillMode=process\n"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n";
    return text;
}

bool haveSystemd()
{
#ifdef __linux__
    struct stat st;
    return ::stat("/run/systemd/system", &st) == 0 && !findBinary("systemctl").empty();
#else
    return false;
#endif
}

} // namespace

int installService()
{
    if (!haveSystemd()) {
        std::fputs("pvai install-service: needs Linux with systemd; the daemon still starts on demand\n", stderr);
        return 1;
    }
    const std::string path = unitPath();
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    {
        std::ofstream out(path, std::ios::trunc);
        if (!(out << unitText())) {
            std::fprintf(stderr, "pvai install-service: cannot write %s\n", path.c_str());
            return 1;
        }
    }
    std::printf("wrote %s\n", path.c_str());
    // Lingering starts the user's systemd at boot, without a login.
    if (runQuiet({"loginctl", "enable-linger"}, false) != 0)
        std::fputs("pvai: loginctl enable-linger failed; the daemon then starts only at login\n", stderr);
    if (runQuiet({"systemctl", "--user", "daemon-reload"}, false) != 0
        || runQuiet({"systemctl", "--user", "enable", kUnit}, false) != 0) {
        std::fputs("pvai install-service: systemctl --user failed (no user session bus?)\n", stderr);
        return 1;
    }
    const int fd = connectUnix(daemonSocket());
    if (fd >= 0) {
        ::close(fd);
        std::puts("enabled; a daemon already runs outside the service: pvai daemon restart moves it in");
        return 0;
    }
    if (runQuiet({"systemctl", "--user", "start", kUnit}, false) != 0) {
        std::fputs("pvai install-service: enabled, but systemctl --user start failed\n", stderr);
        return 1;
    }
    std::puts("enabled and started; it starts again after every boot");
    return 0;
}

int uninstallService()
{
    const std::string path = unitPath();
    if (::access(path.c_str(), F_OK) != 0) {
        std::puts("no service installed");
        return 0;
    }
    // The running daemon stays; pvai daemon stop ends it.
    runQuiet({"systemctl", "--user", "disable", kUnit}, false);
    if (::unlink(path.c_str()) != 0) {
        std::fprintf(stderr, "pvai uninstall-service: cannot remove %s\n", path.c_str());
        return 1;
    }
    runQuiet({"systemctl", "--user", "daemon-reload"}, true);
    std::printf("removed %s (lingering stays on: loginctl disable-linger turns it off)\n", path.c_str());
    return 0;
}

bool startServiceDaemon()
{
    if (!haveSystemd() || ::access(unitPath().c_str(), F_OK) != 0)
        return false;
    return runQuiet({"systemctl", "--user", "start", kUnit}, true) == 0;
}

} // namespace pvai
