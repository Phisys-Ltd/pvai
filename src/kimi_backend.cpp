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
    return unixTime(value.get<std::string>());
}

// The text of a content block, or of a list of tool call contents.
std::string textOf(const json &content)
{
    if (content.is_object()) {
        if (str(content, "type") == "text")
            return str(content, "text");
        if (str(content, "type") == "content")
            return textOf(member(content, "content"));
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
    if (auto text = firstString(member(call, "rawInput"), {"command", "cmd", "path", "file_path", "url", "pattern", "query"}))
        return *text;
    return str(call, "title");
}

// The first option of a permission request with one of these kinds.
std::string optionOf(const json &params, std::initializer_list<const char *> kinds)
{
    const json options = member(params, "options");
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
    const json options = member(params, "options");
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
    : BufferedSession(host, id, cwd, {})
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
    answer_.clear();
    const std::string text = (prompt.voice ? kMicMark : "") + prompt.text;
    promptVoice(text);
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
            loginAttention(message);
        say(message, true);
    } else if (str(result, "stopReason") == "cancelled") {
        pushEntries({{{"kind", "interrupted"}, {"id", "i" + randomHex(6)}, {"time", nowIso()}}});
        turnOver();
    } else {
        rearmLogin(); // a turn worked: the next login problem is news again
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
    const json params = member(request_, "params");
    std::string option;
    if (choice == "allow")
        option = optionOf(params, {"allow_once", "allow_always"});
    else if (choice == "deny")
        option = optionOf(params, {"reject_once", "reject_always"});
    else if (choice == "always")
        option = optionOf(params, {"allow_always"});
    else if (choice.rfind("option:", 0) == 0) {
        const std::string wanted = choice.substr(7);
        const json options = member(params, "options");
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
    resumeFromDialog();
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
        chunk_ += textOf(member(update, "content"));
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
            onUpdate({{"sessionUpdate", "tool_call_update"}, {"toolCallId", id}, {"status", status}, {"content", member(update, "content")}});
    } else if (kind == "tool_call_update") {
        const std::string status = str(update, "status"), id = str(update, "toolCallId");
        if (status != "completed" && status != "failed")
            return;
        const auto it = tools_.find(id);
        const std::string name = it == tools_.end() ? "tool" : it->second;
        if (it != tools_.end())
            tools_.erase(it);
        std::string text = textOf(member(update, "content"));
        if (text.empty() && update.contains("rawOutput") && !update["rawOutput"].is_null())
            text = update["rawOutput"].is_string() ? update["rawOutput"].get<std::string>() : update["rawOutput"].dump();
        if (!text.empty() || status == "failed")
            pushEntries({{{"kind", "result"}, {"tool", name}, {"text", cut(text, kMaxResult)}, {"error", status == "failed"},
                          {"id", id + ":result"}, {"time", nowIso()}}});
    } else if (kind == "usage_update") {
        const json used = member(update, "used"), size = member(update, "size");
        const json context = contextOf(used, size);
        if (context != usage_["context"]) {
            usage_["context"] = context;
            usageChanged();
        }
    } else if (kind == "current_mode_update") {
        if (const std::string mode = str(update, "currentModeId"); !mode.empty() && mode_ != json(mode)) {
            mode_ = mode;
            changed();
        }
    } else if (kind == "config_option_update") {
        setConfig(update);
    } else if (kind == "session_info_update") {
        if (const std::string title = str(update, "title"); !title.empty() && label().empty())
            setLabel(shortTitle(title));
    }
}

// Kimi asks to run a tool (or a question bridged as one).
void KimiSession::onPermission(const json &requestId, const json &params)
{
    flushText();
    if (!request_.is_null()) // a second one replaces it; Kimi asks one at a time
        reply({});
    request_ = {{"id", requestId}, {"params", params}};
    const json call = member(params, "toolCall");
    const bool question = isQuestion(params);
    json options = json::array();
    if (params.contains("options") && params["options"].is_array())
        for (const auto &o : params["options"])
            options.push_back({{"id", str(o, "optionId")}, {"name", str(o, "name")}, {"kind", str(o, "kind")}});
    state_ = "waiting";
    dialog_ = question ? "question" : "permission";
    std::string text = question ? str(call, "title") : toolText(call);
    if (text.empty())
        text = textOf(member(call, "content"));
    dialogInfo_ = dialogInfo(question ? "" : toolName(call), text, question ? textOf(member(call, "content")) : str(call, "title"));
    dialogInfo_["options"] = options;
    attention(dialog_, question ? text : toolName(call) + ": " + text);
    changed();
}

// The model from the session's config options (session/new, load,
// config_option_update): the `model` option's current value, by name.
std::string KimiSession::setMode(const std::string &mode, const json &ref)
{
    bool known = false;
    for (const auto &m : backend_.modes())
        known = known || str(m, "id") == mode;
    if (!known)
        return "unknown-mode";
    Host &host = host_;
    const std::string id = id_;
    backend_.call("session/set_mode", {{"sessionId", id_}, {"modeId", mode}}, [&host, id, mode, ref](const json &, const json &error) {
        auto *self = dynamic_cast<KimiSession *>(host.session(id));
        if (!self)
            return;
        if (!error.is_null())
            return self->modeEvent(ref, "failed", str(error, "message"));
        self->mode_ = mode;
        self->changed();
        self->modeEvent(ref, "done");
    });
    return {};
}

// Kimi's `model` option; the answer has the options again, with the new
// current value.
std::string KimiSession::setModel(const std::string &model, const json &ref)
{
    Host &host = host_;
    const std::string id = id_;
    backend_.call("session/set_config_option", {{"sessionId", id_}, {"configId", "model"}, {"value", model}},
                  [&host, id, model, ref](const json &result, const json &error) {
                      auto *self = dynamic_cast<KimiSession *>(host.session(id));
                      if (!self)
                          return;
                      if (!error.is_null())
                          return self->modelEvent(ref, model, "failed", str(error, "message"));
                      self->setConfig(result);
                      self->modelEvent(ref, model, "done");
                  });
    return {};
}

void KimiSession::setConfig(const json &result)
{
    if (const json modes = member(result, "modes"); modes.is_object() && !str(modes, "currentModeId").empty())
        mode_ = str(modes, "currentModeId");
    const json options = member(result, "configOptions");
    if (!options.is_array())
        return;
    for (const auto &option : options) {
        if (str(option, "id") != "model")
            continue;
        std::string model = str(option, "currentValue");
        const json values = member(option, "options");
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

// ---- KimiLink ----

bool KimiLink::open(const std::string &program)
{
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) // SOCK_CLOEXEC is Linux only
        return false;
    setCloseOnExec(pair[0]);
    setCloseOnExec(pair[1]);
    const std::string log = runtimeDir() + "/kimi.log";
    rotateLog(log);
    ChildOptions options;
    options.argv = {program, "acp"};
    options.newSession = true;
    options.cwd = homeDir();
    options.in = options.out = Stdio::descriptor(pair[1]);
    options.err = Stdio::append(log);
    const pid_t pid = spawnChild(options);
    if (pid < 0) {
        ::close(pair[0]);
        ::close(pair[1]);
        return false;
    }
    ::close(pair[1]);
    setNonBlocking(pair[0]);
    child_ = pid;
    conn_ = std::make_unique<Conn>(pair[0]);
    conn_->setMaxLine(kMaxAgentLine);
    return true;
}

void KimiLink::close()
{
    if (child_ > 0) {
        ::kill(child_, SIGTERM);
        ::waitpid(child_, nullptr, 0);
        child_ = -1;
    }
    conn_.reset();
}

long long KimiLink::request(const std::string &method, const json &params)
{
    const long long id = nextId_++;
    conn_->send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    return id;
}

void KimiLink::notify(const std::string &method, const json &params)
{
    if (conn_)
        conn_->send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void KimiLink::respond(const json &id, const json &result)
{
    if (conn_)
        conn_->send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void KimiLink::respondError(const json &id, int code, const std::string &message)
{
    if (conn_)
        conn_->send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

bool KimiLink::read(std::vector<json> &messages)
{
    return conn_ && conn_->readMessages(messages);
}

// ---- KimiBackend ----

KimiBackend::KimiBackend(Host &host)
    : RpcBackend(host)
{
    bin_ = kimiBinary();
}

std::string KimiBackend::locate() const
{
    return kimiBinary();
}

// Installed with npm, which pvai leaves alone: an update changes nothing.
void KimiBackend::update(std::function<void(const UpdateResult &)> done)
{
    done({installed_, installed_, bin_.empty() ? "not installed" : ""});
}

void KimiBackend::noteAuth(const json &error)
{
    const json now = error.is_null() ? json(true) : integer(error, "code") == kAuthRequired ? json(false) : loggedIn_;
    if (now != loggedIn_) {
        loggedIn_ = now;
        host_.log(std::string("kimi: ") + (now == json(true) ? "logged in" : "not logged in"));
    }
}

void KimiBackend::tick(Clock::time_point now)
{
    if (bin_.empty())
        return;
    if (link_.isOpen()) {
        if (!link_.flush())
            lost();
        return;
    }
    if (!retryDue(now))
        return;
    if (!link_.open(bin_))
        return;
    ready_ = false;
    initialize({{"protocolVersion", 1},
                {"clientCapabilities", {{"fs", {{"readTextFile", false}, {"writeTextFile", false}}}, {"terminal", false}}},
                {"clientInfo", {{"name", "pvai"}, {"title", "PhiVoice AI"}, {"version", kVersion}}}});
}

void KimiBackend::initializeFailed(const json &error)
{
    host_.log("kimi: initialize: " + error.dump());
}

void KimiBackend::initialized(const json &result)
{
    agentVersion_ = str(member(result, "agentInfo"), "version");
    for (KimiSession *session : sessionsOf<KimiSession>())
        session->setVersion(agentVersion_);
}

std::optional<Clock::time_point> KimiBackend::deadline() const
{
    if (bin_.empty() || link_.isOpen())
        return std::nullopt;
    return nextTry_;
}

// Kimi ended (or its link broke): its sessions go with it; the next tick
// starts it again, and `start.resume` brings a conversation back.
void KimiBackend::onLost()
{
    for (KimiSession *session : sessionsOf<KimiSession>())
        session->end();
}

void KimiBackend::service()
{
    if (!link_.isOpen())
        return;
    std::vector<json> messages;
    const bool alive = link_.read(messages);
    for (const auto &message : messages) {
        if (!message.is_object())
            continue;
        const std::string method = str(message, "method");
        auto object = [&](const char *key) -> const json & { return objectIn(message, key); };
        try {
            if (method.empty()) {
                dispatchResponse(message);
            } else if (message.contains("id")) {
                onRequest(message["id"], method, object("params"));
            } else if (method == "session/update") {
                const json &params = object("params");
                if (auto *session = dynamic_cast<KimiSession *>(host_.session(str(params, "sessionId"))))
                    session->onUpdate(member(params, "update"));
            }
        } catch (const json::exception &e) {
            host_.log(std::string("kimi: ") + method + ": " + e.what());
        }
        if (!link_.isOpen())
            return;
    }
    if (!alive || !link_.flush())
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
    respondError(id, -32601, "not supported by pvai");
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

// The modes a session can be in, as Kimi lists them (session/new, load).
void KimiBackend::setModes(const json &result)
{
    const json modes = member(result, "modes");
    if (!modes.is_object() || !modes.contains("availableModes") || !modes["availableModes"].is_array())
        return;
    json list = json::array();
    // Short names, the same across assistants, for the ones known; Kimi's
    // own for the rest.
    static const std::map<std::string, std::string> names = {{"default", "Ask"}, {"plan", "Plan"}, {"auto", "Auto"}, {"yolo", "YOLO"}};
    for (const auto &m : modes["availableModes"]) {
        const std::string id = str(m, "id");
        if (id.empty())
            continue;
        const auto known = names.find(id);
        list.push_back({{"id", id}, {"name", known != names.end() ? known->second : str(m, "name").empty() ? id : str(m, "name")}});
    }
    if (!list.empty())
        modes_ = list;
}

// The models `start` can ask for: the values of the `model` option.
void KimiBackend::setModels(const json &result)
{
    const json options = member(result, "configOptions");
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
                title = conversationTitle(std::move(title));
                if (title.empty())
                    continue;
                out.push_back({id, title, unixTimeOf(member(s, "updatedAt"))});
            }
        trimConversations(out);
        done(std::move(out));
    });
}

// A new session in `cwd`, or an earlier one played back (session/load);
// `args` may name the model (`-m`, `--model`).
void KimiBackend::start(const StartRequest &request, Started done)
{
    if (!ready_)
        return done({}, "no-assistant");
    const std::string model = modelArgument(request.args).value_or("");
    const std::string label = request.label, cwd = request.cwd;
    auto failed = [this, done](const json &error, const char *reason) {
        noteAuth(error);
        done({}, integer(error, "code") == kAuthRequired ? "not-logged-in" : reason);
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
            setModes(result);
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
             setModes(result);
             self->setConfig(result);
             self->loaded();
             withModel(self);
             done(id, {});
         });
}

} // namespace pvai
