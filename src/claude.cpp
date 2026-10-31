#include "claude.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace pvai {

namespace {

// What Claude Code writes into the transcript when the owner interrupts a
// turn with Esc; no hook fires for that.
const char *kInterruptMark = "[Request interrupted by user";
constexpr auto kSubmitTimeout = std::chrono::seconds(15);
constexpr auto kTypedTimeout = std::chrono::seconds(5); // the wrapper checks for up to 2 s
constexpr auto kRetryAfter = std::chrono::milliseconds(600);
constexpr auto kTailEvery = std::chrono::milliseconds(300);
constexpr auto kSubagentsEvery = std::chrono::seconds(1);
// An unfinished subagent from before this session object (a daemon restart,
// an earlier run of a resumed conversation) counts only while its
// transcript still moves: one Claude Code killed never gets `finished`.
constexpr auto kSubagentQuiet = std::chrono::minutes(10);
// Where a conversation's title is looked for.
constexpr std::size_t kTitleScan = 2 * 1024 * 1024;

// What a tool call would do, for the owner deciding on it: the command,
// the file, the URL... else its whole input.
std::string toolText(const json &input)
{
    if (!input.is_object())
        return {};
    for (const char *key : {"command", "file_path", "notebook_path", "url", "pattern", "path", "query", "skill", "prompt"})
        if (input.contains(key) && input[key].is_string())
            return input[key].get<std::string>();
    return input.dump();
}

// AskUserQuestion's questions, one per line.
std::string questionOf(const json &input)
{
    std::string text;
    if (input.is_object() && input.contains("questions") && input["questions"].is_array())
        for (const auto &question : input["questions"])
            if (const std::string q = str(question, "question"); !q.empty())
                text += (text.empty() ? "" : "\n") + q;
    return text;
}

off_t fileSize(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? st.st_size : -1;
}

bool appearsAfter(const std::string &path, off_t offset, const char *needle)
{
    std::ifstream in(path, std::ios::binary);
    if (!in || !in.seekg(offset))
        return false;
    const std::string tail(std::istreambuf_iterator<char>(in), {});
    return tail.find(needle) != std::string::npos;
}

// The first prompt the owner wrote in a transcript: no meta lines, no
// command tags or outputs. Empty when there is none.
std::string firstPrompt(const std::string &path)
{
    std::ifstream in(path);
    std::string line;
    std::size_t read = 0;
    while (read < kTitleScan && std::getline(in, line)) {
        read += line.size() + 1;
        if (line.find("\"type\":\"user\"") == std::string::npos)
            continue;
        const json entry = json::parse(line, nullptr, false);
        if (!entry.is_object() || entry.value("isMeta", false) || entry.value("isSidechain", false))
            continue;
        const json content = entry.value("message", json()).value("content", json());
        std::string text;
        if (content.is_string())
            text = content.get<std::string>();
        else if (content.is_array())
            for (const auto &part : content)
                if (str(part, "type") == "text")
                    text += str(part, "text");
        text = trim(text);
        if (text.empty() || text[0] == '<')
            continue;
        if (text.rfind(kMicMark, 0) == 0)
            text = text.substr(std::string(kMicMark).size());
        return cut(oneLine(text, kTitleMax * 4), kTitleMax);
    }
    return {};
}

bool goodConversationId(const std::string &id)
{
    return !id.empty() && id.size() <= 64
        && std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
}

} // namespace

ClaudeSession::ClaudeSession(Host &host, Conn &wrapper, const json &registration)
    : Session(host, registration.value("id", ""), registration.value("cwd", ""), registration.value("label", ""))
    , wrapper_(wrapper)
    , canRestart_(registration.value("restart", false))
    , verify_(registration.value("verify", false))
{
    // A wrapper that registers again after a daemon restart has a running
    // Claude Code: idle, unless its transcript shows a turn going on.
    if (registration.value("running", false)) {
        state_ = "idle";
        std::ifstream in(runtimeDir() + "/" + id_ + ".claude");
        const json known = json::parse(in, nullptr, false);
        if (known.is_object()) {
            claudeSession_ = known.value("claudeSession", "");
            transcript_ = known.value("transcript", "");
            if (!transcript_.empty()) {
                tail_.follow(transcript_);
                tail_.poll();
                wakeMark_ = tail_.wakeTurns();
                // A daemon restarted during a turn: the turn goes on.
                if (turnRunning(transcript_))
                    state_ = "busy";
            }
        }
    }
}

