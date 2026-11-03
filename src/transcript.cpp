#include "transcript.h"
#include "text.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fstream>
#include <string_view>

namespace pvai {

namespace {

constexpr std::size_t kMaxSummary = 300;

// One line that says what a tool call does.
std::string toolSummary(const json &input)
{
    if (!input.is_object())
        return {};
    if (const auto text = firstString(input, {"description", "command", "file_path", "pattern", "url", "query", "skill", "prompt"}))
        return cut(*text, kMaxSummary);
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

} // namespace

bool isBookkeeping(const json &line)
{
    return line.value("isMeta", false) || line.value("isCompactSummary", false) || line.value("isVisibleInTranscriptOnly", false);
}

bool isMachinePrompt(const std::string &text)
{
    static const char *const kTags[] = {
        "system-reminder", "task-notification", "local-command-caveat", "local-command-stdout",
        "local-command-stderr", "command-name", "command-message", "command-args", "bash-input",
        "bash-stdout", "bash-stderr", "artifact-content-authored-by-others", "user-prompt-submit-hook",
    };
    if (text.empty() || text[0] != '<')
        return false;
    for (const char *tag : kTags) {
        const std::size_t n = std::strlen(tag);
        if (text.compare(1, n, tag) == 0 && text.size() > n + 1 && std::strchr("> /\n", text[n + 1]))
            return true;
    }
    return false;
}

std::string stripPasteFrame(const std::string &text)
{
    if (text.find("<pasted_content") == std::string::npos && text.find("</pasted_content") == std::string::npos)
        return text;
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t open = text.find("<pasted_content", at);
        const std::size_t close = text.find("</pasted_content", at);
        const std::size_t tag = std::min(open, close);
        if (tag == std::string::npos) {
            out += text.substr(at);
            break;
        }
        const std::size_t end = text.find('>', tag);
        if (end == std::string::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, tag - at);
        at = end + 1;
        if (tag == open && at < text.size() && text[at] == '\n')
            ++at;
    }
    const std::size_t first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return out.substr(first, out.find_last_not_of(" \t\r\n") - first + 1);
}

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

namespace {

bool isWake(const json &line);

// A message from another Claude session or from pvai itself: Claude Code
// writes it as a meta user line and takes it up like a task notification.
bool isHandBack(const json &line)
{
    if (line.value("type", "") != "user" || !line.value("isMeta", false) || !line.contains("message") || !line["message"].is_object())
        return false;
    const json content = line["message"].value("content", json());
    if (!content.is_string())
        return false;
    const std::string text = content.get<std::string>();
    return text.rfind("Another Claude session sent a message", 0) == 0 || text.rfind("[Message p", 0) == 0;
}

// A task notification younger than this, not answered yet, is a turn Claude
// Code is about to run (or runs: the answer is written when it is complete).
constexpr long long kNotificationFresh = 10 * 60;

bool freshNote(long long noteAt)
{
    return noteAt > 0 && static_cast<long long>(std::time(nullptr)) - noteAt < kNotificationFresh;
}

// The turn state machine of turnRunning, one transcript line at a time.
// noteAt: when the task notification nobody answered yet was written.
void turnStep(const json &entry, bool &running, bool &ends, long long &noteAt)
{
    if (!entry.is_object() || entry.value("isSidechain", false))
        return;
    const std::string type = entry.value("type", "");
    if (isWake(entry) || isHandBack(entry)) {
        noteAt = unixTime(entry.value("timestamp", ""));
        return;
    }
    if (type == "system" && (entry.value("subtype", "") == "turn_duration" || entry.value("subtype", "") == "stop_hook_summary")) {
        running = false;
        ends = true;
        noteAt = 0;
    } else if (type == "user" && !isBookkeeping(entry) && entry.contains("message") && entry["message"].is_object()) {
        const json content = entry["message"].value("content", json());
        const std::string text = content.is_string() ? content.get<std::string>()
                               : content.is_array() && !content.empty() && content[0].is_object() ? content[0].value("text", "") : "";
        if (text.rfind("[Request interrupted by user", 0) == 0) {
            running = false;
            noteAt = 0;
        } else if (content.is_string() && text.rfind("<command-name>", 0) != 0 && text.rfind("<local-command", 0) != 0
                   && text.rfind("<bash-", 0) != 0 && text.rfind("<task-notification", 0) != 0) {
            running = true;
            noteAt = 0;
        }
    } else if (type == "assistant") {
        running = true;
        noteAt = 0;
    }
}

// A line that cannot produce an entry or move the turn state: a sidechain
// line, or one of a type nothing reads. Cheap, before the JSON parse.
bool skippable(const char *line, std::size_t n)
{
    const std::string_view view(line, n);
    if (view.find("\"isSidechain\":true") != std::string_view::npos || view.find("\"isSidechain\": true") != std::string_view::npos)
        return true;
    static const char *const kTypes[] = {"\"user\"", "\"assistant\"", "\"system\"", "\"attachment\"", "\"queue-operation\""};
    for (const char *type : kTypes)
        if (view.find(type) != std::string_view::npos)
            return false;
    return true;
}

} // namespace

bool turnRunning(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(std::max<std::streamoff>(0, size - 4 * 1024 * 1024));
    std::string line;
    bool running = false, ends = false;
    long long noteAt = 0;
    if (size > 4 * 1024 * 1024)
        std::getline(in, line); // a partial first line
    while (std::getline(in, line)) {
        if (skippable(line.data(), line.size()))
            continue;
        turnStep(json::parse(line, nullptr, false), running, ends, noteAt);
    }
    // A turn end is not needed: an answer after the last one (or none at
    // all, in a window that holds no end) is a turn that goes on.
    (void)ends;
    return running || freshNote(noteAt);
}

std::optional<bool> TranscriptTail::turnRunningSeen() const
{
    if (turnRunning_ || freshNote(noteAt_))
        return true;
    if (turnEnds_)
        return false;
    if (windowed_)
        return std::nullopt; // no turn end in the window: the file may have one further back
    return false;
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
    resetState();
}

void TranscriptTail::resetState()
{
    toolNames_.clear();
    localOutputs_ = shellOutputs_ = wakeTurns_ = 0;
    lastLocalOutput_.clear();
    wakePending_ = false;
    cache_ = nullptr;
    limited_ = nullptr;
    limitHits_ = limitClears_ = turnEndCount_ = 0;
    turnRunning_ = turnEnds_ = windowed_ = false;
    noteAt_ = 0;
    recent_.clear();
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
        windowed_ = true;
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
        if (skippable(partial_.data() + start, end - start))
            continue;
        const json line = json::parse(partial_.begin() + static_cast<long>(start), partial_.begin() + static_cast<long>(end), nullptr, false);
        if (!line.is_object())
            continue;
        turnStep(line, turnRunning_, turnEnds_, noteAt_);
        try {
            take(line, out);
        } catch (const json::exception &) {
            // A line of the wrong shape: skipped.
        }
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

void TranscriptTail::clearLimited()
{
    limited_ = nullptr;
    ++limitClears_;
}

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
    if (type == "system" && line.value("subtype", "") == "turn_duration" && !line.value("isSidechain", false))
        ++turnEndCount_;
    // The usage limit: Claude Code writes the 429 as an assistant line and
    // goes on by itself at the reset, after a dequeue of its own prompt.
    if (type == "queue-operation") {
        if (line.value("operation", "") == "dequeue" && limited_.is_object()) {
            clearLimited();
            json entry = {{"kind", "resumed"}, {"id", line.value("uuid", "")}, {"time", line.value("timestamp", "")}};
            out.push_back(std::move(entry));
        }
        return;
    }
    if (type == "assistant" && !line.value("isSidechain", false)) {
        // A model's own quota (Fable's, model_requires_usage_credits) has no
        // quotaLimits: its reset is in apiErrorParams.rate_limit_info.
        const std::string apiError = line.value("apiError", "");
        const bool model = apiError == "model_requires_usage_credits";
        if (line.value("isApiErrorMessage", false) && (apiError == "usage_limit_reached" || model)) {
            std::string text;
            const json content = line.contains("message") && line["message"].is_object() ? line["message"].value("content", json()) : json();
            if (content.is_array())
                for (const auto &block : content)
                    if (block.is_object() && block.value("type", "") == "text") {
                        text = block.value("text", "");
                        break;
                    }
            text = cut(text, kMaxText);
            std::replace(text.begin(), text.end(), '\n', ' ');
            json quota = line.value("quotaLimits", json());
            if (model) {
                const json params = line.value("apiErrorParams", json());
                quota = params.is_object() ? params.value("rate_limit_info", json()) : json();
            }
            json at = nullptr, kind = nullptr;
            if (quota.is_object()) {
                if (quota.contains("resetsAt") && quota["resetsAt"].is_number())
                    at = quota["resetsAt"].get<long long>();
                if (quota.contains("rateLimitType") && quota["rateLimitType"].is_string())
                    kind = quota["rateLimitType"];
            }
            limited_ = {{"resetsAt", at}, {"type", kind}, {"text", text}, {"model", model}};
            ++limitHits_;
            json entry = {{"kind", "limited"}, {"text", text}, {"resetsAt", at}, {"type", kind}, {"model", model}};
            entry.update(json{{"id", line.value("uuid", "")}, {"time", line.value("timestamp", "")}});
            out.push_back(std::move(entry));
            return;
        }
        if (limited_.is_object())
            clearLimited();
    }
    // A compaction Claude Code started itself (the context ran full, or idle
    // before the prompt cache expired); /compact typed by the owner shows as
    // its command line already.
    if (type == "system" && line.value("subtype", "") == "compact_boundary" && !line.value("isSidechain", false)) {
        const json meta = line.value("compactMetadata", json());
        if (!meta.is_object() || meta.value("trigger", "") != "manual") {
            json entry = {{"kind", "command"}, {"text", "/compact"}};
            entry.update(json{{"id", line.value("uuid", "")}, {"time", line.value("timestamp", "")}});
            out.push_back(std::move(entry));
        }
        return;
    }
    // Local slash commands and their output: a system line of their own in
    // newer Claude Code versions, a user message in older ones. The summary
    // a compaction leaves is no prompt of the owner's.
    const bool localCommand = type == "system" && line.value("subtype", "") == "local_command";
    if ((type != "user" && type != "assistant" && !localCommand) || line.value("isSidechain", false) || isBookkeeping(line))
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
    const json &content = localCommand ? line.value("content", json()) : line["message"].value("content", json());
    const json base = {{"id", line.value("uuid", "")}, {"time", line.value("timestamp", "")}};
    auto emit = [&](json entry) {
        entry.update(base);
        out.push_back(std::move(entry));
    };
    auto prompt = [&](const std::string &raw) {
        const std::string text = stripPasteFrame(raw);
        std::string inner, args;
        if (text.rfind("[Request interrupted by user", 0) == 0) {
            emit({{"kind", "interrupted"}});
        } else if (text.rfind("<command-name>", 0) == 0 && element(text, "command-name", inner)) {
            element(text, "command-args", args);
            emit({{"kind", "command"}, {"text", args.empty() ? inner : inner + " " + args}});
        } else if (element(text, "local-command-stdout", inner) || element(text, "local-command-stderr", inner)) {
            ++localOutputs_;
            inner = stripAnsi(inner);
            lastLocalOutput_.clear();
            if (const auto from = inner.find_first_not_of(" \n\r\t"); from != std::string::npos)
                lastLocalOutput_ = inner.substr(from, inner.find_last_not_of(" \n\r\t") - from + 1);
            if (inner.find_first_not_of(" \n") != std::string::npos)
                emit({{"kind", "output"}, {"text", cut(inner, kMaxResult)}});
        } else if (text.rfind("<bash-input>", 0) == 0 && element(text, "bash-input", inner)) {
            // A shell command the owner typed with ! in Claude Code.
            emit({{"kind", "command"}, {"text", "! " + inner}});
        } else if (text.rfind("<bash-stdout>", 0) == 0 || text.rfind("<bash-stderr>", 0) == 0) {
            ++shellOutputs_; // a shell command's end, also without output
            std::string out, err;
            element(text, "bash-stdout", out);
            element(text, "bash-stderr", err);
            out = stripAnsi(out);
            err = stripAnsi(err);
            const bool hasOut = out.find_first_not_of(" \n") != std::string::npos;
            const bool hasErr = err.find_first_not_of(" \n") != std::string::npos;
            std::string both = hasOut ? out : "";
            if (hasErr)
                both += (hasOut && both.back() != '\n' ? "\n" : "") + err;
            if (hasOut || hasErr)
                emit({{"kind", "output"}, {"text", cut(both, kMaxResult)}});
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
