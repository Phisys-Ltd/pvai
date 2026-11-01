#include "codex_backend.h"

#include "transcript.h"

#include <algorithm>
#include <fstream>

#include <poll.h>

namespace pvai {

namespace {

constexpr auto kRetry = std::chrono::seconds(10);
constexpr auto kRescan = std::chrono::seconds(15);

// What the context bar shows: the last request's tokens against the model's
// window.
json contextOf(long long tokens, const json &size)
{
    json percent = nullptr;
    if (size.is_number() && size.get<long long>() > 0)
        percent = (tokens * 100 + size.get<long long>() / 2) / size.get<long long>();
    return {{"percent", percent}, {"tokens", tokens}, {"size", size}};
}

// The context of a thread the daemon did not see run: the last token count
// in its rollout file (Codex's own format, read from the end), or null.
json rolloutContext(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (path.empty() || !in)
        return nullptr;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = std::max<std::streamoff>(0, size - 4 * 1024 * 1024);
    in.seekg(from);
    std::string line, found;
    while (std::getline(in, line))
        if (line.find("\"token_count\"") != std::string::npos)
            found = line;
    const json event = json::parse(found, nullptr, false);
    if (!event.is_object())
        return nullptr;
    const json info = event.value("payload", json()).value("info", json());
    if (!info.is_object())
        return nullptr;
    return contextOf(info.value("last_token_usage", json()).value("total_tokens", 0LL), info.value("model_context_window", json()));
}

// A thread's status as a session state and dialog.
std::pair<std::string, std::string> stateOf(const json &status)
{
    const std::string type = str(status, "type");
    if (type != "active")
        return {"idle", ""};
    if (status.contains("activeFlags") && status["activeFlags"].is_array())
        for (const auto &flag : status["activeFlags"]) {
            if (flag == "waitingOnApproval")
                return {"waiting", "permission"};
            if (flag == "waitingOnUserInput")
                return {"waiting", "question"};
        }
    return {"busy", ""};
}

// The approvals a thread asks its client for: the session's dialog kind, or
// empty for requests that are not the owner's to answer.
std::string dialogOf(const std::string &method)
{
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval"
        || method == "item/permissions/requestApproval" || method == "applyPatchApproval" || method == "execCommandApproval")
        return "permission";
    if (method == "item/tool/requestUserInput" || method == "mcpServer/elicitation/request")
        return "question";
    return {};
}

// `dialogInfo` for an approval or a question: the command, the files of a
// patch, the permissions asked for, the questions.
json infoOf(const std::string &method, const json &params)
{
    const std::string reason = str(params, "reason");
    if (method == "item/commandExecution/requestApproval")
        return dialogInfo("shell", str(params, "command"), reason);
    if (method == "execCommandApproval") {
        std::string command;
        if (params.contains("command") && params["command"].is_array())
            for (const auto &part : params["command"])
                if (part.is_string())
                    command += (command.empty() ? "" : " ") + part.get<std::string>();
        return dialogInfo("shell", command, reason);
    }
    if (method == "applyPatchApproval") {
        std::string paths;
        if (params.contains("fileChanges") && params["fileChanges"].is_object())
            for (const auto &[path, change] : params["fileChanges"].items())
                paths += (paths.empty() ? "" : "\n") + path;
        return dialogInfo("edit", paths, reason);
    }
    if (method == "item/permissions/requestApproval")
        return dialogInfo("permissions", params.value("permissions", json()).dump(), reason);
    if (method == "item/tool/requestUserInput") {
        std::string questions;
        if (params.contains("questions") && params["questions"].is_array())
            for (const auto &question : params["questions"])
                if (const std::string q = str(question, "question"); !q.empty())
                    questions += (questions.empty() ? "" : "\n") + q;
        return dialogInfo("", questions, "");
    }
    return dialogInfo("", "", str(params, "message"));
}

// What answers an approval: allow is its first choice, deny refuses it and
// ends the turn. Null when deny is an error reply instead.
json decision(const std::string &method, const json &params, bool allow)
{
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval")
        return {{"decision", allow ? "accept" : "cancel"}};
    if (method == "applyPatchApproval" || method == "execCommandApproval")
        return {{"decision", allow ? "approved" : "abort"}};
    if (method == "mcpServer/elicitation/request")
        return {{"action", allow ? "accept" : "cancel"}};
    if (!allow)
        return nullptr;
    if (method == "item/permissions/requestApproval")
        return {{"permissions", params.value("permissions", json::object())}, {"scope", "turn"}};
    // A question: the first option of each.
    json answers = json::object();
    if (params.contains("questions") && params["questions"].is_array())
        for (const auto &question : params["questions"]) {
            const json options = question.value("options", json());
            json picked = json::array();
            if (options.is_array() && !options.empty())
                picked.push_back(str(options[0], "label"));
            answers[str(question, "id")] = {{"answers", picked}};
        }
    return {{"answers", answers}};
}

// The app server's version from its user agent, "name/1.2.3 (...)".
std::string agentVersion(const std::string &userAgent)
{
    const auto slash = userAgent.find('/');
    if (slash == std::string::npos)
        return {};
    const auto end = userAgent.find_first_of(" ;()", slash + 1);
    return userAgent.substr(slash + 1, end == std::string::npos ? std::string::npos : end - slash - 1);
}

} // namespace