json ClaudeSession::describe() const
{
    json entry = Session::describe();
    entry["claudeSession"] = claudeSession_.empty() ? json() : json(claudeSession_);
    json running = json::array();
    for (const auto &[id, agent] : subagents_)
        if (!agent.entry.is_null())
            running.push_back(agent.entry);
    std::sort(running.begin(), running.end(), [](const json &a, const json &b) {
        return a.value("startedAt", 0LL) < b.value("startedAt", 0LL);
    });
    entry["subagents"] = std::move(running);
    if (inFlight_) // typed, not confirmed yet
        entry["queued"] = entry.value("queued", 0) + 1;
    return entry;
}

void ClaudeSession::submit(const Prompt &prompt)
{
    const bool slash = prompt.text[0] == '/';
    const bool verify = verify_ && !sendUnchecked_;
    sendUnchecked_ = false;
    wrapper_.send({{"type", "type"}, {"text", (prompt.voice && !slash ? kMicMark : "") + prompt.text}, {"verify", verify}});
    if (verify_)
        inFlight_ = prompt;
    if (verify) {
        // The wrapper says whether it went in; until then nothing else may.
        typedDeadline_ = Clock::now() + kTypedTimeout;
        state_ = "busy";
        return;
    }
    if (verify_ && slash) { // sent without the check: taken as delivered
        tail_.poll();
        slashPending_ = true;
        slashMark_ = tail_.localOutputs();
        state_ = "busy";
        delivered();
        return;
    }
    if (slash) {
        tail_.poll();
        slashPending_ = true;
        slashMark_ = tail_.localOutputs();
    } else {
        submitDeadline_ = Clock::now() + kSubmitTimeout;
    }
    // UserPromptSubmit confirms it; until then nothing else may go in.
    state_ = "busy";
}

// The app answers the dialog the session waits on: allow is the first,
// highlighted choice of a permission or question (Return), or "Yes, I
// trust this folder" (the second, below "No, exit"); deny is Esc.
std::string ClaudeSession::answer(const std::string &choice)
{
    if (state_ != "waiting" || dialog_.empty())
        return "no-dialog";
    // Single keys move through a dialog; it stays open until the screen
    // or a hook says otherwise.
    if (choice.rfind("key:", 0) == 0 && dialog_ != "trust") {
        static const std::map<std::string, std::string> keys = {
            {"up", "\x1b[A"}, {"down", "\x1b[B"}, {"right", "\x1b[C"}, {"left", "\x1b[D"}, {"enter", "\r"},
            {"esc", "\x1b"},  {"tab", "\t"},      {"space", " "},
        };
        const std::string key = choice.substr(4);
        const auto it = keys.find(key);
        if (it == keys.end() && !(key.size() == 1 && ((key[0] >= '0' && key[0] <= '9') || (key[0] >= 'a' && key[0] <= 'z'))))
            return "bad-choice";
        wrapper_.send({{"type", "keys"}, {"data", it != keys.end() ? it->second : key}});
        if (blocked_) {
            retryOnPrompt_ = true;
            retryAt_ = Clock::now() + kRetryAfter;
        }
        return {};
    }
    // The owner sees the screen and says the prompt is free: the next
    // prompt goes in without the check.
    if (choice == "send" && blocked_) {
        if (queue_.empty())
            return "no-dialog";
        sendUnchecked_ = true;
        unblock();
        return {};
    }
    if (choice != "allow" && choice != "deny")
        return "bad-choice";
    if (dialog_ == "screen") {
        wrapper_.send({{"type", "keys"}, {"data", choice == "deny" ? "\x1b" : "\r"}});
        if (blocked_) {
            retryOnPrompt_ = true;
            retryAt_ = Clock::now() + kRetryAfter;
        }
        return {};
    }
    const bool trustDialog = dialog_ == "trust";
    wrapper_.send({{"type", "keys"}, {"data", choice == "deny" ? "\x1b" : trustDialog ? "\x1b[B\r" : "\r"}});
    dialog_.clear();
    if (trustDialog) {
        state_ = "starting";
    } else {
        state_ = "busy";
        if (choice == "deny")
            armEscapeCheck();
    }
    changed();
    return {};
}

