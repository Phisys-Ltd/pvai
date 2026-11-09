#include "session.h"

#include "transcript.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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
    if (label.empty() || label == label_ || renamed_)
        return;
    label_ = label;
    changed();
}

void Session::applyLabel(const std::string &label)
{
    renamed_ = true;
    labelPushed(label);
    if (label.empty() || label == label_)
        return;
    label_ = label;
    changed();
}

json dialogInfo(const std::string &tool, const std::string &text, const std::string &message,
                const std::string &description)
{
    auto field = [](const std::string &value) { return value.empty() ? json() : json(cut(value, kDialogTextMax)); };
    json info = {{"tool", field(tool)}, {"text", field(text)}, {"message", field(message)}};
    if (!description.empty())
        info["description"] = cut(description, kDialogTextMax);
    return info;
}

void Session::modelEvent(const json &ref, const std::string &model, const std::string &state, const std::string &error)
{
    json event = {{"type", "model"}, {"ref", ref}, {"session", id_}, {"state", state}, {"model", model}};
    if (!error.empty())
        event["error"] = error;
    host_.broadcast(event);
}

void Session::noteModel(const std::string &model, const std::string &why)
{
    if (model.empty() || (usage_.is_object() && usage_.value("model", json()) == json(model)))
        return;
    const std::string before = usage_.is_object() && usage_.value("model", json()).is_string() ? usage_["model"].get<std::string>() : "";
    if (!usage_.is_object())
        usage_ = json::object();
    usage_["model"] = model;
    if (!why.empty())
        attention("model", (before.empty() ? "" : before + " \xE2\x86\x92 ") + model + " (" + why + ")");
    changed();
}

void Session::modeEvent(const json &ref, const std::string &state, const std::string &error)
{
    json event = {{"type", "mode"}, {"ref", ref}, {"session", id_}, {"state", state}, {"mode", mode_}};
    if (!error.empty())
        event["error"] = error;
    host_.broadcast(event);
}

void Session::changed()
{
    if (state_ != loggedState_) {
        host_.log(id_ + ": " + loggedState_ + " -> " + state_ + (dialog_.empty() ? "" : " (" + dialog_ + ")")
                  + ", queued " + std::to_string(queue_.size()));
        loggedState_ = state_;
    }
    host_.sessionsChanged();
}

void Session::usageChanged()
{
    host_.broadcast({{"type", "usage"}, {"session", id_}, {"usage", usage_}});
}

void Session::setVersion(const std::string &version)
{
    if (usage_.value("version", json()) == json(version))
        return;
    usage_["version"] = version;
    usageChanged();
}

void Session::resumeFromDialog()
{
    if (state_ == "waiting") {
        state_ = "busy";
        dialog_.clear();
    }
}

json BufferedSession::history(int limit)
{
    const std::size_t n = std::min<std::size_t>(entries_.size(), static_cast<std::size_t>(std::max(0, limit)));
    json out = json::array();
    for (auto it = entries_.end() - static_cast<long>(n); it != entries_.end(); ++it)
        out.push_back(*it);
    return out;
}

void BufferedSession::trimEntries()
{
    while (entries_.size() > TranscriptTail::kKeep)
        entries_.pop_front();
}

void BufferedSession::pushEntries(std::vector<json> fresh)
{
    for (const auto &entry : fresh)
        entries_.push_back(entry);
    trimEntries();
    if (!quiet())
        sendEntries(std::move(fresh));
}

json contextOf(const json &used, const json &size)
{
    json percent = nullptr;
    if (used.is_number() && size.is_number() && size.get<long long>() > 0)
        percent = (used.get<long long>() * 100 + size.get<long long>() / 2) / size.get<long long>();
    return {{"percent", percent}, {"tokens", used.is_number() ? used : json(0)}, {"size", size}};
}

std::string shortTitle(std::string_view text) { return cut(oneLine(text, kTitleMax * 4), kTitleMax); }

std::string conversationTitle(std::string text)
{
    if (text.rfind(kMicMark, 0) == 0)
        text = text.substr(std::string(kMicMark).size());
    return shortTitle(text);
}