// ---- CodexSession ----

CodexSession::CodexSession(Host &host, CodexBackend &backend, const json &thread, bool detached)
    : Session(host, str(thread, "id"), str(thread, "cwd"), {})
    , backend_(backend)
    , detached_(detached)
{
    std::tie(state_, dialog_) = stateOf(thread.value("status", json()));
    usage_ = {{"model", thread.value("model", json())},
              {"context", nullptr},
              {"fiveHour", backend.limits()["fiveHour"]},
              {"sevenDay", backend.limits()["sevenDay"]},
              {"costUsd", nullptr},
              {"version", backend.serverVersion().empty() ? json() : json(backend.serverVersion())}};
    if (const json context = rolloutContext(str(thread, "path")); !context.is_null())
        usage_["context"] = context;
    else
        usage_["context"] = contextOf(0, nullptr);
}

bool CodexSession::canSubmit() const
{
    return backend_.ready();
}

void CodexSession::submit(const Prompt &prompt)
{
    const std::string text = (prompt.voice ? kMicMark : "") + prompt.text;
    state_ = "busy";
    turnPending_ = true;
    voiceTurn_ = prompt.voice;
    const std::string id = id_;
    Host &host = host_;
    // Codex's slash commands live in its TUI; /compact has a call of its own
    // and runs as a turn like a prompt.
    const bool compact = prompt.text == "/compact";
    json params = compact ? json{{"threadId", id}}
                          : json{{"threadId", id}, {"input", {{{"type", "text"}, {"text", text}, {"text_elements", json::array()}}}}};
    // A mode or model set since the last turn: it holds for this one and
    // the ones after.
    if (!compact && !approval_.empty()) {
        params["approvalPolicy"] = approval_;
        approval_.clear();
    }
    if (!compact && !sandbox_.is_null()) {
        params["sandboxPolicy"] = sandbox_;
        sandbox_ = nullptr;
    }
    if (!compact && !model_.empty()) {
        params["model"] = model_;
        model_.clear();
    }
    backend_.call(compact ? "thread/compact/start" : "turn/start", params,
                  [&host, id](const json &, const json &error) {
                      auto *self = dynamic_cast<CodexSession *>(host.session(id));
                      if (!self || error.is_null())
                          return;
                      self->turnPending_ = false;
                      self->state_ = "idle";
                      self->deliver();
                      self->changed();
                  });
}

std::string CodexSession::answer(const std::string &choice)
{
    if (request_.is_null())
        return "no-dialog";
    if (choice != "allow" && choice != "deny")
        return "bad-choice";
    reply(choice == "allow");
    changed();
    return {};
}

void CodexSession::reply(bool allow)
{
    const json request = request_;
    request_ = nullptr;
    const json result = decision(str(request, "method"), request.value("params", json()), allow);
    if (result.is_null()) {
        backend_.respondError(request["id"], -32000, "declined by the owner");
        if (!turnId_.empty())
            backend_.call("turn/interrupt", {{"threadId", id_}, {"turnId", turnId_}});
    } else {
        backend_.respond(request["id"], result);
    }
    if (state_ == "waiting") {
        state_ = "busy";
        dialog_.clear();
    }
}

namespace {

// Codex's three permission presets (as its /permissions picker): approval
// policy and sandbox together.
struct CodexPreset {
    const char *id, *name, *approval, *sandbox;
};
constexpr CodexPreset kCodexPresets[] = {
    {"read-only", "Read only", "on-request", "readOnly"},
    {"auto", "Auto", "on-request", "workspaceWrite"},
    {"full-access", "Full access", "never", "dangerFullAccess"},
};

} // namespace

void CodexSession::setSettings(const json &approval, const json &sandbox, const json &model)
{
    // Another combination (a granular policy, untrusted, an external
    // sandbox) is none of the presets: no mode.
    json mode;
    const std::string type = sandbox.is_object() ? str(sandbox, "type") : "";
    for (const auto &preset : kCodexPresets)
        if (approval.is_string() && approval.get<std::string>() == preset.approval && type == preset.sandbox)
            mode = preset.id;
    if (!sandbox.is_object() && approval.is_string()) // no sandbox in the answer: keep what we know
        mode = mode_;
    if (mode != mode_) {
        mode_ = mode;
        changed();
    }
    if (model.is_string())
        noteModel(model.get<std::string>());
}