// The app's stop button: Esc, as the owner would press it in the terminal.
// Into a dialog it is a deny. An idle prompt has no turn to stop; an Esc
// there would only touch the owner's draft.
void ClaudeSession::interrupt()
{
    if (state_ != "busy" && !(state_ == "waiting" && dialog_ != "trust"))
        return;
    wrapper_.send({{"type", "keys"}, {"data", "\x1b"}});
    if (dialog_ == "screen") { // Esc closes it; the screen says when
        if (blocked_) {
            retryOnPrompt_ = true;
            retryAt_ = Clock::now() + kRetryAfter;
        }
        return;
    }
    if (state_ == "waiting") {
        state_ = "busy";
        dialog_.clear();
    }
    armEscapeCheck(true);
    changed();
}

// Only at a free prompt: a running turn or an open dialog would be cut
// off, a draft lost. The wrapper ends Claude Code and starts it again with
// `--resume`; SessionStart (source resume) makes the session idle again,
// and what was queued goes in then.
std::string ClaudeSession::restart()
{
    if (!restartable())
        return "not-restartable";
    if (state_ != "idle" || draft_)
        return "busy";
    wrapper_.send({{"type", "restart"}, {"resume", claudeSession_}});
    began_ = Clock::now();
    state_ = "starting";
    // The old process takes its subagents with it.
    created_ = std::filesystem::file_time_type::clock::now();
    for (auto &[id, agent] : subagents_)
        agent.entry = nullptr;
    slashPending_ = false;
    escapeChecks_.clear();
    submitDeadline_.reset();
    changed();
    return {};
}

void ClaudeSession::terminate()
{
    wrapper_.send({{"type", "terminate"}});
}

std::string DetachedClaudeSession::stop()
{
    stopping_ = true;
    terminate();
    return {};
}

json ClaudeSession::history(int limit)
{
    tail_.poll();
    const auto &recent = tail_.recent();
    const std::size_t n = std::min<std::size_t>(recent.size(), static_cast<std::size_t>(std::max(0, limit)));
    json entries = json::array();
    for (auto it = recent.end() - static_cast<long>(n); it != recent.end(); ++it)
        entries.push_back(*it);
    return entries;
}

