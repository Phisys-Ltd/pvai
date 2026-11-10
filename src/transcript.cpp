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
            if (block.is_object() && str(block, "type") == "text")
                text += str(block, "text");
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
    return flag(line, "isMeta") || flag(line, "isCompactSummary") || flag(line, "isVisibleInTranscriptOnly");
}

bool isAgentMessage(const std::string &text)
{
    for (const char *mark : {"[Message ", "Another Claude session sent a message"})
        if (text.rfind(mark, 0) == 0)
            return true;
    return false;
}

bool isMachinePrompt(const std::string &text)
{
    static const char *const kTags[] = {
        "system-reminder", "task-notification", "local-command-caveat", "local-command-stdout",
        "local-command-stderr", "command-name", "command-message", "command-args", "bash-input",
        "bash-stdout", "bash-stderr", "artifact-content-authored-by-others", "user-prompt-submit-hook",
    };
    if (isAgentMessage(text))
        return true;
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
    if (str(line, "type") != "user" || !flag(line, "isMeta") || !line.contains("message") || !line["message"].is_object())
        return false;
    const json content = member(line["message"], "content");
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
// runAt: when the last line that said running was written.
void turnStep(const json &entry, bool &running, bool &ends, long long &noteAt, long long *runAt = nullptr)
{
    if (!entry.is_object() || flag(entry, "isSidechain"))
        return;
    const std::string type = str(entry, "type");
    if (isWake(entry) || isHandBack(entry)) {
        noteAt = unixTime(str(entry, "timestamp"));
        return;
    }
    if (type == "system" && (str(entry, "subtype") == "turn_duration" || str(entry, "subtype") == "stop_hook_summary")) {
        running = false;
        ends = true;
        noteAt = 0;
    } else if (type == "user" && !isBookkeeping(entry) && entry.contains("message") && entry["message"].is_object()) {
        const json content = member(entry["message"], "content");
        const std::string text = content.is_string() ? content.get<std::string>()
                               : content.is_array() && !content.empty() && content[0].is_object() ? str(content[0], "text") : "";
        if (text.rfind("[Request interrupted by user", 0) == 0) {
            running = false;
            noteAt = 0;
        } else if (content.is_string() && text.rfind("<command-name>", 0) != 0 && text.rfind("<local-command", 0) != 0
                   && text.rfind("<bash-", 0) != 0 && text.rfind("<task-notification", 0) != 0
                   // Claude Code 2.1.318 writes a typed slash command once more
                   // as a plain line, and that writes no turn end. A prompt
                   // that only looks like one runs once Claude answers.
                   && text.rfind("/", 0) != 0) {
            running = true;
            noteAt = 0;
            if (runAt)
                *runAt = unixTime(str(entry, "timestamp"));
        }
    } else if (type == "assistant") {
        running = true;
        noteAt = 0;
        if (runAt)
            *runAt = unixTime(str(entry, "timestamp"));
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

bool turnRunning(const std::string &path, long long *runAt)
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
        turnStep(json::parse(line, nullptr, false), running, ends, noteAt, runAt);
    }
    // A turn end is not needed: an answer after the last one (or none at
    // all, in a window that holds no end) is a turn that goes on.
    (void)ends;
    return running || freshNote(noteAt);
}

// The prompt the owner submitted while a turn ran: Claude Code 2.1.x writes
// no user line for it (and none later), only a queued_command attachment. A
// task notification is the same attachment and no prompt.
static bool queuedPrompt(const json &line, std::string &text)
{
    if (str(line, "type") != "attachment" || flag(line, "isSidechain") || isBookkeeping(line) || !line.contains("attachment")
        || !line["attachment"].is_object())
        return false;
    const json &attachment = line["attachment"];
    if (str(attachment, "type") != "queued_command" || str(attachment, "commandMode") != "prompt" || !attachment.contains("prompt")
        || !attachment["prompt"].is_string())
        return false;
    text = stripPasteFrame(attachment["prompt"].get<std::string>());
    return !text.empty() && (!isMachinePrompt(text) || isAgentMessage(text));
}

bool lastPromptSpoken(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(std::max<std::streamoff>(0, size - 4 * 1024 * 1024));
    std::string line;
    if (size > 4 * 1024 * 1024)
        std::getline(in, line); // a partial first line
    bool spoken = false;
    while (std::getline(in, line)) {
        if (skippable(line.data(), line.size())
            || (line.find("\"user\"") == std::string::npos && line.find("queued_command") == std::string::npos))
            continue;
        const json entry = json::parse(line, nullptr, false);
        if (std::string queued; entry.is_object() && queuedPrompt(entry, queued)) {
            if (queued[0] != '/')
                spoken = queued.rfind("\xF0\x9F\x8E\x99 ", 0) == 0;
            continue;
        }
        if (!entry.is_object() || str(entry, "type") != "user" || flag(entry, "isSidechain") || isBookkeeping(entry)
            || !entry.contains("message") || !entry["message"].is_object())
            continue;
        const json content = member(entry["message"], "content");
        if (!content.is_string())
            continue;
        const std::string text = stripPasteFrame(content.get<std::string>());
        if (isMachinePrompt(text) || text.empty() || text[0] == '/')
            continue;
        spoken = text.rfind("\xF0\x9F\x8E\x99 ", 0) == 0;
    }
    return spoken;
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
    localOutputs_ = shellOutputs_ = wakeTurns_ = compactions_ = 0;
    lastLocalOutput_.clear();
    wakePending_ = false;
    cache_ = nullptr;
    limited_ = nullptr;
    limitedAt_.clear();
    limitHits_ = limitClears_ = turnEndCount_ = 0;
    authFailures_ = replies_ = 0;
    authFailedAt_.clear();
    turnRunning_ = turnEnds_ = windowed_ = false;
    noteAt_ = 0;
    queued_.clear();
    absorbed_.clear();
    endedTasks_.clear();
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
        try {
            turnStep(line, turnRunning_, turnEnds_, noteAt_);
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
    if (flag(line, "isSidechain"))
        return false;
    const std::string mark = "<task-notification>";
    const std::string type = str(line, "type");
    if (type == "attachment" && line.contains("attachment") && line["attachment"].is_object()) {
        const json &attachment = line["attachment"];
        return str(attachment, "type") == "queued_command" && attachment.contains("prompt")
            && attachment["prompt"].is_string() && attachment["prompt"].get<std::string>().rfind(mark, 0) == 0;
    }
    if (type == "user" && line.contains("message") && line["message"].is_object()) {
        const json &content = member(line["message"], "content");
        return content.is_string() && content.get<std::string>().rfind(mark, 0) == 0;
    }
    return false;
}

// The text of a line that may hold a task notification, whatever its shape:
// a queue-operation's content, a queued_command's prompt, a user message.
std::string notificationText(const json &line)
{
    const json content = member(line, "content");
    if (str(line, "type") == "queue-operation")
        return content.is_string() ? content.get<std::string>() : "";
    if (const json attachment = member(line, "attachment"); attachment.is_object())
        return str(attachment, "type") == "queued_command" ? str(attachment, "prompt") : "";
    if (const json message = member(line, "message"); message.is_object()) {
        const json body = member(message, "content");
        if (body.is_string())
            return body.get<std::string>();
        if (body.is_array() && !body.empty() && body[0].is_object())
            return str(body[0], "text");
    }
    return "";
}

std::string tagValue(const std::string &text, const std::string &tag)
{
    const std::string open = "<" + tag + ">";
    const auto from = text.find(open);
    if (from == std::string::npos)
        return "";
    const auto to = text.find("</" + tag + ">", from);
    return to == std::string::npos ? "" : text.substr(from + open.size(), to - from - open.size());
}

} // namespace

void TranscriptTail::clearLimited()
{
    limited_ = nullptr;
    ++limitClears_;
}

void TranscriptTail::take(const json &line, std::vector<json> &out)
{
    const std::string type = str(line, "type");
    if (!flag(line, "isSidechain")) {
        if (const std::string text = notificationText(line); text.find("<task-notification>") != std::string::npos) {
            const std::string id = tagValue(text, "task-id"), status = tagValue(text, "status");
            if (!id.empty() && (status == "killed" || status == "failed" || status == "completed"))
                endedTasks_[id] = std::filesystem::file_time_type::clock::now();
        }
    }
    // A hand-back is taken up like a task notification, with no hook.
    if (isWake(line) || (!flag(line, "isSidechain") && isHandBack(line))) {
        wakePending_ = true;
    } else if (!flag(line, "isSidechain") && !flag(line, "isMeta")) {
        if (type == "assistant" && wakePending_) {
            ++wakeTurns_;
            wakePending_ = false;
        } else if (type == "user" && line.contains("message") && line["message"].is_object()
                   && member(line["message"], "content").is_string()) {
            wakePending_ = false; // the owner's prompt takes the notification along
        }
    }
    if (type == "system" && str(line, "subtype") == "turn_duration" && !flag(line, "isSidechain"))
        ++turnEndCount_;
    // The usage limit: Claude Code writes the 429 as an assistant line and
    // goes on by itself at the reset, after a dequeue of its own prompt.
    if (type == "queue-operation") {
        const std::string operation = str(line, "operation");
        const json content = member(line, "content");
        if (operation == "enqueue" && content.is_string()) {
            queued_.push_back(content.get<std::string>());
            if (queued_.size() > 64)
                queued_.pop_front();
        } else if (operation == "dequeue" && !queued_.empty()) {
            queued_.pop_front();
        } else if (operation == "remove" && content.is_string()) {
            if (str(line, "reason") == "absorbed_mid_turn") {
                absorbed_.push_back(content.get<std::string>());
                if (absorbed_.size() > 64)
                    absorbed_.erase(absorbed_.begin());
            }
            if (const auto it = std::find(queued_.begin(), queued_.end(), content.get<std::string>()); it != queued_.end())
                queued_.erase(it);
        }
        if (operation == "dequeue" && limited_.is_object()) {
            clearLimited();
            json entry = {{"kind", "resumed"}, {"id", str(line, "uuid")}, {"time", str(line, "timestamp")}};
            out.push_back(std::move(entry));
        }
        return;
    }
    if (type == "assistant" && !flag(line, "isSidechain")) {
        if (flag(line, "isApiErrorMessage")) {
            const json status = member(line, "apiErrorStatus");
            const std::string error = str(line, "error"), apiError = str(line, "apiError");
            if (error == "authentication_failed" || apiError == "authentication_failed" || apiError == "authentication_error"
                || (status.is_number() && status == 401)) {
                ++authFailures_;
                authFailedAt_ = str(line, "timestamp");
            }
        } else if (!flag(line, "isMeta")) {
            ++replies_;
        }
        // A model's own quota (Fable's, model_requires_usage_credits) has no
        // quotaLimits: its reset is in apiErrorParams.rate_limit_info.
        const std::string apiError = str(line, "apiError");
        const bool model = apiError == "model_requires_usage_credits";
        if (flag(line, "isApiErrorMessage") && (apiError == "usage_limit_reached" || model)) {
            std::string text;
            const json content = line.contains("message") && line["message"].is_object() ? member(line["message"], "content") : json();
            if (content.is_array())
                for (const auto &block : content)
                    if (block.is_object() && str(block, "type") == "text") {
                        text = str(block, "text");
                        break;
                    }
            text = cut(text, kMaxText);
            std::replace(text.begin(), text.end(), '\n', ' ');
            json quota = member(line, "quotaLimits");
            if (model) {
                const json params = member(line, "apiErrorParams");
                quota = params.is_object() ? member(params, "rate_limit_info") : json();
            }
            json at = nullptr, kind = nullptr;
            if (quota.is_object()) {
                if (quota.contains("resetsAt") && quota["resetsAt"].is_number())
                    at = quota["resetsAt"].get<long long>();
                if (quota.contains("rateLimitType") && quota["rateLimitType"].is_string())
                    kind = quota["rateLimitType"];
            }
            limited_ = {{"resetsAt", at}, {"type", kind}, {"text", text}, {"model", model}};
            limitedAt_ = str(line, "timestamp");
            ++limitHits_;
            json entry = {{"kind", "limited"}, {"text", text}, {"resetsAt", at}, {"type", kind}, {"model", model}};
            entry.update(json{{"id", str(line, "uuid")}, {"time", str(line, "timestamp")}});
            out.push_back(std::move(entry));
            return;
        }
        if (limited_.is_object())
            clearLimited();
    }
    // A compaction Claude Code started itself (the context ran full, or idle
    // before the prompt cache expired); /compact typed by the owner shows as
    // its command line already.
    if (type == "system" && str(line, "subtype") == "compact_boundary" && !flag(line, "isSidechain")) {
        ++compactions_;
        const json meta = member(line, "compactMetadata");
        if (!meta.is_object() || str(meta, "trigger") != "manual") {
            json entry = {{"kind", "command"}, {"text", "/compact"}};
            entry.update(json{{"id", str(line, "uuid")}, {"time", str(line, "timestamp")}});
            out.push_back(std::move(entry));
        }
        return;
    }
    // Local slash commands and their output: a system line of their own in
    // newer Claude Code versions, a user message in older ones. The summary
    // a compaction leaves is no prompt of the owner's.
    const bool localCommand = type == "system" && str(line, "subtype") == "local_command";
    if (std::string queued; queuedPrompt(line, queued)) {
        json entry = {{"kind", "prompt"}, {"text", cut(queued, kMaxText)}};
        entry.update(json{{"id", str(line, "uuid")}, {"time", str(line, "timestamp")}});
        out.push_back(std::move(entry));
        return;
    }
    if ((type != "user" && type != "assistant" && !localCommand) || flag(line, "isSidechain") || isBookkeeping(line))
        return;
    if (!localCommand && (!line.contains("message") || !line["message"].is_object()))
        return;
    if (type == "assistant") {
        const json usage = member(line["message"], "usage");
        const json created = usage.is_object() ? member(usage, "cache_creation") : json();
        long long ttl = cache_.is_object() ? integer(cache_, "ttl") : 0;
        if (created.is_object() && integer(created, "ephemeral_1h_input_tokens") > 0)
            ttl = 3600;
        else if (created.is_object() && integer(created, "ephemeral_5m_input_tokens") > 0)
            ttl = 300;
        if (const long long at = unixTime(str(line, "timestamp")); usage.is_object() && ttl > 0 && at > 0)
            cache_ = {{"ttl", ttl}, {"at", at}};
    }
    const json &content = localCommand ? member(line, "content") : member(line["message"], "content");
    const json base = {{"id", str(line, "uuid")}, {"time", str(line, "timestamp")}};
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
        } else if (!isMachinePrompt(text) || isAgentMessage(text)) {
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
        const std::string kind = str(block, "type");
        if (type == "user" && kind == "text") {
            prompt(str(block, "text"));
        } else if (type == "assistant" && kind == "text") {
            const std::string text = str(block, "text");
            if (!text.empty())
                emit({{"kind", "text"}, {"text", cut(text, kMaxText)}});
        } else if (type == "assistant" && kind == "tool_use") {
            const std::string name = str(block, "name");
            toolNames_[str(block, "id")] = name;
            if (toolNames_.size() > 2000)
                toolNames_.erase(toolNames_.begin());
            const json input = member(block, "input");
            json entry = {{"kind", "tool"}, {"tool", name}, {"text", toolSummary(input)}};
            // The file whose content the result shows.
            const std::string file = !input.is_object() ? std::string()
                                     : name == "Read" ? str(input, "file_path")
                                     : name == "Bash" ? commandFile(str(input, "command"))
                                                      : std::string();
            if (!file.empty())
                entry["file"] = file;
            emit(std::move(entry));
        } else if (type == "user" && kind == "tool_result") {
            const auto it = toolNames_.find(str(block, "tool_use_id"));
            emit({{"kind", "result"}, {"tool", it == toolNames_.end() ? "" : it->second},
                  {"error", flag(block, "is_error")},
                  {"text", cut(resultText(member(block, "content")), kMaxResult)}});
        }
    }
}

} // namespace pvai