// A preset goes with the next turn/start, approval policy and sandbox
// together; the app server keeps them from there on.
std::string CodexSession::setMode(const std::string &mode, const json &ref)
{
    for (const auto &preset : kCodexPresets) {
        if (mode != preset.id)
            continue;
        approval_ = preset.approval;
        sandbox_ = {{"type", preset.sandbox}};
        mode_ = mode;
        changed();
        modeEvent(ref, "done");
        return {};
    }
    return "unknown-mode";
}

// The model goes with the next turn/start and holds from there.
std::string CodexSession::setModel(const std::string &model, const json &ref)
{
    model_ = model;
    noteModel(model);
    modelEvent(ref, model, "done");
    return {};
}

bool CodexSession::stoppable() const
{
    return detached_ || !backend_.heldByTui(id_);
}

// A thread the daemon started is archived: unsubscribing would leave it
// loaded in the app server for good; archived, it is unloaded and kept in
// Codex's history, where `start.resume` finds it (a thread without a turn
// cannot be archived: it is only left). Another client's thread that no
// `pvai codex` TUI holds is only let go of: it stays in `codex resume`.
json CodexSession::describe() const
{
    json entry = Session::describe();
    entry["subagents"] = backend_.subagentsOf(id_);
    return entry;
}

std::string CodexSession::stop()
{
    if (!detached_) {
        if (backend_.heldByTui(id_))
            return "not-detached";
        backend_.release(id_);
        end();
        return {};
    }
    CodexBackend &backend = backend_;
    const std::string id = id_;
    backend.call("thread/archive", {{"threadId", id}}, [&backend, id](const json &, const json &error) {
        if (!error.is_null())
            backend.call("thread/unsubscribe", {{"threadId", id}});
    });
    backend.forget(id);
    end();
    return {};
}

void CodexSession::interrupt()
{
    if (!request_.is_null())
        reply(false);
    if (!turnId_.empty())
        backend_.call("turn/interrupt", {{"threadId", id_}, {"turnId", turnId_}});
    changed();
}

json CodexSession::history(int limit)
{
    const std::size_t n = std::min<std::size_t>(entries_.size(), static_cast<std::size_t>(std::max(0, limit)));
    json out = json::array();
    for (auto it = entries_.end() - static_cast<long>(n); it != entries_.end(); ++it)
        out.push_back(*it);
    return out;
}

// thread/items/list, newest first: what came before the daemon saw it.
void CodexSession::setOlderItems(const json &data)
{
    std::deque<json> older;
    for (auto entry = data.rbegin(); entry != data.rend(); ++entry)
        for (auto &e : entries(entry->value("item", json()), entry->value("completedAtMs", entry->value("startedAtMs", 0LL))))
            older.push_back(std::move(e));
    // What arrived live meanwhile is newer.
    older.insert(older.end(), entries_.begin(), entries_.end());
    entries_ = std::move(older);
    while (entries_.size() > TranscriptTail::kKeep)
        entries_.pop_front();
    historyReset();
}

void CodexSession::setLimits(const json &limits)
{
    usage_["fiveHour"] = limits["fiveHour"];
    usage_["sevenDay"] = limits["sevenDay"];
    usageChanged();
}

void CodexSession::setVersion(const std::string &version)
{
    if (usage_.value("version", json()) == json(version))
        return;
    usage_["version"] = version;
    usageChanged();
}

void CodexSession::usageChanged()
{
    host_.broadcast({{"type", "usage"}, {"session", id_}, {"usage", usage_}});
}