void ClaudeSession::onHook(const json &event)
{
    const std::string name = event.value("hook_event_name", "");
    // The same hook installed twice (user and project settings) reports
    // every event twice.
    const std::string fingerprint = event.dump();
    if (fingerprint == lastEvent_)
        return;
    lastEvent_ = fingerprint;
    if (!ours(event.value("session_id", ""), name == "SessionStart" ? event.value("source", "") : "")) {
        host_.log(id_ + ": " + name + " of another Claude Code (" + event.value("session_id", "") + "), dropped");
        return;
    }
    bool known = false;
    if (const std::string id = event.value("session_id", ""); !id.empty() && id != claudeSession_) {
        claudeSession_ = id;
        known = true;
    }
    if (const std::string path = event.value("transcript_path", ""); !path.empty() && path != transcript_) {
        known = true;
        transcript_ = path;
        tail_.follow(path);
        tail_.poll();
        wakeMark_ = tail_.wakeTurns();
        subagents_.clear();
        nextSubagents_ = Clock::now();
        historyReset();
    }
    if (known)
        remember();

    if (name != "Notification" && name != "PermissionRequest")
        dialog_.clear();
    if (name == "UserPromptSubmit" || name == "Stop" || name == "SessionStart") {
        blocked_ = false; // the prompt is Claude's again
        retryAt_.reset();
        typedDeadline_.reset();
        if (name == "UserPromptSubmit")
            delivered();
        else if (inFlight_) { // a turn ended or started over without it
            queue_.push_front(*inFlight_);
            inFlight_.reset();
        }
        submitDeadline_.reset();
        catchUp();
    }
    if (name == "SessionStart") {
        const std::string source = event.value("source", "");
        if (state_ == "starting" || source == "clear" || source == "resume")
            state_ = "idle";
        if (source == "clear") // a new transcript: its output count starts at 0
            slashPending_ = false;
    } else if (name == "UserPromptSubmit") {
        state_ = "busy";
        voiceTurn_ = event.value("prompt", "").rfind(kMicMark, 0) == 0;
    } else if (name == "Stop") {
        state_ = "idle";
        forceIdle_ = false;
        slashPending_ = false;
        say(event.value("last_assistant_message", ""));
    } else if (name == "PermissionRequest") {
        // Claude Code shows a permission dialog for this tool call; its
        // Notification may come before or after.
        const std::string tool = event.value("tool_name", "");
        const json input = event.value("tool_input", json());
        const bool asks = tool == "AskUserQuestion";
        std::string text = asks ? questionOf(input) : toolText(input);
        const std::string description = tool == "Bash" && input.is_object() ? input.value("description", "") : "";
        const json message = dialogInfo_.is_object() && !dialog_.empty() ? dialogInfo_.value("message", json()) : json();
        dialogInfo_ = dialogInfo(asks ? "" : tool, text, message.is_string() ? message.get<std::string>() : "", description);
        if (state_ == "waiting" && !dialog_.empty() && dialog_ != "screen") {
            changed();
            return;
        }
        state_ = "waiting";
        dialog_ = asks ? "question" : "permission";
        attention(dialog_, asks ? text : tool + ": " + text);
    } else if (name == "Notification") {
        const std::string kind = event.value("notification_type", "");
        const std::string text = event.value("message", "");
        const bool idleNudge = kind == "idle_prompt" || (kind.empty() && text.find("waiting for your input") != std::string::npos);
        if (idleNudge || state_ == "idle")
            return;
        // PermissionRequest came first: only its words are new.
        if (state_ == "waiting" && !dialog_.empty() && dialog_ != "screen" && dialogInfo_.is_object()) {
            dialogInfo_["message"] = text.empty() ? json() : json(cut(text, kDialogTextMax));
            changed();
            return;
        }
        state_ = "waiting";
        dialog_ = kind == "permission_prompt" || text.find("permission") != std::string::npos ? "permission" : "question";
        // Without PermissionRequest (an older Claude Code): the last tool
        // call in the transcript is the one that waits.
        dialogInfo_ = dialogInfo("", "", text);
        if (dialog_ == "permission") {
            tail_.poll();
            const auto &recent = tail_.recent();
            for (auto it = recent.rbegin(); it != recent.rend(); ++it)
                if (str(*it, "kind") == "tool") {
                    dialogInfo_ = dialogInfo(str(*it, "tool"), str(*it, "text"), text);
                    break;
                }
        }
        attention(dialog_, text);
    } else {
        return;
    }
    deliver();
    changed();
}

// A Claude Code started from within this one (a shell command of Claude's)
// inherits pvai's environment, and its hooks and status line report to
// this session. Another session id is ours only before the first one is
// known, while starting (a restart), or when SessionStart says that this
// Claude Code moved to another conversation (/clear, /resume, a compaction).
bool ClaudeSession::ours(const std::string &claudeSession, const std::string &source) const
{
    if (claudeSession.empty() || claudeSession_.empty() || claudeSession == claudeSession_ || state_ == "starting")
        return true;
    return source == "clear" || source == "resume" || source == "compact";
}

