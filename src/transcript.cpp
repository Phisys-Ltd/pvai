#include "transcript.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include <fstream>

namespace pvai {

namespace {

constexpr std::size_t kMaxText = 64 * 1024;
constexpr std::size_t kMaxResult = 4 * 1024;
constexpr std::size_t kMaxSummary = 300;

std::string cut(std::string text, std::size_t limit)
{
    if (text.size() <= limit)
        return text;
    std::size_t at = limit;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        --at;
    text.resize(at);
    return text + "\xE2\x80\xA6"; // …
}

// Unix seconds of a transcript timestamp ("2026-10-28T19:20:28.623Z"), 0 if
// it is none.
long long unixTime(const std::string &stamp)
{
    std::tm tm{};
    if (std::sscanf(stamp.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<long long>(timegm(&tm));
}

// One line that says what a tool call does.
std::string toolSummary(const json &input)
{
    if (!input.is_object())
        return {};
    for (const char *key : {"description", "command", "file_path", "pattern", "url", "query", "skill", "prompt"})
        if (input.contains(key) && input[key].is_string())
            return cut(input[key].get<std::string>(), kMaxSummary);
    return cut(input.dump(), kMaxSummary);
}

std::string resultText(const json &content)
{
    if (content.is_string())
        return content.get<std::string>();
    std::string text;
    if (content.is_array())
        for (const auto &block : content)
            if (block.is_object() && block.value("type", "") == "text")
                text += block.value("text", "");
    return text;
}

std::string stripAnsi(const std::string &text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && !(text[i] >= 0x40 && text[i] <= 0x7e))
                ++i;
            continue;
        }
        out += text[i];
    }
    return out;
}

// The text between <tag> and </tag>, if `text` holds that element.
bool element(const std::string &text, const std::string &tag, std::string &inner)
{
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    const auto a = text.find(open);
    if (a == std::string::npos)
        return false;
    const auto b = text.find(close, a + open.size());
    inner = text.substr(a + open.size(), b == std::string::npos ? std::string::npos : b - a - open.size());
    return true;
}

// Prompts Claude Code writes itself: command echoes, reminders, caveats.
bool isMachinePrompt(const std::string &text)
{
    return !text.empty() && text[0] == '<';
}

} // namespace