// An item as protocol entries.
std::vector<json> CodexSession::entries(const json &item, long long ms)
{
    std::vector<json> out;
    const std::string type = str(item, "type"), id = str(item, "id");
    const json base = {{"id", id}, {"time", isoTime(ms)}};
    auto emit = [&](json entry, const std::string &suffix = {}) {
        entry.update(base);
        if (!suffix.empty())
            entry["id"] = id + suffix;
        out.push_back(std::move(entry));
    };
    auto tool = [&](const std::string &name, const std::string &text, const std::string &file = {}) {
        json entry = {{"kind", "tool"}, {"tool", name}, {"text", cut(text, 300)}};
        if (!file.empty())
            entry["file"] = file;
        emit(std::move(entry));
    };
    auto result = [&](const std::string &name, const std::string &text, bool error) {
        if (!text.empty() || error)
            emit({{"kind", "result"}, {"tool", name}, {"text", cut(text, kMaxResult)}, {"error", error}}, ":result");
    };
    if (type == "userMessage") {
        std::string text;
        if (item.contains("content") && item["content"].is_array())
            for (const auto &part : item["content"])
                if (str(part, "type") == "text")
                    text += (text.empty() ? "" : "\n") + str(part, "text");
        if (!text.empty())
            emit({{"kind", "prompt"}, {"text", cut(text, kMaxText)}});
    } else if (type == "agentMessage") {
        if (const std::string text = str(item, "text"); !text.empty())
            emit({{"kind", "text"}, {"text", cut(text, kMaxText)}});
    } else if (type == "commandExecution") {
        std::string command = str(item, "command");
        if (item.contains("commandActions") && item["commandActions"].is_array() && item["commandActions"].size() == 1)
            if (const std::string action = str(item["commandActions"][0], "command"); !action.empty())
                command = action;
        tool("shell", command, commandFile(command));
        std::string output = str(item, "aggregatedOutput");
        if (const auto it = outputs_.find(id); it != outputs_.end()) {
            if (output.empty())
                output = it->second;
            outputs_.erase(it);
        }
        const json code = item.value("exitCode", json());
        const std::string status = str(item, "status");
        result("shell", output, (code.is_number() && code.get<long long>() != 0) || status == "failed" || status == "declined");
    } else if (type == "fileChange") {
        std::string paths;
        if (item.contains("changes") && item["changes"].is_array())
            for (const auto &change : item["changes"])
                paths += (paths.empty() ? "" : ", ") + str(change, "path");
        tool("edit", paths);
        const std::string status = str(item, "status");
        if (status == "failed" || status == "declined")
            result("edit", status, true);
    } else if (type == "mcpToolCall" || type == "dynamicToolCall") {
        const std::string name = type == "mcpToolCall" ? str(item, "server") + "." + str(item, "tool") : str(item, "tool");
        tool(name, item.value("arguments", json()).dump());
        const json error = item.value("error", json());
        if (error.is_object())
            result(name, str(error, "message"), true);
        else if (item.contains("result") && !item["result"].is_null())
            result(name, item["result"].dump(), false);
    } else if (type == "webSearch") {
        tool("web_search", str(item, "query"));
    } else if (type == "contextCompaction") {
        emit({{"kind", "command"}, {"text", "/compact"}});
    }
    return out;
}

void CodexSession::pushEntries(std::vector<json> fresh)
{
    for (const auto &entry : fresh)
        entries_.push_back(entry);
    while (entries_.size() > TranscriptTail::kKeep)
        entries_.pop_front();
    sendEntries(std::move(fresh));
}

void CodexSession::onNotification(const std::string &method, const json &params)
{
    if (method == "thread/closed" || method == "thread/archived" || method == "thread/deleted") {
        end();
    } else if (method == "thread/status/changed") {
        const json status = params.value("status", json());
        if (str(status, "type") == "notLoaded") {
            end();
            return;
        }
        auto [state, dialog] = stateOf(status);
        // turn/start is on its way (the thread is not busy yet), or the turn
        // has not reported its end: no other prompt may go in.
        if (state == "idle" && (turnPending_ || !turnId_.empty()))
            return;
        // A dialog we hold the request for says better what it is.
        if (state == "waiting" && !request_.is_null())
            dialog = dialogOf(str(request_, "method"));
        if (state == state_ && dialog == dialog_)
            return;
        state_ = state;
        dialog_ = dialog;
        if (request_.is_null())
            dialogInfo_ = nullptr;
        deliver();
        changed();
    } else if (method == "model/rerouted") {
        // Codex moved the turn to another model on its own (a limit, a
        // capacity problem): the app hears it.
        noteModel(str(params, "toModel"), str(params, "reason").empty() ? "rerouted" : str(params, "reason"));
    } else if (method == "thread/settings/updated") {
        const json settings = params.value("threadSettings", json());
        setSettings(settings.value("approvalPolicy", json()), settings.value("sandboxPolicy", json()), settings.value("model", json()));
    } else if (method == "turn/started") {
        turnId_ = str(params.value("turn", json()), "id");
        turnPending_ = false;
        answer_.clear();
        if (state_ == "idle") {
            state_ = "busy";
            changed();
        }
    } else if (method == "turn/completed") {
        const json turn = params.value("turn", json());
        turnId_.clear();
        request_ = nullptr;
        outputs_.clear();
        files_.clear();
        if (str(turn, "status") == "interrupted") {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            pushEntries({{{"kind", "interrupted"}, {"id", str(turn, "id") + ":interrupted"}, {"time", isoTime(now.count())}}});
            voiceTurn_ = false;
        } else {
            std::string answer = answer_;
            if (answer.empty())
                answer = str(turn.value("error", json()), "message");
            say(answer);
        }
        answer_.clear();
        if (!turnPending_) {
            state_ = "idle";
            dialog_.clear();
            deliver();
        }
        changed();
    } else if (method == "item/completed") {
        const json item = params.value("item", json());
        const std::string type = str(item, "type");
        if (type == "agentMessage")
            answer_ = str(item, "text");
        else if (type == "userMessage")
            for (const auto &e : entries(item, 0))
                voiceTurn_ = str(e, "text").rfind(kMicMark, 0) == 0;
        pushEntries(entries(item, params.value("completedAtMs", 0LL)));
    } else if (method == "item/started") {
        // A file change's approval names only the item: its paths are here.
        const json item = params.value("item", json());
        if (str(item, "type") == "fileChange" && item.contains("changes") && item["changes"].is_array()) {
            std::string paths;
            for (const auto &change : item["changes"])
                paths += (paths.empty() ? "" : "\n") + str(change, "path");
            files_[str(item, "id")] = paths;
        }
    } else if (method == "item/commandExecution/outputDelta") {
        std::string &output = outputs_[str(params, "itemId")];
        if (output.size() < kMaxResult + 16)
            output += str(params, "delta");
    } else if (method == "thread/tokenUsage/updated") {
        const json usage = params.value("tokenUsage", json());
        const json context = contextOf(usage.value("last", json()).value("totalTokens", 0LL), usage.value("modelContextWindow", json()));
        if (context != usage_["context"]) {
            usage_["context"] = context;
            usageChanged();
        }
    } else if (method == "serverRequest/resolved") {
        if (!request_.is_null() && request_["id"] == params.value("requestId", json())) {
            request_ = nullptr;
            if (state_ == "waiting") {
                state_ = "busy";
                dialog_.clear();
            }
            changed();
        }
    }
}