// Claude Code's status line input: what /context and /usage would show, in
// short. Passed on when it changes.
void ClaudeSession::onStatus(const json &status)
{
    if (status.is_object() && !ours(status.value("session_id", ""), ""))
        return;
    auto pick = [](const json &object, const char *key) {
        return object.is_object() && object.contains(key) ? object[key] : json();
    };
    const json context = pick(status, "context_window");
    const json current = pick(context, "current_usage");
    long long tokens = 0;
    for (const char *key : {"input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"})
        if (pick(current, key).is_number())
            tokens += pick(current, key).get<long long>();
    auto limit = [&](const char *key) -> json {
        const json window = pick(pick(status, "rate_limits"), key);
        if (!window.is_object())
            return nullptr;
        return {{"percent", pick(window, "used_percentage")}, {"resetsAt", pick(window, "resets_at")}};
    };
    const json usage = {
        {"model", pick(pick(status, "model"), "display_name")},
        {"context", {{"percent", pick(context, "used_percentage")}, {"tokens", tokens}, {"size", pick(context, "context_window_size")}}},
        {"fiveHour", limit("five_hour")},
        {"sevenDay", limit("seven_day")},
        {"costUsd", pick(pick(status, "cost"), "total_cost_usd")},
        {"version", pick(status, "version")},
        {"cache", tail_.cache()},
    };
    // Cost moves with every turn; a change of it alone is not news.
    json withoutCost = usage, oldWithoutCost = usage_;
    withoutCost.erase("costUsd");
    if (oldWithoutCost.is_object())
        oldWithoutCost.erase("costUsd");
    const bool news = withoutCost != oldWithoutCost;
    usage_ = usage;
    if (news)
        host_.broadcast({{"type", "usage"}, {"session", id_}, {"usage", usage_}});
}

// The owner typed into the session. Into a dialog Claude waited on: they
// answer it, the turn goes on. An Esc may end the turn without a hook; the
// transcript tells, a moment later.
void ClaudeSession::onInput(bool escape)
{
    if (escape && state_ != "idle")
        armEscapeCheck();
    if (state_ == "waiting" && dialog_ != "trust" && dialog_ != "screen") {
        state_ = "busy";
        dialog_.clear();
        changed();
    }
}

void ClaudeSession::onDraft(bool dirty)
{
    if (draft_ == dirty)
        return;
    draft_ = dirty;
    deliver();
    changed();
}

// Claude Code asks whether to trust the folder before anything else; the
// wrapper saw it on the screen.
void ClaudeSession::onDialog(const std::string &kind)
{
    if (kind != "trust" || state_ != "starting")
        return;
    state_ = "waiting";
    dialog_ = "trust";
    dialogInfo_ = dialogInfo("", cwd_, "");
    attention("trust", cwd_);
    changed();
}

// A dialog no hook reports, read off the screen by the wrapper (empty: it
// is gone). At a free prompt it counts at once; in a turn only after a
// moment, since a permission dialog's hook may still be on its way.
void ClaudeSession::onScreen(const std::string &text, bool prompt)
{
    screenText_ = text;
    screenAt_.reset();
    promptFree_ = prompt;
    if (blocked_) {
        if (!text.empty() && dialogInfo_.value("text", json()) != json(text)) {
            dialogInfo_ = dialogInfo("", text, "");
            changed();
        }
        if (prompt && retryOnPrompt_)
            unblock();
        return;
    }
    if (dialog_ == "screen") {
        if (text.empty()) {
            state_ = screenFrom_;
            dialog_.clear();
            // A slash command's picker closed onto a free prompt: the command
            // is done, even when no output reaches the transcript (one that
            // only has local commands is not written yet).
            if (state_ == "busy" && slashPending_ && prompt) {
                slashPending_ = false;
                state_ = "idle";
            }
            deliver();
        } else {
            dialogInfo_ = dialogInfo("", text, "");
        }
        changed();
    } else if (!text.empty() && dialog_.empty() && (state_ == "idle" || state_ == "busy")) {
        if (state_ == "idle")
            openScreenDialog();
        else
            screenAt_ = Clock::now() + std::chrono::seconds(1);
    }
}

// Claude Code ended. A session the app started that ends within 90 s, or
// with an error, and not because it was stopped: the app hears why, as far
// as the screen tells (the last lines it showed).
void ClaudeSession::onExited(int status, const std::string &screen)
{
    host_.log(id_ + ": claude exited with " + std::to_string(status));
    if (!detached() || stopping_)
        return;
    if (status == 0 && Clock::now() - began_ > std::chrono::seconds(90))
        return;
    std::string tail = screen;
    for (int lines = 0, at = static_cast<int>(tail.size()) - 1; at >= 0; --at)
        if (tail[at] == '\n' && ++lines == 10) {
            tail = tail.substr(at + 1);
            break;
        }
    attention("ended", tail.empty() ? "Claude Code ended (exit status " + std::to_string(status) + ")." : tail);
}

