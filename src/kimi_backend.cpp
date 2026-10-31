#include "kimi_backend.h"

#include "transcript.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>

namespace pvai {

namespace {

constexpr auto kRetry = std::chrono::seconds(10);
constexpr int kAuthRequired = -32000;
constexpr std::size_t kMaxAgentLine = 16 * 1024 * 1024; // session/load plays a whole history back

std::string kimiBinary()
{
    if (const char *chosen = std::getenv("PVAI_KIMI")) // an empty value: none
        return *chosen && ::access(chosen, X_OK) == 0 ? chosen : std::string();
    return findBinary("kimi");
}

std::string nowIso()
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    return isoTime(now.count());
}

// ACP's updatedAt: an ISO 8601 string (or, to be safe, Unix seconds or ms).
long long unixTimeOf(const json &value)
{
    if (value.is_number())
        return value.get<long long>() > 100000000000LL ? value.get<long long>() / 1000 : value.get<long long>();
    if (!value.is_string())
        return 0;
    std::tm tm{};
    if (std::sscanf(value.get<std::string>().c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<long long>(timegm(&tm));
}

// The text of a content block, or of a list of tool call contents.
std::string textOf(const json &content)
{
    if (content.is_object()) {
        if (str(content, "type") == "text")
            return str(content, "text");
        if (str(content, "type") == "content")
            return textOf(content.value("content", json()));
        if (str(content, "type") == "diff")
            return str(content, "path");
        return {};
    }
    std::string out;
    if (content.is_array())
        for (const auto &part : content)
            if (const std::string text = textOf(part); !text.empty())
                out += (out.empty() ? "" : "\n") + text;
    return out;
}

// A tool call's kind as the name the app shows (Claude Code's and Codex's
// where they have one).
std::string toolName(const json &call)
{
    const std::string kind = str(call, "kind");
    if (kind == "execute")
        return "shell";
    if (kind == "edit" || kind == "delete" || kind == "move")
        return "edit";
    if (kind.empty() || kind == "other")
        return "tool";
    return kind; // read, search, fetch, think, switch_mode
}

// What a tool call does, in one line: the command, else its title.
std::string toolText(const json &call)
{
    const json input = call.value("rawInput", json());
    if (input.is_object()) {
        for (const char *key : {"command", "cmd", "path", "file_path", "url", "pattern", "query"})
            if (input.contains(key) && input[key].is_string())
                return input[key].get<std::string>();
    }
    return str(call, "title");
}

// The first option of a permission request with one of these kinds.
std::string optionOf(const json &params, std::initializer_list<const char *> kinds)
{
    const json options = params.value("options", json());
    if (!options.is_array())
        return {};
    for (const char *kind : kinds)
        for (const auto &option : options)
            if (str(option, "kind") == kind)
                return str(option, "optionId");
    return {};
}

// A question bridged as a permission request (q1_opt_0 ... q1_skip), not a
// tool's approval.
bool isQuestion(const json &params)
{
    const json options = params.value("options", json());
    if (!options.is_array())
        return false;
    for (const auto &option : options)
        if (str(option, "optionId").rfind("q", 0) == 0 && str(option, "optionId").find("_opt_") != std::string::npos)
            return true;
    return false;
}

} // namespace

// ---- KimiSession ----

KimiSession::KimiSession(Host &host, KimiBackend &backend, const std::string &id, const std::string &cwd)
    : Session(host, id, cwd, {})
    , backend_(backend)
{
    state_ = "starting";
    usage_ = {{"model", nullptr},
              {"context", {{"percent", nullptr}, {"tokens", 0}, {"size", nullptr}}},
              {"fiveHour", nullptr},
              {"sevenDay", nullptr},
              {"costUsd", nullptr},
              {"version", backend.agentVersion().empty() ? json() : json(backend.agentVersion())}};
}

bool KimiSession::canSubmit() const
{
    return backend_.ready();
}

void KimiSession::submit(const Prompt &prompt)
{
    state_ = "busy";
    turnRunning_ = true;
    voiceTurn_ = prompt.voice;
    answer_.clear();
    const std::string text = (prompt.voice ? kMicMark : "") + prompt.text;
    json blocks = json::array({{{"type", "text"}, {"text", text}}});
    // The 🔊 convention goes with the first prompt as embedded context: it
    // is not part of the conversation Kimi plays back.
    if (!voiceSent_) {
        blocks.push_back({{"type", "resource"},
                          {"resource", {{"uri", "pvai://voice-context"}, {"mimeType", "text/plain"}, {"text", kVoiceContext}}}});
        voiceSent_ = true;
    }
    pushEntries({{{"kind", "prompt"}, {"text", cut(text, kMaxText)}, {"id", "p" + randomHex(6)}, {"time", nowIso()}}});
    Host &host = host_;
    const std::string id = id_;
    backend_.call("session/prompt", {{"sessionId", id_}, {"prompt", blocks}}, [&host, id](const json &result, const json &error) {
        if (auto *self = dynamic_cast<KimiSession *>(host.session(id)))
            self->turnEnded(result, error);
    });
}

// The answer to session/prompt: the turn is over.
void KimiSession::turnEnded(const json &result, const json &error)
{
    flushText();
    turnRunning_ = false;
    tools_.clear();
    if (!request_.is_null())
        reply({});
    backend_.noteAuth(error);
    if (!error.is_null()) {
        const bool login = error.value("code", 0) == kAuthRequired;
        const std::string message = login ? "Kimi Code is not logged in." : "Kimi Code: " + str(error, "message");
        pushEntries({{{"kind", "text"}, {"text", message}, {"id", "e" + randomHex(6)}, {"time", nowIso()}}});
        if (login)
            attention("login", message);
        say(message);
    } else if (str(result, "stopReason") == "cancelled") {
        pushEntries({{{"kind", "interrupted"}, {"id", "i" + randomHex(6)}, {"time", nowIso()}}});
        voiceTurn_ = false;
    } else {
        say(answer_);
    }
    state_ = "idle";
    dialog_.clear();
    deliver();
    changed();
}

std::string KimiSession::answer(const std::string &choice)
{
    if (request_.is_null())
        return "no-dialog";
    const json params = request_.value("params", json());
    std::string option;
    if (choice == "allow")
        option = optionOf(params, {"allow_once", "allow_always"});
    else if (choice == "deny")
        option = optionOf(params, {"reject_once", "reject_always"});
    else if (choice == "always")
        option = optionOf(params, {"allow_always"});
    else if (choice.rfind("option:", 0) == 0) {
        const std::string wanted = choice.substr(7);
        const json options = params.value("options", json());
        if (options.is_array())
            for (const auto &o : options)
                if (str(o, "optionId") == wanted)
                    option = wanted;
    }
    if (option.empty())
        return "bad-choice";
    reply(option);
    changed();
    return {};
}

void KimiSession::reply(const std::string &optionId)
{
    const json id = request_["id"];
    request_ = nullptr;
    backend_.respond(id, {{"outcome", optionId.empty() ? json{{"outcome", "cancelled"}}
                                                       : json{{"outcome", "selected"}, {"optionId", optionId}}}});
    if (state_ == "waiting") {
        state_ = "busy";
        dialog_.clear();
    }
}

std::string KimiSession::stop()
{
    if (turnRunning_)
        backend_.notify("session/cancel", {{"sessionId", id_}});
    backend_.call("session/close", {{"sessionId", id_}});
    end();
    return {};
}

void KimiSession::interrupt()
{
    if (!request_.is_null())
        reply({});
    if (turnRunning_)
        backend_.notify("session/cancel", {{"sessionId", id_}});
    changed();
}

json KimiSession::history(int limit)
{
    const std::size_t n = std::min<std::size_t>(entries_.size(), static_cast<std::size_t>(std::max(0, limit)));
    json out = json::array();
    for (auto it = entries_.end() - static_cast<long>(n); it != entries_.end(); ++it)
        out.push_back(*it);
    return out;
}

void KimiSession::pushEntries(std::vector<json> fresh)
{
    for (const auto &entry : fresh)
        entries_.push_back(entry);
    while (entries_.size() > TranscriptTail::kKeep)
        entries_.pop_front();
    if (!replaying_)
        sendEntries(std::move(fresh));
}

// A streamed message is one entry once it is complete: when something else
// follows, or the turn ends.
void KimiSession::flushText()
{
    if (chunkKind_.empty())
        return;
    const std::string kind = chunkKind_, text = chunk_;
    chunkKind_.clear();
    chunk_.clear();
    if (trim(text).empty())
        return;
    if (kind == "text")
        answer_ = text;
    pushEntries({{{"kind", kind}, {"text", cut(text, kMaxText)}, {"id", id_ + ":" + std::to_string(++chunkCount_)}, {"time", chunkTime_}}});
}

void KimiSession::onUpdate(const json &update)
{
    const std::string kind = str(update, "sessionUpdate");
    if (kind == "agent_message_chunk" || kind == "user_message_chunk") {
        const std::string as = kind == "agent_message_chunk" ? "text" : "prompt";
        // Live, a prompt is ours and already in the history.
        if (as == "prompt" && !replaying_)
            return;
        if (chunkKind_ != as) {
            flushText();
            chunkKind_ = as;
            chunkTime_ = nowIso();
        }
        chunk_ += textOf(update.value("content", json()));
    } else if (kind == "tool_call") {
        flushText();
        const std::string id = str(update, "toolCallId"), name = toolName(update);
        tools_[id] = name;
        const std::string text = toolText(update);
        json entry = {{"kind", "tool"}, {"tool", name}, {"text", cut(text, 300)}, {"id", id}, {"time", nowIso()}};
        if (name == "shell")
            if (const std::string file = commandFile(text); !file.empty())
                entry["file"] = file;
        pushEntries({std::move(entry)});
        const std::string status = str(update, "status");
        if (status == "completed" || status == "failed")
            onUpdate({{"sessionUpdate", "tool_call_update"}, {"toolCallId", id}, {"status", status}, {"content", update.value("content", json())}});
    } else if (kind == "tool_call_update") {
        const std::string status = str(update, "status"), id = str(update, "toolCallId");
        if (status != "completed" && status != "failed")
            return;
        const auto it = tools_.find(id);
        const std::string name = it == tools_.end() ? "tool" : it->second;
        if (it != tools_.end())
            tools_.erase(it);
        std::string text = textOf(update.value("content", json()));
        if (text.empty() && update.contains("rawOutput") && !update["rawOutput"].is_null())
            text = update["rawOutput"].is_string() ? update["rawOutput"].get<std::string>() : update["rawOutput"].dump();
        if (!text.empty() || status == "failed")
            pushEntries({{{"kind", "result"}, {"tool", name}, {"text", cut(text, kMaxResult)}, {"error", status == "failed"},
                          {"id", id + ":result"}, {"time", nowIso()}}});
    } else if (kind == "usage_update") {
        const json used = update.value("used", json()), size = update.value("size", json());
        json percent = nullptr;
        if (used.is_number() && size.is_number() && size.get<long long>() > 0)
            percent = (used.get<long long>() * 100 + size.get<long long>() / 2) / size.get<long long>();
        const json context = {{"percent", percent}, {"tokens", used.is_number() ? used : json(0)}, {"size", size}};
        if (context != usage_["context"]) {
            usage_["context"] = context;
            usageChanged();
        }
    } else if (kind == "config_option_update") {
        setConfig(update);
    } else if (kind == "session_info_update") {
        if (const std::string title = str(update, "title"); !title.empty() && label().empty())
            setLabel(cut(oneLine(title, kTitleMax * 4), kTitleMax));
    }
}

// Kimi asks to run a tool (or a question bridged as one).
void KimiSession::onPermission(const json &requestId, const json &params)
{
    flushText();
    if (!request_.is_null()) // a second one replaces it; Kimi asks one at a time
        reply({});
    request_ = {{"id", requestId}, {"params", params}};
    const json call = params.value("toolCall", json::object());
    const bool question = isQuestion(params);
    json options = json::array();
    if (params.contains("options") && params["options"].is_array())
        for (const auto &o : params["options"])
            options.push_back({{"id", str(o, "optionId")}, {"name", str(o, "name")}, {"kind", str(o, "kind")}});
    state_ = "waiting";
    dialog_ = question ? "question" : "permission";
    std::string text = question ? str(call, "title") : toolText(call);
    if (text.empty())
        text = textOf(call.value("content", json()));
    dialogInfo_ = dialogInfo(question ? "" : toolName(call), text, question ? textOf(call.value("content", json())) : str(call, "title"));
    dialogInfo_["options"] = options;
    attention(dialog_, question ? text : toolName(call) + ": " + text);
    changed();
}

// The model from the session's config options (session/new, load,
// config_option_update): the `model` option's current value, by name.
void KimiSession::setConfig(const json &result)
{
    const json options = result.value("configOptions", json());
    if (!options.is_array())
        return;
    for (const auto &option : options) {
        if (str(option, "id") != "model")
            continue;
        std::string model = str(option, "currentValue");
        const json values = option.value("options", json());
        if (values.is_array())
            for (const auto &v : values)
                if (str(v, "value") == model && !str(v, "name").empty())
                    model = str(v, "name");
        if (!model.empty() && usage_["model"] != json(model)) {
            usage_["model"] = model;
            usageChanged();
        }
    }
}

void KimiSession::setVersion(const std::string &version)
{
    if (usage_.value("version", json()) == json(version))
        return;
    usage_["version"] = version;
    usageChanged();
}

void KimiSession::loaded()
{
    flushText();
    tools_.clear();
    replaying_ = false;
    state_ = "idle";
    historyReset();
    deliver();
    changed();
}

void KimiSession::usageChanged()
{
    host_.broadcast({{"type", "usage"}, {"session", id_}, {"usage", usage_}});
}

// ---- KimiBackend ----

KimiBackend::KimiBackend(Host &host)
    : Backend(host)
    , bin_(kimiBinary())
{
}

KimiBackend::~KimiBackend()
{
    if (child_ > 0) {
        ::kill(child_, SIGTERM);
        ::waitpid(child_, nullptr, 0);
    }
}

void KimiBackend::findProgram()
{
    if (bin_.empty())
        bin_ = kimiBinary();
}

// Installed with npm, which pvai leaves alone: an update changes nothing.
void KimiBackend::update(std::function<void(const UpdateResult &)> done)
{
    done({installed_, installed_, bin_.empty() ? "not installed" : ""});
}

int KimiBackend::fd() const
{
    return conn_ ? conn_->fd() : -1;
}

short KimiBackend::events() const
{
    return static_cast<short>(POLLIN | (conn_ && conn_->wantsWrite() ? POLLOUT : 0));
}

void KimiBackend::call(const std::string &method, const json &params, Done done)
{
    if (!conn_) {
        if (done)
            done(json::object(), {{"code", -32603}, {"message", "Kimi Code is not running"}});
        return;
    }
    const long long id = nextId_++;
    conn_->send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    if (done)
        pending_[id] = std::move(done);
}

void KimiBackend::notify(const std::string &method, const json &params)
{
    if (conn_)
        conn_->send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void KimiBackend::respond(const json &id, const json &result)
{
    if (conn_)
        conn_->send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void KimiBackend::noteAuth(const json &error)
{
    const json now = error.is_null() ? json(true) : error.value("code", 0) == kAuthRequired ? json(false) : loggedIn_;
    if (now != loggedIn_) {
        loggedIn_ = now;
        host_.log(std::string("kimi: ") + (now == json(true) ? "logged in" : "not logged in"));
    }
}

// `kimi acp` on a socket pair: its stdin and stdout are one end, the daemon
// holds the other. Its stderr goes to kimi.log beside the daemon's log.
bool KimiBackend::spawn()
{
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) // SOCK_CLOEXEC is Linux only
        return false;
    setCloseOnExec(pair[0]);
    setCloseOnExec(pair[1]);
    const std::string log = runtimeDir() + "/kimi.log";
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pair[0]);
        ::close(pair[1]);
        return false;
    }
    if (pid == 0) {
        ::setsid();
        const char *home = std::getenv("HOME");
        if (::chdir(home ? home : "/") != 0)
            ::_exit(1);
        dropClaudeEnvironment();
        ::dup2(pair[1], STDIN_FILENO);
        ::dup2(pair[1], STDOUT_FILENO);
        const int err = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (err >= 0)
            ::dup2(err, STDERR_FILENO);
        ::execl(bin_.c_str(), bin_.c_str(), "acp", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(pair[1]);
    setNonBlocking(pair[0]);
    child_ = pid;
    conn_ = std::make_unique<Conn>(pair[0]);
    conn_->setMaxLine(kMaxAgentLine);
    return true;
}

void KimiBackend::tick(Clock::time_point now)
{
    if (bin_.empty())
        return;
    if (conn_) {
        if (!conn_->flush())
            lost();
        return;
    }
    if (now < nextTry_)
        return;
    nextTry_ = now + kRetry;
    if (!spawn())
        return;
    ready_ = false;
    call("initialize",
         {{"protocolVersion", 1},
          {"clientCapabilities", {{"fs", {{"readTextFile", false}, {"writeTextFile", false}}}, {"terminal", false}}},
          {"clientInfo", {{"name", "pvai"}, {"title", "PhiVoice AI"}, {"version", kVersion}}}},
         [this](const json &result, const json &error) {
             if (!error.is_null()) {
                 host_.log("kimi: initialize: " + error.dump());
                 lost();
                 return;
             }
             ready_ = true;
             agentVersion_ = str(result.value("agentInfo", json()), "version");
             for (Session *session : host_.sessions())
                 if (auto *kimi = dynamic_cast<KimiSession *>(session))
                     kimi->setVersion(agentVersion_);
         });
}

std::optional<Clock::time_point> KimiBackend::deadline() const
{
    if (bin_.empty() || conn_)
        return std::nullopt;
    return nextTry_;
}

// Kimi ended (or its link broke): its sessions go with it; the next tick
// starts it again, and `start.resume` brings a conversation back.
void KimiBackend::lost()
{
    if (child_ > 0) {
        ::kill(child_, SIGTERM);
        ::waitpid(child_, nullptr, 0);
        child_ = -1;
    }
    conn_.reset();
    ready_ = false;
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto &[id, done] : pending)
        done(json::object(), {{"code", -32603}, {"message", "Kimi Code ended"}});
    for (Session *session : host_.sessions())
        if (auto *kimi = dynamic_cast<KimiSession *>(session))
            kimi->end();
    nextTry_ = Clock::now() + std::chrono::seconds(2);
}

void KimiBackend::service()
{
    if (!conn_)
        return;
    std::vector<json> messages;
    const bool alive = conn_->readMessages(messages);
    for (const auto &message : messages) {
        if (!message.is_object())
            continue;
        const std::string method = str(message, "method");
        auto object = [&](const char *key) {
            const json value = message.contains(key) ? message[key] : json();
            return value.is_object() ? value : json::object();
        };
        try {
            if (method.empty()) {
                const json id = message.contains("id") ? message["id"] : json();
                const auto it = id.is_number_integer() ? pending_.find(id.get<long long>()) : pending_.end();
                if (it == pending_.end())
                    continue;
                auto done = std::move(it->second);
                pending_.erase(it);
                const json error = message.contains("error") && !message["error"].is_null() ? object("error") : json();
                done(object("result"), error);
            } else if (message.contains("id")) {
                onRequest(message["id"], method, object("params"));
            } else if (method == "session/update") {
                const json params = object("params");
                if (auto *session = dynamic_cast<KimiSession *>(host_.session(str(params, "sessionId"))))
                    session->onUpdate(params.value("update", json::object()));
            }
        } catch (const json::exception &e) {
            host_.log(std::string("kimi: ") + method + ": " + e.what());
        }
        if (!conn_)
            return;
    }
    if (!alive || !conn_->flush())
        lost();
}

// What Kimi asks of its client: permissions are the owner's; files and
// terminals are not offered (clientCapabilities), anything else is unknown.
void KimiBackend::onRequest(const json &id, const std::string &method, const json &params)
{
    if (method == "session/request_permission") {
        if (auto *session = dynamic_cast<KimiSession *>(host_.session(str(params, "sessionId")))) {
            session->onPermission(id, params);
            return;
        }
        respond(id, {{"outcome", {{"outcome", "cancelled"}}}});
        return;
    }
    if (conn_)
        conn_->send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "not supported by pvai"}}}});
}