// Codex asks its clients (all of a thread's subscribers; the first answer
// counts) to approve something.
void CodexSession::onRequest(const json &requestId, const std::string &method, const json &params)
{
    const std::string dialog = dialogOf(method);
    if (dialog.empty())
        return;
    request_ = {{"id", requestId}, {"method", method}, {"params", params}};
    state_ = "waiting";
    dialog_ = dialog;
    std::string text = str(params, "command");
    if (text.empty())
        text = str(params, "reason");
    if (text.empty())
        text = str(params, "message");
    if (text.empty() && params.contains("questions") && params["questions"].is_array() && !params["questions"].empty())
        text = str(params["questions"][0], "question");
    dialogInfo_ = infoOf(method, params);
    if (method == "item/fileChange/requestApproval") {
        const auto it = files_.find(str(params, "itemId"));
        dialogInfo_ = dialogInfo("edit", it == files_.end() ? "" : it->second, str(params, "reason"));
    }
    attention(dialog, text);
    changed();
}

// ---- CodexBackend ----

CodexBackend::CodexBackend(Host &host)
    : Backend(host)
{
}

json CodexBackend::modes() const
{
    json modes = json::array();
    for (const auto &preset : kCodexPresets)
        modes.push_back({{"id", preset.id}, {"name", preset.name}});
    return modes;
}

void CodexBackend::findProgram()
{
    if (bin_.empty())
        bin_ = codexBinary();
}

// `codex login status`: "Logged in using ..." and status 0 when it is.
void CodexBackend::refreshLogin(std::function<void(bool changed)> done)
{
    if (bin_.empty())
        return done(false);
    host_.runProcess({bin_, "login", "status"}, std::chrono::seconds(30), [this, done](int status, const std::string &output) {
        const json now = status == 0 && output.find("Logged in") != std::string::npos;
        const bool changed = now != loggedIn_;
        loggedIn_ = now;
        done(changed);
    });
}

short CodexBackend::events() const
{
    return static_cast<short>(POLLIN | (link_.wantsWrite() ? POLLOUT : 0));
}

void CodexBackend::call(const std::string &method, const json &params, Done done)
{
    const long long id = link_.request(method, params);
    if (done)
        pending_[id] = std::move(done);
}

std::vector<CodexSession *> CodexBackend::sessions()
{
    std::vector<CodexSession *> out;
    for (Session *session : host_.sessions())
        if (auto *codex = dynamic_cast<CodexSession *>(session))
            out.push_back(codex);
    return out;
}