void trimConversations(std::vector<Conversation> &list)
{
    std::sort(list.begin(), list.end(), [](const auto &x, const auto &y) { return x.at > y.at; });
    if (list.size() > kMaxConversations)
        list.resize(kMaxConversations);
}

std::optional<std::string> modelArgument(const std::vector<std::string> &args)
{
    std::optional<std::string> model;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "-m" || args[i] == "--model")
            model = args[i + 1];
    return model;
}

void sortByStart(json &entries)
{
    std::sort(entries.begin(), entries.end(), [](const json &a, const json &b) {
        return a.value("startedAt", 0LL) < b.value("startedAt", 0LL);
    });
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
            {"stoppable", stoppable()},
            {"restartable", restartable()},
            {"inputClearable", clearable()},
            {"dialog", dialog_.empty() ? json() : json(dialog_)},
            {"dialogInfo", dialog_.empty() || !dialogInfo_.is_object() ? json() : dialogInfo_},
            {"usage", usage_},
            {"mode", mode_},
            {"claudeSession", nullptr},
            {"wrapperVersion", nullptr}};
}

std::vector<Prompt> Session::unsent() const
{
    std::vector<Prompt> out;
    for (const auto &p : queue_)
        if (!p.modelSwitch)
            out.push_back(p);
    return out;
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

void Session::promptVoice(const std::string &raw, const Prompt *typed)
{
    const std::string text = stripPasteFrame(raw);
    if (typed) {
        if (typed->voice)
            spokenLast_ = true;
        else if (!typed->agent)
            spokenLast_ = false;
    } else if (text.rfind(kMicMark, 0) == 0) {
        spokenLast_ = true;
    } else if (!isMachinePrompt(text)) { // a message, a hand-back, a task notification: leaves the voice state
        spokenLast_ = false;
    }
    voiceTurn_ = spokenLast_;
    prompted_ = true;
}

void Session::say(const std::string &answer, bool exact)
{
    // A turn no prompt started (a hand-back, a task) is spoken as the owner's
    // last prompt was, whatever state it passed through on the way.
    if (!prompted_)
        voiceTurn_ = spokenLast_;
    std::string text = exact ? oneLine(answer, 200) : speakLine(answer);
    std::size_t words = 0;
    bool inWord = false;
    for (unsigned char c : text) {
        const bool space = std::isspace(c);
        if (!space && !inWord) ++words;
        inWord = !space;
    }
    host_.log(id_ + ": say voice=" + (voiceTurn_ ? "true" : "false")
              + (text.empty() ? ", no speak line"
                              : ", speak line " + std::to_string(words) + " words " + std::to_string(text.size()) + " bytes"));
    if (text.empty() && !voiceTurn_ && !exact)
        text = firstSentence(answer);
    // A 🔊 line is written for the ear: it is spoken whether the turn counts
    // as spoken or not. Without one, the turn decides as before.
    const bool voice = voiceTurn_ || (!exact && !text.empty() && !speakLine(answer).empty());
    host_.broadcast({{"type", "say"}, {"session", id_}, {"text", text}, {"voice", voice}});
    turnOver();
}

void Session::attention(const std::string &kind, const std::string &text)
{
    host_.broadcast({{"type", "attention"}, {"session", id_}, {"kind", kind}, {"text", text}});
}

void Session::loginAttention(const std::string &text)
{
    if (loginReported_)
        return;
    loginReported_ = true;
    attention("login", text);
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
    return {{"name", name()}, {"title", title()}, {"models", models()}, {"modes", modes()}, {"installed", installedHere()},
            {"loggedIn", loggedIn_}, {"account", accountName_}};
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
        plain.resize(utf8Boundary(plain, 200));
        plain += "...";
    }
    return plain;
}

std::string basename(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string nowIso()
{
    return isoTime(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void BufferedSession::failure(const std::string &assistant, const std::string &message, bool login)
{
    const std::string shown = login ? assistant + " is not logged in." : assistant + ": " + (message.empty() ? std::string("the turn failed") : message);
    pushEntries({{{"kind", "text"}, {"text", cut(shown, kMaxText)}, {"id", "e" + randomHex(6)}, {"time", nowIso()}}});
    if (login)
        loginAttention(shown);
    say(shown, true);
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