std::string commandFile(const std::string &command)
{
    std::string found;
    std::size_t at = 0;
    while (at < command.size()) {
        const std::size_t end = std::min(command.find_first_of(" \t\n|;&<>()", at), command.size());
        std::string word = command.substr(at, end - at);
        at = end + 1;
        if (word.size() >= 2 && (word.front() == '\'' || word.front() == '"') && word.back() == word.front())
            word = word.substr(1, word.size() - 2);
        if (word.empty() || word.front() == '-' || word.find_first_of("*?$=:") != std::string::npos)
            continue;
        const std::size_t slash = word.rfind('/');
        const std::size_t dot = word.rfind('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == 0 || dot + 1 == word.size()
            || dot + 11 < word.size())
            continue;
        const std::string ext = word.substr(dot + 1);
        if (!std::all_of(ext.begin(), ext.end(), [](unsigned char c) { return std::isalnum(c); })
            || std::all_of(ext.begin(), ext.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        if (!found.empty() && found != word)
            return {};
        found = word;
    }
    return found;
}

void TranscriptTail::follow(const std::string &path)
{
    path_ = path;
    offset_ = 0;
    first_ = true;
    partial_.clear();
    recent_.clear();
    toolNames_.clear();
    cache_ = nullptr;
}

std::vector<json> TranscriptTail::poll()
{
    std::vector<json> out;
    if (path_.empty())
        return out;
    struct stat st;
    if (::stat(path_.c_str(), &st) != 0)
        return out;
    if (st.st_size < offset_) // rewritten: start over
        follow(std::string(path_));
    if (st.st_size == offset_)
        return out;
    std::ifstream in(path_, std::ios::binary);
    bool skipFirstLine = false;
    if (first_ && st.st_size > kFirstRead) {
        offset_ = st.st_size - kFirstRead;
        skipFirstLine = true;
    }
    first_ = false;
    if (!in.seekg(offset_))
        return out;
    std::string data(static_cast<std::size_t>(st.st_size - offset_), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(in.gcount()));
    offset_ += static_cast<off_t>(data.size());
    partial_ += data;

    std::size_t start = 0;
    if (skipFirstLine) {
        const auto nl = partial_.find('\n');
        start = nl == std::string::npos ? partial_.size() : nl + 1;
    }
    for (std::size_t end; (end = partial_.find('\n', start)) != std::string::npos; start = end + 1) {
        const json line = json::parse(partial_.begin() + static_cast<long>(start), partial_.begin() + static_cast<long>(end), nullptr, false);
        if (line.is_object())
            take(line, out);
    }
    partial_.erase(0, start);
    for (const auto &entry : out) {
        recent_.push_back(entry);
        if (recent_.size() > kKeep)
            recent_.pop_front();
    }
    return out;
}

namespace {

// A task notification Claude Code queues for itself: an attachment in
// newer versions, a user message in older ones.
bool isWake(const json &line)
{
    if (line.value("isSidechain", false))
        return false;
    const std::string mark = "<task-notification>";
    const std::string type = line.value("type", "");
    if (type == "attachment" && line.contains("attachment") && line["attachment"].is_object()) {
        const json &attachment = line["attachment"];
        return attachment.value("type", "") == "queued_command" && attachment.contains("prompt")
            && attachment["prompt"].is_string() && attachment["prompt"].get<std::string>().rfind(mark, 0) == 0;
    }
    if (type == "user" && line.contains("message") && line["message"].is_object()) {
        const json &content = line["message"].value("content", json());
        return content.is_string() && content.get<std::string>().rfind(mark, 0) == 0;
    }
    return false;
}

} // namespace

void TranscriptTail::take(const json &line, std::vector<json> &out)
{
    const std::string type = line.value("type", "");
    if (isWake(line)) {
        wakePending_ = true;
    } else if (!line.value("isSidechain", false) && !line.value("isMeta", false)) {
        if (type == "assistant" && wakePending_) {
            ++wakeTurns_;
            wakePending_ = false;
        } else if (type == "user" && line.contains("message") && line["message"].is_object()
                   && line["message"].value("content", json()).is_string()) {
            wakePending_ = false; // the owner's prompt takes the notification along
        }
    }
    // Local slash commands and their output: a system line of their own in
    // newer Claude Code versions, a user message in older ones.
    const bool localCommand = type == "system" && line.value("subtype", "") == "local_command";
    if ((type != "user" && type != "assistant" && !localCommand) || line.value("isSidechain", false)
        || line.value("isMeta", false))
        return;
    if (!localCommand && (!line.contains("message") || !line["message"].is_object()))
        return;
    if (type == "assistant") {
        const json usage = line["message"].value("usage", json());
        const json created = usage.is_object() ? usage.value("cache_creation", json()) : json();
        long long ttl = cache_.is_object() ? cache_.value("ttl", 0LL) : 0;
        if (created.is_object() && created.value("ephemeral_1h_input_tokens", 0LL) > 0)
            ttl = 3600;
        else if (created.is_object() && created.value("ephemeral_5m_input_tokens", 0LL) > 0)
            ttl = 300;
        if (const long long at = unixTime(line.value("timestamp", "")); usage.is_object() && ttl > 0 && at > 0)
            cache_ = {{"ttl", ttl}, {"at", at}};
    }
    const json &content = localCommand ? line["content"] : line["message"].value("content", json());
    const json base = {{"id", line.value("uuid", "")}, {"time", line.value("timestamp", "")}};
    auto emit = [&](json entry) {
        entry.update(base);
        out.push_back(std::move(entry));
    };
    auto prompt = [&](const std::string &text) {
        std::string inner, args;
        if (text.rfind("[Request interrupted by user", 0) == 0) {
            emit({{"kind", "interrupted"}});
        } else if (text.rfind("<command-name>", 0) == 0 && element(text, "command-name", inner)) {
            element(text, "command-args", args);
            emit({{"kind", "command"}, {"text", args.empty() ? inner : inner + " " + args}});
        } else if (element(text, "local-command-stdout", inner) || element(text, "local-command-stderr", inner)) {
            ++localOutputs_;
            inner = stripAnsi(inner);
            if (inner.find_first_not_of(" \n") != std::string::npos)
                emit({{"kind", "output"}, {"text", cut(inner, kMaxResult)}});
        } else if (!isMachinePrompt(text)) {
            emit({{"kind", "prompt"}, {"text", cut(text, kMaxText)}});
        }
    };

    if (content.is_string()) {
        if (type == "user" || localCommand)
            prompt(content.get<std::string>());
        return;
    }
    if (!content.is_array())
        return;
    for (const auto &block : content) {
        if (!block.is_object())
            continue;
        const std::string kind = block.value("type", "");
        if (type == "user" && kind == "text") {
            prompt(block.value("text", ""));
        } else if (type == "assistant" && kind == "text") {
            const std::string text = block.value("text", "");
            if (!text.empty())
                emit({{"kind", "text"}, {"text", cut(text, kMaxText)}});
        } else if (type == "assistant" && kind == "tool_use") {
            const std::string name = block.value("name", "");
            toolNames_[block.value("id", "")] = name;
            if (toolNames_.size() > 2000)
                toolNames_.erase(toolNames_.begin());
            const json input = block.value("input", json());
            json entry = {{"kind", "tool"}, {"tool", name}, {"text", toolSummary(input)}};
            // The file whose content the result shows.
            const std::string file = !input.is_object() ? std::string()
                                     : name == "Read" ? input.value("file_path", "")
                                     : name == "Bash" ? commandFile(input.value("command", ""))
                                                      : std::string();
            if (!file.empty())
                entry["file"] = file;
            emit(std::move(entry));
        } else if (type == "user" && kind == "tool_result") {
            const auto it = toolNames_.find(block.value("tool_use_id", ""));
            emit({{"kind", "result"}, {"tool", it == toolNames_.end() ? "" : it->second},
                  {"error", block.value("is_error", false)},
                  {"text", cut(resultText(block.value("content", json())), kMaxResult)}});
        }
    }
}

} // namespace pvai