// Connects to the app server when it is not; starts it (Codex's own daemon)
// when it does not run, at most once a minute.
void CodexBackend::tick(Clock::time_point now)
{
    if (bin_.empty())
        return;
    if (link_.isOpen()) {
        if (ready_ && now >= nextRescan_) {
            nextRescan_ = now + kRescan;
            rescan();
        }
        if (!link_.flush())
            lost();
        return;
    }
    if (now < nextTry_)
        return;
    nextTry_ = now + kRetry;
    if (!link_.open()) {
        if (!lastStart_ || now - *lastStart_ > std::chrono::minutes(1)) {
            lastStart_ = now;
            const char *home = std::getenv("HOME");
            spawnDetached(home ? home : "/", {bin_, "app-server", "daemon", "start"});
            nextTry_ = now + std::chrono::seconds(3);
        }
        return;
    }
    ready_ = false;
    call("initialize",
         {{"clientInfo", {{"name", "pvai"}, {"title", "PhiVoice AI"}, {"version", kVersion}}},
          {"capabilities", {{"experimentalApi", true}}}},
         [this](const json &result, const json &error) {
             if (!error.is_null()) {
                 lost();
                 return;
             }
             link_.notify("initialized", nullptr);
             ready_ = true;
             serverVersion_ = agentVersion(str(result, "userAgent"));
             for (CodexSession *session : sessions())
                 session->setVersion(serverVersion_);
             call("account/rateLimits/read", json::object(),
                  [this](const json &result, const json &) { setLimits(result.value("rateLimits", json())); });
             call("model/list", json::object(), [this](const json &result, const json &) {
                 json models = json::array();
                 if (result.contains("data") && result["data"].is_array())
                     for (const auto &model : result["data"])
                         if (!model.value("hidden", false) && !str(model, "model").empty())
                             models.push_back(str(model, "model"));
                 models_ = models;
             });
             nextRescan_ = Clock::now() + kRescan;
             rescan();
         });
}

std::optional<Clock::time_point> CodexBackend::deadline() const
{
    if (bin_.empty())
        return std::nullopt;
    return link_.isOpen() ? nextRescan_ : nextTry_;
}

void CodexBackend::lost()
{
    link_.close();
    ready_ = false;
    pending_.clear();
    adopting_.clear(); // their answers will not come
    subagents_.clear();
    for (CodexSession *session : sessions())
        session->end();
    nextTry_ = Clock::now() + std::chrono::seconds(2);
}

void CodexBackend::service()
{
    std::vector<json> messages;
    const bool alive = link_.read(messages);
    for (const auto &message : messages) {
        if (!message.is_object())
            continue;
        const std::string method = str(message, "method");
        // Codex's messages are the app server's to shape: anything not an
        // object where one belongs reads as an empty one, and a surprise in
        // one message costs that message, not the daemon.
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
                const json params = object("params");
                if (auto *session = dynamic_cast<CodexSession *>(host_.session(str(params, "threadId"))))
                    session->onRequest(message["id"], method, params);
            } else {
                const json params = object("params");
                if (method == "account/rateLimits/updated") {
                    setLimits(params.value("rateLimits", json()));
                } else if (method == "thread/started") {
                    const json thread = params.value("thread", json());
                    if (thread.value("parentThreadId", json()).is_null())
                        adopt(str(thread, "id"));
                    else
                        noteSubagent(thread);
                } else if (subagentEvent(method, params)) {
                } else if (auto *session = dynamic_cast<CodexSession *>(host_.session(str(params, "threadId")))) {
                    session->onNotification(method, params);
                }
            }
        } catch (const json::exception &e) {
            host_.log(std::string("codex: ") + method + ": " + e.what());
        }
        if (!link_.isOpen())
            return;
    }
    if (!alive || !link_.flush())
        lost();
}

// The threads loaded in the app server: those of Codex TUIs and of other
// clients. Thread events reach only a thread's subscribers, so each is
// resumed (which subscribes) once.
void CodexBackend::rescan()
{
    call("thread/loaded/list", json::object(), [this](const json &result, const json &) {
        if (!result.contains("data") || !result["data"].is_array())
            return;
        std::set<std::string> loaded;
        for (const auto &id : result["data"])
            if (id.is_string())
                loaded.insert(id.get<std::string>());
        std::erase_if(released_, [&](const std::string &id) { return !loaded.count(id); });
        if (std::erase_if(subagents_, [&](const auto &entry) { return !loaded.count(entry.first); }))
            host_.sessionsChanged();
        for (const auto &id : loaded)
            adopt(id);
    });
}

namespace {

bool activeStatus(const json &holder)
{
    const json status = holder.value("status", json());
    return status.is_object() && status.value("type", "") == "active";
}

} // namespace

