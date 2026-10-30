#include "session.h"

#include <ctime>

namespace pvai {

const char *const kMicMark = "\xF0\x9F\x8E\x99 ";  // 🎙 + space
const char *const kSpeakMark = "\xF0\x9F\x94\x8A"; // 🔊

Session::Session(Host &host, std::string id, std::string cwd, std::string label)
    : host_(host)
    , id_(std::move(id))
    , cwd_(std::move(cwd))
    , label_(std::move(label))
{
    if (label_.empty())
        label_ = basename(cwd_);
}

void Session::setLabel(const std::string &label)
{
    if (label.empty() || label == label_)
        return;
    label_ = label;
    changed();
}

json Session::describe() const
{
    return {{"id", id_},
            {"assistant", assistant()},
            {"label", label_},
            {"cwd", cwd_},
            {"state", state_},
            {"queued", queue_.size()},
            {"draft", draft_},
            {"detached", detached()},
            {"restartable", restartable()},
            {"dialog", dialog_.empty() ? json() : json(dialog_)},
            {"usage", usage_},
            {"claudeSession", nullptr}};
}

bool Session::prompt(const Prompt &prompt)
{
    queue_.push_back(prompt);
    const bool typed = deliver();
    changed();
    return typed;
}

void Session::end()
{
    if (ended_)
        return;
    ended_ = true;
    changed();
}

bool Session::deliver()
{
    if (state_ != "idle" || draft_ || queue_.empty() || !canSubmit())
        return false;
    const Prompt prompt = queue_.front();
    queue_.pop_front();
    submit(prompt);
    return true;
}

void Session::say(const std::string &answer)
{
    std::string text = speakLine(answer);
    if (text.empty() && !voiceTurn_)
        text = firstSentence(answer);
    host_.broadcast({{"type", "say"}, {"session", id_}, {"text", text}, {"voice", voiceTurn_}});
    voiceTurn_ = false;
}

void Session::attention(const std::string &kind, const std::string &text)
{
    host_.broadcast({{"type", "attention"}, {"session", id_}, {"kind", kind}, {"text", text}});
}

void Session::historyReset()
{
    host_.toSubscribers(id_, {{"type", "history"}, {"session", id_}, {"entries", history(200)}});
}

void Session::sendEntries(std::vector<json> entries)
{
    for (auto &entry : entries)
        host_.toSubscribers(id_, {{"type", "entry"}, {"session", id_}, {"entry", std::move(entry)}});
}

Backend::Backend(Host &host)
    : host_(host)
{
}

json Backend::describe() const
{
    return {{"name", name()}, {"title", title()}, {"models", models()}};
}

void Backend::refreshVersion(std::function<void()> done)
{
    const std::string bin = binary();
    if (bin.empty()) {
        installed_.clear();
        if (done)
            done();
        return;
    }
    host_.runProcess({bin, "--version"}, std::chrono::seconds(30), [this, done](int status, const std::string &output) {
        if (status == 0)
            installed_ = versionIn(output);
        if (done)
            done();
    });
}

void Backend::update(std::function<void(const UpdateResult &)> done)
{
    const std::string bin = binary();
    if (bin.empty()) {
        done({installed_, installed_, "not installed"});
        return;
    }
    // One at a time; a second request while one runs only reports.
    if (updating_) {
        done({installed_, installed_, {}});
        return;
    }
    updating_ = true;
    std::vector<std::string> argv = {bin};
    for (const auto &arg : updateArgs())
        argv.push_back(arg);
    const std::string before = installed_;
    host_.runProcess(argv, std::chrono::minutes(10), [this, done, before](int status, const std::string &output) {
        std::string error;
        if (status != 0) {
            // The last line that says something.
            std::string text = trim(output);
            const auto newline = text.find_last_of('\n');
            error = oneLine(newline == std::string::npos ? text : text.substr(newline + 1), 200);
            if (error.empty())
                error = status < 0 ? "timed out" : "exit status " + std::to_string(status);
        }
        refreshVersion([this, done, before, error] {
            updating_ = false;
            host_.log(name() + " update: " + (error.empty() ? before + " -> " + installed_ : error));
            done({before, installed_, error});
        });
    });
}

std::string trim(const std::string &s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The last line that starts with 🔊 (after blanks, quote marks and markdown
// emphasis), without the marker.
std::string speakLine(const std::string &answer)
{
    std::string found;
    std::size_t start = 0;
    while (start <= answer.size()) {
        auto end = answer.find('\n', start);
        if (end == std::string::npos)
            end = answer.size();
        std::string line = answer.substr(start, end - start);
        const auto lead = line.find_first_not_of(" \t>*_");
        if (lead != std::string::npos && line.compare(lead, 4, kSpeakMark) == 0) {
            const std::string rest = line.substr(lead + 4);
            const auto a = rest.find_first_not_of(" \t*_");
            const auto b = rest.find_last_not_of(" \t\r*_");
            found = a == std::string::npos ? std::string() : rest.substr(a, b - a + 1);
        }
        start = end + 1;
    }
    return found;
}

// The first sentence of an answer as plain text, at most 200 bytes, cut at a
// character boundary.
std::string firstSentence(const std::string &answer)
{
    std::string plain;
    bool inFence = false;
    std::size_t start = 0;
    while (start < answer.size() && plain.size() < 400) {
        auto end = answer.find('\n', start);
        if (end == std::string::npos)
            end = answer.size();
        const std::string line = trim(answer.substr(start, end - start));
        start = end + 1;
        if (line.rfind("```", 0) == 0) {
            inFence = !inFence;
            continue;
        }
        if (inFence || line.empty() || line[0] == '|')
            continue;
        for (const char c : line)
            if (c != '*' && c != '`' && c != '#' && c != '_')
                plain += c;
        plain += ' ';
    }
    plain = trim(plain);
    for (std::size_t i = 0; i < plain.size(); ++i) {
        if ((plain[i] == '.' || plain[i] == '!' || plain[i] == '?' || plain[i] == ':')
            && (i + 1 == plain.size() || plain[i + 1] == ' ')) {
            plain.resize(i + 1);
            break;
        }
    }
    if (plain.size() > 200) {
        std::size_t cut = 200;
        while (cut > 0 && (static_cast<unsigned char>(plain[cut]) & 0xC0) == 0x80)
            --cut;
        plain.resize(cut);
        plain += "...";
    }
    return plain;
}

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

std::string oneLine(const std::string &text, std::size_t limit)
{
    std::string out;
    for (const char c : text)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f)
            out += c;
    if (out.size() > limit) {
        std::size_t at = limit;
        while (at > 0 && (static_cast<unsigned char>(out[at]) & 0xC0) == 0x80)
            --at;
        out.resize(at);
    }
    return trim(out);
}

std::string str(const json &object, const char *key)
{
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>()
                                                                                 : std::string();
}

std::string basename(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string isoTime(long long ms)
{
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buffer[40];
    const std::size_t n = std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S", &tm);
    std::snprintf(buffer + n, sizeof buffer - n, ".%03dZ", static_cast<int>(ms % 1000));
    return buffer;
}

std::string versionIn(const std::string &output)
{
    std::size_t at = 0;
    while (at < output.size()) {
        const auto start = output.find_first_not_of(" \t\r\n", at);
        if (start == std::string::npos)
            break;
        const auto end = output.find_first_of(" \t\r\n", start);
        const std::string word = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (std::isdigit(static_cast<unsigned char>(word[0])))
            return word;
        at = end;
    }
    return {};
}

} // namespace pvai