// The wrapper's answer to a prompt typed with `verify`.
void ClaudeSession::onTyped(bool ok, const std::string &screen)
{
    if (!inFlight_ || !typedDeadline_)
        return;
    typedDeadline_.reset();
    if (!ok) {
        notDelivered(screen, true);
        return;
    }
    if (inFlight_->text[0] == '/') {
        // No hook for a slash command: in is in. It ends with its output.
        tail_.poll();
        slashPending_ = true;
        slashMark_ = tail_.localOutputs();
        delivered();
    } else {
        submitDeadline_ = Clock::now() + kSubmitTimeout;
    }
}

// The screen after a prompt that went in without becoming a turn.
void ClaudeSession::onSnapshot(const std::string &screen)
{
    if (!snapshotPending_)
        return;
    snapshotPending_ = false;
    if (inFlight_)
        notDelivered(screen, false);
}

void ClaudeSession::delivered()
{
    if (!inFlight_)
        return;
    host_.broadcast({{"type", "delivered"}, {"session", id_}, {"ref", inFlight_->ref}});
    inFlight_.reset();
}

// The prompt did not go in: in front of the queue again, and the session
// waits on the screen until the prompt box is free.
void ClaudeSession::notDelivered(const std::string &screen, bool retry)
{
    if (inFlight_)
        queue_.push_front(*inFlight_);
    inFlight_.reset();
    typedDeadline_.reset();
    submitDeadline_.reset();
    voiceTurn_ = false;
    const bool asked = dialog_ == "screen";
    blocked_ = true;
    retryOnPrompt_ = retry;
    retryAt_.reset();
    screenFrom_ = "idle";
    state_ = "waiting";
    dialog_ = "screen";
    dialogInfo_ = dialogInfo("", screen, "");
    if (!asked)
        attention("screen", screen.substr(0, screen.find('\n')));
    changed();
}

void ClaudeSession::unblock()
{
    blocked_ = false;
    retryAt_.reset();
    state_ = "idle";
    dialog_.clear();
    deliver();
    changed();
}

void ClaudeSession::openScreenDialog()
{
    screenFrom_ = state_;
    state_ = "waiting";
    dialog_ = "screen";
    dialogInfo_ = dialogInfo("", screenText_, "");
    attention("screen", screenText_.substr(0, screenText_.find('\n')));
    changed();
}

void ClaudeSession::armEscapeCheck(bool forceIdle)
{
    forceIdle_ = forceIdle;
    if (transcript_.empty() && !forceIdle)
        return;
    escapeOffset_ = transcript_.empty() ? 0 : fileSize(transcript_);
    const auto now = Clock::now();
    escapeChecks_ = {now + std::chrono::milliseconds(500), now + std::chrono::milliseconds(1500), now + std::chrono::seconds(4)};
}

void ClaudeSession::tick(Clock::time_point now)
{
    runEscapeChecks(now);
    if (typedDeadline_ && now >= *typedDeadline_) // the wrapper never answered
        notDelivered("", true);
    if (retryAt_ && now >= *retryAt_) {
        retryAt_.reset();
        if (blocked_ && promptFree_)
            unblock();
    }
    if (screenAt_ && now >= *screenAt_) {
        screenAt_.reset();
        if (!screenText_.empty() && dialog_.empty() && state_ == "busy")
            openScreenDialog();
    }
    if (now >= nextTail_) {
        pollTranscript();
        nextTail_ = now + kTailEvery;
    }
    if (now >= nextSubagents_) {
        scanSubagents(now);
        nextSubagents_ = now + kSubagentsEvery;
    }
}

std::optional<Clock::time_point> ClaudeSession::deadline() const
{
    Clock::time_point next = std::min(nextTail_, nextSubagents_);
    for (const auto &at : escapeChecks_)
        next = std::min(next, at);
    if (submitDeadline_)
        next = std::min(next, *submitDeadline_);
    if (screenAt_)
        next = std::min(next, *screenAt_);
    if (typedDeadline_)
        next = std::min(next, *typedDeadline_);
    if (retryAt_)
        next = std::min(next, *retryAt_);
    return next;
}