KimiSession *KimiBackend::addSession(const std::string &id, const std::string &cwd)
{
    if (auto *existing = dynamic_cast<KimiSession *>(host_.session(id)))
        return existing;
    auto owned = std::make_unique<KimiSession>(host_, *this, id, cwd);
    KimiSession *session = owned.get();
    host_.addSession(std::move(owned));
    return session;
}

// The models `start` can ask for: the values of the `model` option.
void KimiBackend::setModels(const json &result)
{
    const json options = result.value("configOptions", json());
    if (!options.is_array())
        return;
    for (const auto &option : options) {
        if (str(option, "id") != "model" || !option.contains("options") || !option["options"].is_array())
            continue;
        json models = json::array();
        for (const auto &v : option["options"])
            if (!str(v, "value").empty())
                models.push_back(str(v, "value"));
        models_ = models;
    }
}

void KimiBackend::conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done)
{
    if (!ready_)
        return done({});
    call("session/list", {{"cwd", cwd}}, [this, done](const json &result, const json &) {
        std::vector<Conversation> out;
        if (result.contains("sessions") && result["sessions"].is_array())
            for (const auto &s : result["sessions"]) {
                const std::string id = str(s, "sessionId");
                if (id.empty() || host_.session(id))
                    continue;
                std::string title = str(s, "title");
                if (title.rfind(kMicMark, 0) == 0)
                    title = title.substr(std::string(kMicMark).size());
                title = cut(oneLine(title, kTitleMax * 4), kTitleMax);
                if (title.empty())
                    continue;
                out.push_back({id, title, unixTimeOf(s.value("updatedAt", json()))});
            }
        std::sort(out.begin(), out.end(), [](const auto &x, const auto &y) { return x.at > y.at; });
        if (out.size() > kMaxConversations)
            out.resize(kMaxConversations);
        done(std::move(out));
    });
}