// A subagent thread (one with parentThreadId): it shows in its parent's
// `subagents` while it works. Codex's own helpers (review, compact) are
// subagents too; their kind stands in for a role.
void CodexBackend::noteSubagent(const json &thread)
{
    const std::string id = str(thread, "id");
    const std::string parent = str(thread, "parentThreadId");
    if (id.empty() || parent.empty())
        return;
    std::string type = str(thread, "agentRole");
    const json source = thread.value("source", json());
    if (type.empty() && source.is_object() && source.contains("subAgent") && source["subAgent"].is_string())
        type = source["subAgent"].get<std::string>();
    std::string description = cut(oneLine(str(thread, "preview"), kTitleMax * 4), kTitleMax);
    if (description.empty())
        description = str(thread, "agentNickname");
    Subagent agent{parent,
                   {{"id", id},
                    {"type", type},
                    {"description", description},
                    {"name", str(thread, "agentNickname")},
                    {"background", true},
                    {"startedAt", thread.value("createdAt", 0LL)}},
                   activeStatus(thread)};
    auto known = subagents_.find(id);
    const bool news = known == subagents_.end() || known->second.active != agent.active || known->second.entry != agent.entry;
    subagents_[id] = std::move(agent);
    if (news)
        host_.sessionsChanged();
}

// A notification about a subagent thread: its status, or that it closed.
// True when it was one.
bool CodexBackend::subagentEvent(const std::string &method, const json &params)
{
    const auto it = subagents_.find(str(params, "threadId"));
    if (it == subagents_.end())
        return false;
    if (method == "thread/status/changed") {
        const bool active = activeStatus(params);
        if (active != it->second.active) {
            it->second.active = active;
            host_.sessionsChanged();
        }
    } else if (method == "thread/closed" || method == "thread/archived" || method == "thread/deleted") {
        subagents_.erase(it);
        host_.sessionsChanged();
    }
    return true;
}

json CodexBackend::subagentsOf(const std::string &thread) const
{
    json out = json::array();
    for (const auto &[id, agent] : subagents_)
        if (agent.parent == thread && agent.active)
            out.push_back(agent.entry);
    std::sort(out.begin(), out.end(), [](const json &a, const json &b) {
        return a.value("startedAt", 0LL) < b.value("startedAt", 0LL);
    });
    return out;
}

bool CodexBackend::heldByTui(const std::string &thread) const
{
    return std::any_of(tuis_.begin(), tuis_.end(), [&](const Tui &tui) { return tui.threads.count(thread) > 0; });
}

void CodexBackend::release(const std::string &thread)
{
    call("thread/unsubscribe", {{"threadId", thread}});
    released_.insert(thread);
}

void CodexBackend::processStarted(unsigned long long id, const json &hello)
{
    Tui tui{id, str(hello, "cwd"), {}};
    if (const std::string resume = str(hello, "resume"); !resume.empty()) {
        tui.threads.insert(resume);
        released_.erase(resume);
    }
    tuis_.push_back(std::move(tui));
    host_.sessionsChanged(); // a resumed thread is no longer stoppable
}

// The TUI is gone: its threads that are not the daemon's own (detached) are
// unsubscribed, which unloads them unless another client holds them, and
// leave the list.
void CodexBackend::processEnded(unsigned long long id)
{
    const auto it = std::find_if(tuis_.begin(), tuis_.end(), [&](const Tui &tui) { return tui.id == id; });
    if (it == tuis_.end())
        return;
    for (const auto &thread : it->threads) {
        auto *session = dynamic_cast<CodexSession *>(host_.session(thread));
        if (!session || session->detached())
            continue;
        release(thread);
        session->end();
    }
    tuis_.erase(it);
}

// A thread without a turn yet has nothing to resume; it is taken on a later
// scan, once it has one. (A TUI also leaves an empty thread behind each
// start, which can be neither resumed nor archived.)
void CodexBackend::adopt(const std::string &id)
{
    if (id.empty() || host_.session(id) || stopped_.count(id) || adopting_.count(id) || released_.count(id)
        || subagents_.count(id))
        return;
    adopting_.insert(id);
    call("thread/resume", {{"threadId", id}, {"excludeTurns", true}}, [this, id](const json &result, const json &) {
        adopting_.erase(id);
        if (!result.contains("thread"))
            return;
        if (result["thread"].value("parentThreadId", json()).is_null()) {
            if (CodexSession *session = addSession(result["thread"], false))
                session->setSettings(result.value("approvalPolicy", json()), result.value("sandbox", json()), result.value("model", json()));
        } else
            noteSubagent(result["thread"]);
    });
}

CodexSession *CodexBackend::addSession(const json &thread, bool detached)
{
    const std::string id = str(thread, "id");
    // Subagents' threads are part of their parent's turn.
    if (id.empty() || !thread.value("parentThreadId", json()).is_null())
        return nullptr;
    if (auto *existing = dynamic_cast<CodexSession *>(host_.session(id))) {
        if (detached)
            existing->setDetached();
        return existing;
    }
    auto owned = std::make_unique<CodexSession>(host_, *this, thread, detached);
    CodexSession *session = owned.get();
    // A TUI's thread: the only `pvai codex` running in its folder.
    if (!detached) {
        Tui *only = nullptr;
        int count = 0;
        for (auto &tui : tuis_)
            if (tui.cwd == session->cwd()) {
                only = &tui;
                ++count;
            }
        if (count == 1)
            only->threads.insert(id);
    }
    host_.addSession(std::move(owned));
    Host &host = host_;
    call("thread/items/list", {{"threadId", id}, {"limit", 200}, {"sortDirection", "desc"}}, [&host, id](const json &result, const json &) {
        auto *session = dynamic_cast<CodexSession *>(host.session(id));
        if (session && result.contains("data") && result["data"].is_array())
            session->setOlderItems(result["data"]);
    });
    return session;
}