void ClaudeSession::runEscapeChecks(Clock::time_point now)
{
    if (submitDeadline_ && *submitDeadline_ <= now) {
        // The prompt never became a turn: free the session. Under a wrapper
        // that verifies, it goes back in the queue and the owner sees the
        // screen (a Claude Code without a login, a dialog not recognised).
        submitDeadline_.reset();
        if (inFlight_ && state_ == "busy" && !slashPending_) {
            snapshotPending_ = true;
            wrapper_.send({{"type", "snapshot"}});
        } else if (state_ == "busy" && !slashPending_) {
            state_ = "idle";
            voiceTurn_ = false;
            deliver();
            changed();
        }
    }
    const auto due = std::erase_if(escapeChecks_, [&](auto at) { return at <= now; });
    if (!due || state_ == "idle" || escapeOffset_ < 0)
        return;
    const bool seen = !transcript_.empty() && appearsAfter(transcript_, escapeOffset_, kInterruptMark);
    if (!seen && !(forceIdle_ && escapeChecks_.empty()))
        return;
    escapeChecks_.clear();
    state_ = "idle";
    voiceTurn_ = false;
    submitDeadline_.reset();
    // Interrupted by the app and nothing in the transcript says so: the
    // session hung (a Claude Code without a login answers every prompt with
    // "Not logged in" and no hook); what waited for it goes, so it does not
    // land in the same hole.
    if (!seen && !slashPending_)
        queue_.clear();
    forceIdle_ = false;
    slashPending_ = false;
    deliver();
    changed();
}

// What the transcript holds up to now belongs to the turn a hook reports
// on: an answer to a task notification in it started no turn of its own.
void ClaudeSession::catchUp()
{
    if (transcript_.empty())
        return;
    sendEntries(tail_.poll());
    tail_.settleWakes();
    wakeMark_ = tail_.wakeTurns();
}

// Claude Code's session id and transcript, kept beside the wrapper's socket
// so that a daemon started again knows them before the next hook.
void ClaudeSession::remember() const
{
    std::ofstream(runtimeDir() + "/" + id_ + ".claude")
        << json({{"claudeSession", claudeSession_}, {"transcript", transcript_}}).dump() << "\n";
}

void ClaudeSession::pollTranscript()
{
    auto fresh = tail_.poll();
    if (usage_.is_object() && usage_.value("cache", json()) != tail_.cache()) {
        usage_["cache"] = tail_.cache();
        host_.broadcast({{"type", "usage"}, {"session", id_}, {"usage", usage_}});
    }
    // A background task or agent finished and Claude Code took it up: a
    // turn without UserPromptSubmit, ended by Stop as usual.
    if (tail_.wakeTurns() != wakeMark_) {
        wakeMark_ = tail_.wakeTurns();
        if (state_ == "idle") {
            state_ = "busy";
            voiceTurn_ = false;
            changed();
        }
    }
    if (slashPending_ && tail_.localOutputs() > slashMark_) {
        slashPending_ = false;
        if (state_ == "busy") {
            state_ = "idle";
            deliver();
            changed();
        } else if (dialog_ == "screen") { // its picker is still open: idle once it closes
            screenFrom_ = "idle";
        }
    }
    sendEntries(std::move(fresh));
}