// A new session in `cwd`, or an earlier one played back (session/load);
// `args` may name the model (`-m`, `--model`).
void KimiBackend::start(const StartRequest &request, Started done)
{
    if (!ready_)
        return done({}, "no-assistant");
    std::string model;
    for (std::size_t i = 0; i + 1 < request.args.size(); ++i)
        if (request.args[i] == "-m" || request.args[i] == "--model")
            model = request.args[i + 1];
    const std::string label = request.label, cwd = request.cwd;
    auto failed = [this, done](const json &error, const char *reason) {
        noteAuth(error);
        done({}, error.value("code", 0) == kAuthRequired ? "not-logged-in" : reason);
    };
    auto withModel = [this, model](KimiSession *session) {
        if (model.empty())
            return;
        Host &host = host_;
        const std::string id = session->id();
        call("session/set_config_option", {{"sessionId", id}, {"configId", "model"}, {"value", model}},
             [&host, id](const json &result, const json &) {
                 if (auto *self = dynamic_cast<KimiSession *>(host.session(id)))
                     self->setConfig(result);
             });
    };
    const json params = {{"cwd", cwd}, {"mcpServers", json::array()}};
    if (request.resume.empty()) {
        call("session/new", params, [this, label, cwd, done, failed, withModel](const json &result, const json &error) {
            const std::string id = str(result, "sessionId");
            if (!error.is_null() || id.empty())
                return failed(error, "bad-start");
            noteAuth(error);
            setModels(result);
            KimiSession *session = addSession(id, cwd);
            session->setLabel(label);
            session->setConfig(result);
            session->loaded();
            withModel(session);
            done(id, {});
        });
        return;
    }
    const std::string id = request.resume;
    if (host_.session(id)) {
        host_.session(id)->setLabel(label);
        return done(id, {});
    }
    // The session exists before the call: the history Kimi plays back lands
    // in it.
    KimiSession *session = addSession(id, cwd);
    session->setLabel(label);
    Host &host = host_;
    call("session/load", {{"sessionId", id}, {"cwd", cwd}, {"mcpServers", json::array()}},
         [this, &host, id, done, failed, withModel](const json &result, const json &error) {
             auto *self = dynamic_cast<KimiSession *>(host.session(id));
             if (!error.is_null()) {
                 if (self)
                     self->end();
                 return failed(error, "unknown-conversation");
             }
             if (!self)
                 return done({}, "unknown-conversation");
             noteAuth(error);
             setModels(result);
             self->setConfig(result);
             self->loaded();
             withModel(self);
             done(id, {});
         });
}

} // namespace pvai