void CodexBackend::setLimits(const json &limits)
{
    if (!limits.is_object())
        return;
    auto window = [&](const char *key) -> json {
        const json w = limits.value(key, json());
        if (!w.is_object())
            return nullptr;
        return {{"percent", w.value("usedPercent", json())}, {"resetsAt", w.value("resetsAt", json())}};
    };
    const json fresh = {{"fiveHour", window("primary")}, {"sevenDay", window("secondary")}};
    if (fresh == limits_)
        return;
    limits_ = fresh;
    for (CodexSession *session : sessions())
        session->setLimits(limits_);
}

void CodexBackend::listThreads(const std::string &cwd, bool archived, std::function<void(json)> done)
{
    call("thread/list",
         {{"cwd", cwd}, {"archived", archived}, {"limit", kMaxConversations}, {"sortKey", "updated_at"}, {"sortDirection", "desc"}},
         [done](const json &result, const json &) {
             done(result.contains("data") && result["data"].is_array() ? result["data"] : json::array());
         });
}

// The threads of `cwd`, archived ones too (`stop` puts them there), without
// those that are loaded.
void CodexBackend::conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done)
{
    if (!ready_)
        return done({});
    listThreads(cwd, false, [this, cwd, done](json open) {
        listThreads(cwd, true, [this, open, done](json archived) {
            std::vector<Conversation> out;
            for (const json *list : std::initializer_list<const json *>{&open, &archived})
                for (const auto &thread : *list) {
                    const std::string id = str(thread, "id");
                    if (id.empty() || host_.session(id) || !thread.value("parentThreadId", json()).is_null()
                        || thread.value("ephemeral", false))
                        continue;
                    std::string title = str(thread, "name");
                    if (title.empty())
                        title = str(thread, "preview");
                    if (title.rfind(kMicMark, 0) == 0)
                        title = title.substr(std::string(kMicMark).size());
                    title = cut(oneLine(title, kTitleMax * 4), kTitleMax);
                    if (title.empty())
                        continue;
                    out.push_back({id, title, thread.value("updatedAt", 0LL)});
                }
            std::sort(out.begin(), out.end(), [](const auto &x, const auto &y) { return x.at > y.at; });
            if (out.size() > kMaxConversations)
                out.resize(kMaxConversations);
            done(std::move(out));
        });
    });
}

// A new thread in `cwd`, or an ended one again; `args` may name the model
// (`-m`, `--model`).
void CodexBackend::start(const StartRequest &request, Started done)
{
    if (!ready_)
        return done({}, "no-assistant");
    json params = {{"developerInstructions", kVoiceContext}};
    for (std::size_t i = 0; i + 1 < request.args.size(); ++i)
        if (request.args[i] == "-m" || request.args[i] == "--model")
            params["model"] = request.args[i + 1];
    const std::string label = request.label;
    auto started = [this, label, done](const json &result, const json &error, const char *failure) {
        CodexSession *session = error.is_null() && result.contains("thread") ? addSession(result["thread"], true) : nullptr;
        if (!session)
            return done({}, failure);
        session->setLabel(label);
        session->setSettings(result.value("approvalPolicy", json()), result.value("sandbox", json()), result.value("model", json()));
        done(session->id(), {});
    };
    if (request.resume.empty()) {
        params["cwd"] = request.cwd;
        call("thread/start", params, [started](const json &result, const json &error) { started(result, error, "bad-start"); });
        return;
    }
    const std::string id = request.resume;
    if (auto *running = dynamic_cast<CodexSession *>(host_.session(id))) {
        running->setDetached();
        running->setLabel(label);
        return done(id, {});
    }
    // Archived by `stop`, or not: unarchive answers an error for the
    // latter, which does not matter.
    stopped_.erase(id);
    adopting_.insert(id);
    params["threadId"] = id;
    params["excludeTurns"] = true;
    call("thread/unarchive", {{"threadId", id}}, [this, id, params, started](const json &, const json &) {
        call("thread/resume", params, [this, id, started](const json &result, const json &error) {
            adopting_.erase(id);
            started(result, error, "unknown-conversation");
        });
    });
}

} // namespace pvai