void ClaudeSession::scanSubagents(Clock::time_point)
{
    namespace fs = std::filesystem;
    const std::string suffix = ".jsonl";
    if (transcript_.size() <= suffix.size())
        return;
    const fs::path dir = transcript_.substr(0, transcript_.size() - suffix.size()) + "/subagents";
    const auto fileNow = fs::file_time_type::clock::now();
    bool news = false;
    std::error_code ec;
    for (const auto &file : fs::directory_iterator(dir, ec)) {
        const std::string name = file.path().filename().string();
        const std::string prefix = "agent-", metaSuffix = ".meta.json";
        if (name.size() <= prefix.size() + metaSuffix.size() || name.rfind(prefix, 0) != 0
            || name.compare(name.size() - metaSuffix.size(), metaSuffix.size(), metaSuffix) != 0)
            continue;
        const std::string id = name.substr(prefix.size(), name.size() - prefix.size() - metaSuffix.size());
        std::error_code timeError;
        const auto mtime = fs::last_write_time(file.path(), timeError);
        if (timeError)
            continue;
        auto known = subagents_.find(id);
        json entry;
        if (known != subagents_.end() && known->second.mtime == mtime) {
            entry = known->second.entry;
        } else {
            std::ifstream in(file.path());
            const json meta = json::parse(in, nullptr, false);
            if (meta.is_object() && !meta.value("finished", false)) {
                const auto started = std::chrono::system_clock::now()
                    - std::chrono::duration_cast<std::chrono::system_clock::duration>(fileNow - mtime);
                entry = {{"id", id},
                         {"type", meta.value("agentType", "")},
                         {"description", meta.value("description", "")},
                         {"background", meta.value("requestShape", "") == "background"},
                         {"startedAt", std::chrono::duration_cast<std::chrono::seconds>(started.time_since_epoch()).count()}};
            }
        }
        if (!entry.is_null() && mtime < created_) {
            std::error_code logError;
            const auto moved = fs::last_write_time(dir / (prefix + id + suffix), logError);
            if (logError || fileNow - moved > kSubagentQuiet)
                entry = nullptr;
        }
        if (known == subagents_.end() ? !entry.is_null() : known->second.entry != entry)
            news = true;
        subagents_[id] = {mtime, entry};
    }
    if (news)
        changed();
}

std::string ClaudeBackend::projectDir(const std::string &cwd)
{
    std::string name = cwd;
    for (char &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '-';
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/") + "/.claude/projects/" + name;
}

// The transcripts of `cwd`, newest first, without the sessions that run.
void ClaudeBackend::conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done)
{
    std::set<std::string> running;
    for (Session *session : host_.sessions())
        if (const auto *claude = dynamic_cast<ClaudeSession *>(session); claude && !claude->claudeSession().empty())
            running.insert(claude->claudeSession());
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    std::error_code ec;
    for (const auto &file : std::filesystem::directory_iterator(projectDir(cwd), ec))
        if (file.path().extension() == ".jsonl" && !running.count(file.path().stem().string()))
            files.emplace_back(file.last_write_time(ec), file.path());
    std::sort(files.begin(), files.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    std::vector<Conversation> out;
    for (const auto &[time, path] : files) {
        if (out.size() >= kMaxConversations)
            break;
        // A transcript without a prompt of the owner's (only a /model, say)
        // is no conversation to go back to.
        std::string title = firstPrompt(path.string());
        if (title.empty())
            continue;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::file_clock::to_sys(time).time_since_epoch());
        out.push_back({path.stem().string(), std::move(title), seconds.count()});
    }
    done(std::move(out));
}

// `pvai run --detached`, which registers like any other wrapper.
// PVAI_CLAUDE names the binary (an empty value: none), as PVAI_CODEX does.
std::string ClaudeBackend::binary() const
{
    if (const char *chosen = std::getenv("PVAI_CLAUDE"))
        return *chosen && ::access(chosen, X_OK) == 0 ? chosen : std::string();
    return findBinary("claude");
}

void ClaudeBackend::start(const StartRequest &request, Started done)
{
    if (!request.resume.empty()) {
        struct stat st;
        if (!goodConversationId(request.resume)
            || ::stat((projectDir(request.cwd) + "/" + request.resume + ".jsonl").c_str(), &st) != 0)
            return done({}, "unknown-conversation");
    }
    const std::string id = "w" + randomHex(4);
    std::vector<std::string> argv = {selfExe(), "run", "--detached", "--id", id};
    if (!request.label.empty()) {
        argv.push_back("--name");
        argv.push_back(request.label);
    }
    argv.push_back("--");
    argv.push_back("claude");
    if (!request.resume.empty()) {
        argv.push_back("--resume");
        argv.push_back(request.resume);
    }
    argv.insert(argv.end(), request.args.begin(), request.args.end());
    spawnDetached(request.cwd, argv);
    done(id, {});
}

std::unique_ptr<ClaudeSession> ClaudeBackend::attach(Conn &wrapper, const json &registration)
{
    if (registration.value("detached", false))
        return std::make_unique<DetachedClaudeSession>(host_, wrapper, registration);
    return std::make_unique<TerminalClaudeSession>(host_, wrapper, registration);
}

} // namespace pvai
