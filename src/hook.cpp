// pvai hook: what Claude Code's hooks run. It hands the event to the daemon
// and gets out of the way: no output except the context SessionStart adds,
// exit 0 whatever happens, nothing at all outside a `pvai run` session.
#include "common.h"

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <iterator>

namespace pvai {

int runHook()
{
    const std::string input(std::istreambuf_iterator<char>(std::cin), {});
    const char *id = std::getenv(kEnvId);
    const char *socket = std::getenv(kEnvSocket);
    if (!id || !*id || !socket || !*socket)
        return 0;
    json event = json::parse(input, nullptr, false);
    if (event.is_discarded() || !event.is_object())
        return 0;
    if (event.value("hook_event_name", "") == "SessionStart")
        std::fputs(kVoiceContext, stdout);
    const int fd = connectUnix(socket);
    if (fd < 0)
        return 0;
    sendLine(fd, {{"type", "hook"}, {"id", id}, {"event", event}});
    ::close(fd);
    return 0;
}

} // namespace pvai

namespace pvai {

namespace {

std::string percent(const json &value)
{
    return value.is_number() ? std::to_string(value.get<long long>()) + "%" : "?";
}

} // namespace

// pvai statusline: what Claude Code's status line runs. Hands the status
// (context window, rate limits, model, cost) to the daemon and prints a
// short line; with --chain, the owner's own status line command prints it.
int runStatusline(const std::string &chain)
{
    const std::string input(std::istreambuf_iterator<char>(std::cin), {});
    const json status = json::parse(input, nullptr, false);
    const char *id = std::getenv(kEnvId);
    const char *socket = std::getenv(kEnvSocket);
    if (id && *id && socket && *socket && status.is_object()) {
        if (const int fd = connectUnix(socket); fd >= 0) {
            sendLine(fd, {{"type", "status"}, {"id", id}, {"status", status}});
            ::close(fd);
        }
    }
    if (!chain.empty()) {
        FILE *pipe = ::popen(chain.c_str(), "w");
        if (!pipe)
            return 0;
        std::fwrite(input.data(), 1, input.size(), pipe);
        return ::pclose(pipe) == 0 ? 0 : 1;
    }
    if (!status.is_object())
        return 0;
    const json none = json::object();
    const json &model = status.contains("model") ? status["model"] : none;
    const json &context = status.contains("context_window") ? status["context_window"] : none;
    const json &limits = status.contains("rate_limits") ? status["rate_limits"] : none;
    std::string line = model.is_object() ? model.value("display_name", "") : "";
    if (context.is_object())
        line += " \xC2\xB7 ctx " + percent(context.value("used_percentage", json()));
    if (limits.is_object()) {
        if (limits.contains("five_hour") && limits["five_hour"].is_object())
            line += " \xC2\xB7 5h " + percent(limits["five_hour"].value("used_percentage", json()));
        if (limits.contains("seven_day") && limits["seven_day"].is_object())
            line += " \xC2\xB7 7d " + percent(limits["seven_day"].value("used_percentage", json()));
    }
    if (id && *id)
        line += " \xC2\xB7 pvai";
    std::printf("%s\n", line.c_str());
    return 0;
}

} // namespace pvai
