#include "claude.h"
#include "screen.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>

namespace pvai {

namespace {

// What Claude Code writes into the transcript when the owner interrupts a
// turn with Esc; no hook fires for that.
const char *kInterruptMark = "[Request interrupted by user";
constexpr auto kSubmitTimeout = std::chrono::seconds(15);
constexpr auto kTypedTimeout = std::chrono::seconds(5); // the wrapper checks for up to 2 s
constexpr int kTypedTries = 3; // typed checks that may fail before the prompt waits for the owner
constexpr auto kModelTimeout = std::chrono::seconds(30); // a /model with no output by then has failed
constexpr auto kRetryAfter = std::chrono::milliseconds(600);
constexpr auto kTypedRetryBase = std::chrono::seconds(2); // after a failed typed check: 2 s, 4 s, ... while the screen stays the same
constexpr auto kTailEvery = std::chrono::milliseconds(300);
constexpr auto kSubagentsEvery = std::chrono::seconds(1);
// An idle session with nothing queued polls slower; a hook (a turn
// starting) brings the fast rhythm back at once.
constexpr auto kTailIdleEvery = std::chrono::milliseconds(1500);
constexpr auto kSubagentsIdleEvery = std::chrono::seconds(3);
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
    if (auto text = firstString(input, {"command", "file_path", "notebook_path", "url", "pattern", "path", "query", "skill", "prompt"}))
        return *text;
    return input.dump();
}

// AskUserQuestion's questions with their choices, as dialogInfo.questions.
json questionsOf(const json &input)
{
    json out = json::array();
    if (!input.is_object() || !input.contains("questions") || !input["questions"].is_array())
        return out;
    for (const auto &question : input["questions"]) {
        json options = json::array();
        if (question.is_object() && question.contains("options") && question["options"].is_array())
            for (const auto &option : question["options"])
                options.push_back({{"label", str(option, "label")}, {"description", str(option, "description")}});
        const bool multi = question.is_object() && question.value("multiSelect", false);
        json entry = {{"question", str(question, "question")}, {"header", str(question, "header")},
                      {"multiSelect", multi}, {"options", options}};
        if (multi)
            entry["selected"] = json::array();
        out.push_back(std::move(entry));
    }
    return out;
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
        if (!entry.is_object() || isBookkeeping(entry) || entry.value("isSidechain", false))
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
        if (text.empty() || isMachinePrompt(text))
            continue;
        return conversationTitle(std::move(text));
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
    , screenModes_(registration.value("modes", false))
{
    canClear_ = registration.value("clear", false);
    if (const json v = registration.value("version", json()); v.is_string())
        wrapperVersion_ = cut(oneLine(v.get<std::string>(), 64), 32);
    canPace_ = registration.value("paced", false);
    submitCheck_ = registration.value("submitCheck", false);
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
                const auto seen = tail_.turnRunningSeen();
                if (seen ? *seen : turnRunning(transcript_))
                    state_ = "busy";
                // A limit end is a turn end in the file, yet Claude Code
                // goes on at the reset: waiting, while that is ahead.
                syncMarks();
                if (tail_.limited().is_object()) {
                    if (tail_.limited().value("model", false)) {
                        // A model's quota: idle at once, the owner switches model.
                        state_ = "idle";
                    } else if (limitAhead(tail_.limited())) {
                        limited_ = tail_.limited();
                        state_ = "busy";
                    } else {
                        state_ = "idle";
                    }
                }
            }
        }
        claimKept();
        if (wrapperVersion_ != kVersion)
            host_.log(id_ + ": taken on, its pvai run is " + (wrapperVersion_.empty() ? std::string("older than 0.5.52") : wrapperVersion_)
                      + ", the daemon " + kVersion);
        host_.log(id_ + ": taken on while running, " + (transcript_.empty() ? "no transcript known" : transcript_ + ": " + state_));
    }
}

json ClaudeSession::describe() const
{
    json entry = Session::describe();
    entry["claudeSession"] = claudeSession_.empty() ? json() : json(claudeSession_);
    entry["wrapperVersion"] = wrapperVersion_.empty() ? json() : json(wrapperVersion_);
    if (wrapperVersion_ != kVersion)
        entry["stale"] = true;
    json running = json::array();
    for (const auto &[id, agent] : subagents_)
        if (!agent.entry.is_null())
            running.push_back(agent.entry);
    sortByStart(running);
    entry["subagents"] = std::move(running);
    entry["limited"] = limited_;
    if (inFlight_) // typed, not confirmed yet
        entry["queued"] = entry.value("queued", 0) + 1;
    return entry;
}

bool ClaudeSession::wrapperTooOld() const
{
    // A wrapper that reports no version (before 0.5.52) is stale but served.
    return !wrapperVersion_.empty() && compareVersions(wrapperVersion_, kMinWrapperVersion) < 0;
}

std::vector<Prompt> ClaudeSession::unsent() const
{
    std::vector<Prompt> out;
    if (inFlight_ && !inFlight_->modelSwitch)
        out.push_back(*inFlight_);
    for (const auto &p : Session::unsent())
        out.push_back(p);
    return out;
}

// Prompts kept from an ended session of this conversation go in front of
// the queue, in their order, and are delivered as usual.
void ClaudeSession::claimKept()
{
    if (claudeSession_.empty())
        return;
    const std::vector<Prompt> back = host_.takeKept(claudeSession_, cwd_);
    if (back.empty())
        return;
    for (auto it = back.rbegin(); it != back.rend(); ++it)
        queue_.push_front(*it);
    host_.log(id_ + ": " + std::to_string(back.size()) + " kept prompt(s) of conversation " + claudeSession_ + " queued again");
    changed();
}

// A slash or shell command went in: it ends with its output.
void ClaudeSession::markSlash(const Prompt &prompt)
{
    drain();
    slashPending_ = true;
    slashModel_ = prompt.modelSwitch;
    slashShell_ = prompt.text[0] == '!';
    slashMark_ = localDone();
    slashText_ = prompt.text;
    slashCompact_ = prompt.text == "/compact" || prompt.text.rfind("/compact ", 0) == 0;
    compactMark_ = tail_.compactions();
    // A /model that goes in has 30 s to show its output (see the tick).
    modelAt_.reset();
    if (prompt.modelSwitch)
        modelAt_ = Clock::now() + kModelTimeout;
}

void ClaudeSession::submit(const Prompt &prompt)
{
    // A slash command or a shell command (!): no UserPromptSubmit, it ends
    // with its output in the transcript.
    const bool slash = prompt.text[0] == '/' || prompt.text[0] == '!';
    const bool verify = verify_ && !sendUnchecked_;
    sendUnchecked_ = false;
    {
        std::string shown = prompt.text.substr(0, 40);
        std::replace(shown.begin(), shown.end(), '\n', ' ');
        host_.log(id_ + ": types " + shown + (verify ? " (verify)" : " (unchecked)"));
    }
    typedAt_ = static_cast<long long>(std::time(nullptr));
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
        markSlash(prompt);
        state_ = "busy";
        delivered();
        return;
    }
    if (slash) {
        markSlash(prompt);
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
    if (dialog_ == "question" && dialogInfo_.is_object() && dialogInfo_.contains("questions")
        && (choice.rfind("option:", 0) == 0 || choice.rfind("text:", 0) == 0 || choice == "next"))
        return answerQuestion(choice);
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
        retryIfBlocked();
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
        retryIfBlocked();
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

// Claude Code's question dialog: a digit picks a choice (single select:
// on to the next question; multi select: toggles it), Tab goes on from a
// multi-select question, the choice after the last opens a field for
// typed words. With more than one question a review comes last, which
// `allow` submits and `deny` cancels.
std::string ClaudeSession::answerQuestion(const std::string &choice)
{
    json &questions = dialogInfo_["questions"];
    const int count = static_cast<int>(questions.size());
    const int current = dialogInfo_.value("current", 0);
    if (current < 0 || current >= count)
        return "bad-choice"; // the review: allow or deny
    json &question = questions[static_cast<std::size_t>(current)];
    const int options = static_cast<int>(question["options"].size());
    const bool multi = question.value("multiSelect", false);
    if (choice == "next") {
        if (!multi)
            return "bad-choice";
        wrapper_.send({{"type", "keys"}, {"data", "\t"}});
        questionAnswered(true);
        return {};
    }
    if (choice.rfind("option:", 0) == 0) {
        const std::string number = choice.substr(7);
        if (number.empty() || number.size() > 1 || number[0] < '1' || number[0] - '0' > options)
            return "bad-choice";
        wrapper_.send({{"type", "keys"}, {"data", number}});
        if (multi) {
            json &selected = question["selected"];
            const int n = number[0] - '0';
            auto it = std::find(selected.begin(), selected.end(), json(n));
            if (it != selected.end())
                selected.erase(it);
            else
                selected.push_back(n);
            changed();
            return {};
        }
        questionAnswered(true);
        return {};
    }
    // text:<words>: the field after the choices, the words, Return.
    const std::string words = choice.substr(5);
    if (multi || words.empty() || words.size() > 2000 || words.find_first_of("\r\n\x1b") != std::string::npos)
        return "bad-choice";
    if (!canPace_)
        return "not-supported";
    wrapper_.send({{"type", "paced"}, {"gapMs", 300},
                   {"keys", {std::to_string(options + 1), "\x1b[200~" + words + "\x1b[201~", "\r"}}});
    questionAnswered(true);
    return {};
}

// One question done: the next one, the review after the last of several;
// a single question is done with its answer and the turn goes on.
void ClaudeSession::questionAnswered(bool advance)
{
    const int count = static_cast<int>(dialogInfo_["questions"].size());
    const int next = dialogInfo_.value("current", 0) + (advance ? 1 : 0);
    if (next >= count && count == 1) {
        dialog_.clear();
        state_ = "busy";
    } else {
        dialogInfo_["current"] = std::min(next, count);
    }
    changed();
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
        retryIfBlocked();
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
    limited_ = nullptr;
    stopCheck_.reset();
    modelDone("failed", "ended");
    escapeChecks_.clear();
    submitDeadline_.reset();
    changed();
    return {};
}

// Claude Code's own /model, queued like a prompt: it goes in when the
// prompt is free. The answer waits for the command's output in the
// transcript ("Set model to" or "Kept model as"); a dialog Claude Code
// shows first (usage credits) is the app's to answer as a screen dialog.
std::string ClaudeSession::setModel(const std::string &model, const json &ref)
{
    if (!modelTarget_.empty())
        return "busy";
    modelTarget_ = model;
    modelRef_ = ref;
    Prompt command{"/model " + model, false, json()};
    command.modelSwitch = true;
    prompt(command);
    return {};
}

void ClaudeSession::modelDone(const std::string &state, const std::string &error)
{
    if (modelTarget_.empty())
        return;
    const std::string model = modelTarget_;
    const json ref = modelRef_;
    modelAt_.reset();
    modelTarget_.clear();
    modelRef_ = nullptr;
    slashModel_ = false;
    modelEvent(ref, model, state, error);
}

// The wrapper empties the box; its `draft` and `screen` say when it is
// free, and the queue goes on from there.
std::string ClaudeSession::clear()
{
    if (!canClear_)
        return "not-supported";
    if (!clearable())
        return "busy";
    wrapper_.send({{"type", "clear"}});
    retryIfBlocked();
    return {};
}

void ClaudeSession::terminate()
{
    modelDone("failed", "ended");
    wrapper_.send({{"type", "terminate"}});
}

// A wrapper of another pvai only starts Claude Code again inside itself,
// and stays what it is: it ends instead, and the daemon starts a wrapper of
// the installed pvai with the same session id and conversation.
std::string DetachedClaudeSession::restart()
{
    if (wrapperVersion() == kVersion)
        return ClaudeSession::restart();
    if (!restartable())
        return "not-restartable";
    if (state_ != "idle" || draft_)
        return "busy";
    replacement_ = StartRequest{cwd(), label(), {}, claudeSession(), id()};
    host_.log(id() + ": restart replaces its pvai run " + (wrapperVersion().empty() ? std::string("(unversioned)") : wrapperVersion()));
    stopping_ = true;
    terminate();
    state_ = "starting";
    changed();
    return {};
}

std::string DetachedClaudeSession::stop()
{
    stopping_ = true;
    terminate();
    return {};
}

json ClaudeSession::history(int limit)
{
    drain();
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
    // every event twice, a moment apart; an exact repeat later is a new one.
    const auto field = [&](const char *key) {
        const json value = event.contains(key) ? event[key] : json();
        return value.is_string() ? value.get<std::string>().substr(0, 200) : std::string();
    };
    const std::string fingerprint = name + '\n' + field("session_id") + '\n' + field("tool_use_id") + '\n' + field("tool_name")
                                  + '\n' + field("message") + '\n' + field("timestamp") + '\n' + field("source");
    const auto now = Clock::now();
    nextTail_ = std::min(nextTail_, now + kTailEvery);
    nextSubagents_ = std::min(nextSubagents_, now + kSubagentsEvery);
    const bool twice = fingerprint == lastEvent_ && now - lastEventAt_ < std::chrono::milliseconds(500);
    lastEvent_ = fingerprint;
    lastEventAt_ = now;
    if (twice)
        return;
    if (!ours(event.value("session_id", ""), name == "SessionStart" ? event.value("source", "") : "")) {
        host_.log(id_ + ": " + name + " of another Claude Code (" + event.value("session_id", "") + "), dropped");
        return;
    }
    if (const std::string mode = event.value("permission_mode", ""); !mode.empty())
        noteMode(mode);
    bool known = false;
    if (const std::string id = event.value("session_id", ""); !id.empty() && id != claudeSession_) {
        claudeSession_ = id;
        known = true;
        claimKept();
    }
    if (const std::string path = event.value("transcript_path", ""); !path.empty() && path != transcript_) {
        known = true;
        transcript_ = path;
        tail_.follow(path);
        tail_.poll();
        wakeMark_ = tail_.wakeTurns();
        syncMarks();
        limited_ = nullptr;
        subagents_.clear();
        nextSubagents_ = Clock::now();
        historyReset();
    }
    if (known)
        remember();

    const bool trust = dialog_ == "trust"; // answered in the terminal: the next hook says so
    if (name != "Notification" && name != "PermissionRequest")
        dialog_.clear();
    std::optional<Prompt> confirmed; // the prompt this hook confirms, when the daemon typed it
    if (name == "UserPromptSubmit" || name == "Stop" || name == "SessionStart") {
        blocked_ = false; // the prompt is Claude's again
        retryAt_.reset();
        typedDeadline_.reset();
        if (name == "UserPromptSubmit") {
            if (!inFlight_)
                dropQueued(event.value("prompt", ""));
            confirmed = inFlight_;
            delivered();
        } else if (inFlight_) { // a turn ended or started over without it
            queue_.push_front(*inFlight_);
            inFlight_.reset();
        }
        submitDeadline_.reset();
        stopCheck_.reset();
        catchUp();
    }
    if (name == "SessionStart") {
        limited_ = nullptr;
        const std::string source = event.value("source", "");
        if (state_ == "starting" || (state_ == "waiting" && trust) || source == "clear" || source == "resume")
            state_ = "idle";
        if (source == "clear") { // a new transcript: its output count starts at 0
            endSlash();
        }
    } else if (name == "UserPromptSubmit") {
        limited_ = nullptr;
        state_ = "busy";
        promptVoice(event.value("prompt", ""), confirmed ? &*confirmed : nullptr);
    } else if (name == "Stop" && limited_.is_object() && limitAhead(limited_)) {
        // The limit ended the turn in the file, but Claude Code goes on by
        // itself at the reset: stay busy until it does.
    } else if (name == "Stop") {
        // A Stop while idle ends a turn no prompt started (a background task
        // or agent came back) that the transcript read had not shown yet:
        // catchUp() above took its wake along, so it is spoken here.
        if (state_ == "idle")
            wakeVoice();
        limited_ = nullptr;
        state_ = "idle";
        forceIdle_ = false;
        endSlash();
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
        if (asks) {
            dialogInfo_["questions"] = questionsOf(input);
            dialogInfo_["current"] = 0;
        }
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
            drain();
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
        usageChanged();
}

// The owner typed into the session. Into a dialog Claude waited on: they
// answer it, the turn goes on. An Esc may end the turn without a hook; the
// transcript tells, a moment later.
// `key` is what the wrapper saw: return, escape, digit or other (arrow keys,
// terminal replies); an older wrapper says nothing and every key counts.
void ClaudeSession::onInput(bool escape, const std::string &key)
{
    if (escape && state_ != "idle")
        armEscapeCheck();
    if (key == "other")
        return;
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

// The permission mode as a hook or the screen says; a change under way
// goes on with the next press or is done.
void ClaudeSession::noteMode(const std::string &mode)
{
    if (mode_ != json(mode)) {
        mode_ = mode;
        changed();
    }
    if (modeTarget_.empty() || mode == modeSeen_) // no news (a hook saying it again)
        return;
    modeSeen_ = mode;
    if (mode == modeTarget_)
        return modeDone("done");
    if (modePresses_ > 0 && mode == modeFrom_)
        return modeDone("failed", "unavailable"); // once round: not in this session's cycle
    if (modeDeadline_ && modePresses_ > 0) // this press has shown its mode: the next one
        pressModeKey();
}

std::string ClaudeSession::setMode(const std::string &mode, const json &ref)
{
    bool known = false;
    for (const auto &m : ClaudeBackend::claudeModes())
        known = known || str(m, "id") == mode;
    if (!known)
        return "unknown-mode";
    if (!screenModes_)
        return "not-supported"; // a wrapper before 0.5.25 does not report the footer
    if (state_ == "starting" || state_ == "waiting")
        return "busy";
    if (!modeTarget_.empty())
        return "busy";
    modeRef_ = ref;
    if (mode_ == json(mode)) {
        modeEvent(ref, "done");
        return {};
    }
    modeTarget_ = mode;
    modeFrom_ = mode_.is_string() ? mode_.get<std::string>() : "";
    modeSeen_ = modeFrom_;
    modePresses_ = 0;
    pressModeKey();
    return {};
}

void ClaudeSession::pressModeKey()
{
    if (modePresses_ >= 6)
        return modeDone("failed", "unavailable");
    ++modePresses_;
    wrapper_.send({{"type", "keys"}, {"data", "\x1b[Z"}}); // Shift+Tab
    modeDeadline_ = Clock::now() + std::chrono::seconds(3);
}

void ClaudeSession::modeDone(const std::string &state, const std::string &error)
{
    modeTarget_.clear();
    modeFrom_.clear();
    modeSeen_.clear();
    modeDeadline_.reset();
    modePresses_ = 0;
    const json ref = modeRef_;
    modeRef_ = nullptr;
    modeEvent(ref, state, error);
}

// The dialog info of a screen dialog; a session survey also lists its keys.
static json screenInfo(const std::string &text)
{
    json info = dialogInfo("", text, "");
    if (const auto options = surveyOptions(text); !options.empty()) {
        info["options"] = json::array();
        for (const auto &[id, label] : options)
            info["options"].push_back({{"id", id}, {"label", label}});
    }
    return info;
}

// What the owner is told about a screen: the last lines above the prompt box
// (the wrapper's snapshot shows rules as ───), or the dialog's own first
// lines when no box shows; at most 4 lines and 300 bytes.
static std::string screenGist(const std::string &screen)
{
    std::vector<std::string> lines;
    std::istringstream in(screen);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    const std::string rule = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80";
    int top = -1;
    for (int i = static_cast<int>(lines.size()) - 1; i > 0; --i) {
        if (lines[i - 1] == rule && (lines[i].compare(0, 3, "\xE2\x9D\xAF") == 0 || lines[i].compare(0, 2, "! ") == 0)) {
            top = i - 1;
            break;
        }
    }
    std::vector<std::string> shown;
    if (top >= 0) {
        for (int i = top - 1; i >= 0 && shown.size() < 4; --i)
            if (!trim(lines[i], " \t\r").empty() && lines[i] != rule)
                shown.insert(shown.begin(), trim(lines[i], " \t\r"));
    } else {
        for (std::size_t i = 0; i < lines.size() && shown.size() < 4; ++i)
            if (!trim(lines[i], " \t\r").empty())
                shown.push_back(trim(lines[i], " \t\r"));
    }
    std::string out;
    for (const auto &line : shown)
        out += (out.empty() ? "" : "\n") + line;
    if (out.size() > 300)
        out.resize(utf8Boundary(out, 300));
    return out;
}

// A dialog no hook reports, read off the screen by the wrapper (empty: it
// is gone). At a free prompt it counts at once; in a turn only after a
// moment, since a permission dialog's hook may still be on its way.
void ClaudeSession::onScreen(const std::string &text, bool prompt, const std::string &mode)
{
    if (!mode.empty())
        noteMode(mode);
    screenText_ = text;
    screenAt_.reset();
    promptFree_ = prompt;
    if (blocked_) {
        if (!text.empty() && dialogInfo_.value("text", json()) != json(text)) {
            dialogInfo_ = screenInfo(text);
            changed();
        }
        // Never while a dialog (a session survey takes single keys) stands,
        // and not before the pause after a failed typed check is over.
        if (prompt && retryOnPrompt_ && text.empty() && !typedPause())
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
                if (slashModel_)
                    modelDone("done");
                state_ = "idle";
            }
            deliver();
        } else {
            dialogInfo_ = screenInfo(text);
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
    modelDone("failed", "ended");
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
void ClaudeSession::onTyped(bool ok, const std::string &screen, const std::string &how)
{
    if (!inFlight_ || !typedDeadline_)
        return;
    typedDeadline_.reset();
    if (ok) {
        failedScreen_.clear();
        sameFails_ = 0;
    }
    if (ok && !how.empty())
        host_.log(id_ + ": typed (" + how + ")");
    if (!ok) {
        // Retried by itself a few times (a screen read between two frames);
        // then it waits for the owner. Unbounded, a box that never shows the
        // paste (Claude Code busy compacting) typed the same /compact 146
        // times in 3 minutes.
        if (++inFlight_->typedFailures >= kTypedTries) {
            host_.log(id_ + ": typed check failed " + std::to_string(kTypedTries) + " times, waits for the owner");
            const std::string text = inFlight_->text.substr(0, 40);
            inFlight_->typedFailures = 0;
            notDelivered(screen, false);
            attention("stuck", text);
            return;
        }
        // Typed again only after a pause that doubles while the screen is
        // the same (old wrappers send no snapshot with `screen`, so this
        // cannot wait for a change): 2 s, 4 s.
        sameFails_ = screen == failedScreen_ ? sameFails_ + 1 : 1;
        failedScreen_ = screen;
        host_.log(id_ + ": typed check failed");
        notDelivered(screen, true);
        typedPauseUntil_ = Clock::now() + kTypedRetryBase * (1 << std::min(sameFails_ - 1, 3));
        retryAt_ = *typedPauseUntil_;
        return;
    }
    if (inFlight_->text[0] == '/' || inFlight_->text[0] == '!') {
        // No hook for a slash or shell command: in is in. It ends with its output.
        markSlash(*inFlight_);
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
    if (!inFlight_)
        return;
    // The paste may still stand in the box (its Return did not take): typed
    // again it would be added to it, and the owner's Return would send both.
    // The owner sees the screen as it was.
    if (canClear_)
        wrapper_.send({{"type", "clear"}});
    notDelivered(screen, false);
}

// A prompt Claude Code took after the check had said no (a screen read
// between two frames): UserPromptSubmit names it, and it leaves the queue
// instead of going in a second time.
void ClaudeSession::dropQueued(const std::string &text)
{
    std::string sent = trim(stripPasteFrame(text), " \t\r\n");
    if (sent.compare(0, std::strlen(kMicMark), kMicMark) == 0)
        sent = trim(sent.substr(std::strlen(kMicMark)), " \t\r\n");
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->text[0] == '/' || it->text[0] == '!')
            continue;
        // A long one also inside a longer prompt: a copy left in the box
        // went out with it (a short one could be the owner's own words).
        const std::string queued = trim(it->text, " \t\r\n");
        if (queued != sent && (queued.size() < 40 || sent.find(queued) == std::string::npos))
            continue;
        inFlight_ = *it;
        queue_.erase(it);
        host_.log(id_ + ": a prompt the check had refused was taken: not sent again");
        return;
    }
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
    turnOver();
    const bool asked = dialog_ == "screen";
    blocked_ = true;
    retryOnPrompt_ = retry;
    retryAt_.reset();
    if (retry) // the screen may not change: look again after a moment
        retryAt_ = Clock::now() + kRetryAfter;
    screenFrom_ = "idle";
    state_ = "waiting";
    dialog_ = "screen";
    dialogInfo_ = screenInfo(screen);
    if (!asked)
        attention("screen", screenGist(screen));
    changed();
}

// The keys went to a screen the daemon is waiting on: look at it again in a moment.
void ClaudeSession::retryIfBlocked()
{
    if (blocked_) {
        typedPauseUntil_.reset(); // the owner acted
        retryOnPrompt_ = true;
        retryAt_ = Clock::now() + kRetryAfter;
    }
}

// A slash command is over (or never ran): a model change that waited for it fails.
void ClaudeSession::endSlash()
{
    slashPending_ = false;
    if (slashModel_)
        modelDone("failed", "interrupted");
}

bool ClaudeSession::typedPause() const
{
    return typedPauseUntil_ && Clock::now() < *typedPauseUntil_;
}

void ClaudeSession::unblock()
{
    blocked_ = false;
    typedPauseUntil_.reset();
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
    dialogInfo_ = screenInfo(screenText_);
    attention("screen", screenGist(screenText_));
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

// The wrapper pressed Return three times and the box still held the text:
// Claude Code never took it, and the wrapper has emptied the box (its
// `draft` message, which came first, keeps the next prompt waiting for it).
void ClaudeSession::onSubmitted(bool ok, const std::string &why)
{
    if (ok && !why.empty())
        host_.log(id_ + ": submitted (" + why + ")");
    if (ok || !submitCheck_)
        return;
    if (slashPending_) {
        host_.log(id_ + ": " + slashText_.substr(0, 40) + " stayed in the prompt box, cleared");
        slashPending_ = false;
        if (slashModel_)
            modelDone("failed", "did not run");
        if (state_ == "busy") {
            state_ = "idle";
            turnOver();
            deliver();
        }
        attention("stuck", slashText_.substr(0, 40));
        changed();
        return;
    }
    if (!inFlight_ || state_ != "busy")
        return;
    const std::string text = inFlight_->text;
    std::string shown = text.substr(0, 40);
    std::replace(shown.begin(), shown.end(), '\n', ' ');
    // Never typed again by itself: Claude Code may have taken it after all
    // (a compaction it started first), and a second copy would be a second
    // turn. The owner sees the screen and decides.
    host_.log(id_ + ": the Return did not take for " + shown);
    notDelivered("Claude Code did not take the Return for:\n" + shown, false);
    attention("stuck", text.substr(0, 40));
}

void ClaudeSession::tick(Clock::time_point now)
{
    runEscapeChecks(now);
    if (stopCheck_ && now >= *stopCheck_) {
        // The turn ended in the transcript and no Stop came: Claude Code
        // writes no Stop for some ends. Still busy only when it went on.
        stopCheck_.reset();
        if (state_ == "busy" && !limited_.is_object() && !slashPending_ && !inFlight_ && !turnRunning(transcript_)) {
            state_ = "idle";
            turnOver();
            submitDeadline_.reset();
            deliver();
            changed();
        }
    }
    if (limited_.is_object()) {
        // Gone on without a line we know, or the session ended some other way.
        const json &at = limited_.value("resetsAt", json());
        if ((state_ != "busy" && state_ != "waiting")
            || (at.is_number() && at.get<long long>() + kLimitGrace.count() < static_cast<long long>(std::time(nullptr)))) {
            limited_ = nullptr;
            if (state_ == "busy" && !slashPending_) {
                state_ = "idle";
                turnOver();
                deliver();
            }
            changed();
        }
    }
    if (typedDeadline_ && now >= *typedDeadline_) // the wrapper never answered
        notDelivered("", true);
    if (retryAt_ && now >= *retryAt_) {
        retryAt_.reset();
        if (blocked_ && promptFree_ && screenText_.empty() && !typedPause())
            unblock();
    }
    if (modeDeadline_ && now >= *modeDeadline_)
        modeDone("failed", "no change on the screen");
    if (modelAt_ && now >= *modelAt_) {
        // The /model shows no output and no dialog the daemon knows of (an
        // old wrapper cannot say): it has failed, and the session is free.
        modelAt_.reset();
        if (slashPending_ && slashModel_) {
            slashPending_ = false;
            modelDone("failed", "no answer");
            if (state_ == "busy") {
                state_ = "idle";
                turnOver();
                deliver();
            }
            changed();
        }
    }
    if (screenAt_ && now >= *screenAt_) {
        screenAt_.reset();
        if (!screenText_.empty() && dialog_.empty() && state_ == "busy")
            openScreenDialog();
    }
    if (now >= nextTail_ || (!quiet() && now >= lastTail_ + kTailEvery)) {
        pollTranscript();
        lastTail_ = now;
        nextTail_ = now + (quiet() ? kTailIdleEvery : kTailEvery);
    }
    if (now >= nextSubagents_ || (!quiet() && now >= lastSubagents_ + kSubagentsEvery)) {
        scanSubagents(now);
        lastSubagents_ = now;
        nextSubagents_ = now + (quiet() && !hasRunningSubagent() ? kSubagentsIdleEvery : kSubagentsEvery);
    }
}

bool ClaudeSession::quiet() const
{
    return state_ == "idle" && queue_.empty() && !slashPending_ && !inFlight_;
}

bool ClaudeSession::hasRunningSubagent() const
{
    return std::any_of(subagents_.begin(), subagents_.end(), [](const auto &agent) { return !agent.second.entry.is_null(); });
}

std::optional<Clock::time_point> ClaudeSession::deadline() const
{
    Clock::time_point next = std::min(nextTail_, nextSubagents_);
    if (!quiet())
        next = std::min({next, lastTail_ + kTailEvery, lastSubagents_ + kSubagentsEvery});
    for (const auto &at : escapeChecks_)
        next = std::min(next, at);
    if (stopCheck_)
        next = std::min(next, *stopCheck_);
    if (submitDeadline_)
        next = std::min(next, *submitDeadline_);
    if (screenAt_)
        next = std::min(next, *screenAt_);
    if (modeDeadline_)
        next = std::min(next, *modeDeadline_);
    if (modelAt_)
        next = std::min(next, *modelAt_);
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
        // The transcript shows a turn that began after the prompt went in
        // (the session was idle): it went in, only UserPromptSubmit never
        // came (a slow or lost hook). Typed again it would arrive twice.
        // Only a line written since the typing counts: an old one that left
        // the state at running (a slash command writes no turn end) is not
        // this prompt.
        long long runAt = 0;
        if (state_ == "busy" && !slashPending_ && !transcript_.empty() && turnRunning(transcript_, &runAt) && runAt >= typedAt_) {
            host_.log(id_ + ": no UserPromptSubmit in 15 s, but the transcript shows the turn: taken as delivered");
            if (inFlight_) {
                promptVoice(inFlight_->text, &*inFlight_);
                delivered();
            }
            changed();
        } else if (inFlight_ && state_ == "busy" && !slashPending_) {
            snapshotPending_ = true;
            wrapper_.send({{"type", "snapshot"}});
        } else if (state_ == "busy" && !slashPending_) {
            state_ = "idle";
            turnOver();
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
    limited_ = nullptr;
    turnOver();
    submitDeadline_.reset();
    // Interrupted by the app and nothing in the transcript says so: the
    // session hung (a Claude Code without a login answers every prompt with
    // "Not logged in" and no hook); what waited for it goes, so it does not
    // land in the same hole.
    const bool cleared = !seen && !slashPending_;
    if (cleared)
        queue_.clear();
    forceIdle_ = false;
    slashPending_ = false;
    if (slashModel_ || cleared) // a kept queue still types the /model
        modelDone("failed", "interrupted");
    deliver();
    changed();
}

// What the transcript holds up to now belongs to the turn a hook reports
// on: an answer to a task notification in it started no turn of its own.
void ClaudeSession::catchUp()
{
    if (transcript_.empty())
        return;
    drain();
    tail_.settleWakes();
    wakeMark_ = tail_.wakeTurns();
    endMark_ = tail_.turnEndCount();
}

void ClaudeSession::syncMarks()
{
    hitMark_ = tail_.limitHits();
    clearMark_ = tail_.limitClears();
    endMark_ = tail_.turnEndCount();
}

// A line written just now. A fork copies the earlier conversation into its
// new transcript: such a line is old, and no news.
static bool freshLine(const std::string &iso)
{
    std::tm tm{};
    std::istringstream in(iso);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail())
        return false;
    const std::time_t at = timegm(&tm), now = std::time(nullptr);
    return at - now < 600 && now - at < 600;
}

bool ClaudeSession::limitAhead(const json &limited)
{
    const json &at = limited.value("resetsAt", json());
    return !at.is_number() || at.get<long long>() > static_cast<long long>(std::time(nullptr));
}

// Claude Code's session id and transcript, kept beside the wrapper's socket
// so that a daemon started again knows them before the next hook.
void ClaudeSession::remember() const
{
    std::ofstream(runtimeDir() + "/" + id_ + ".claude")
        << json({{"claudeSession", claudeSession_}, {"transcript", transcript_}}).dump() << "\n";
}

// Entries the transcript got since the last read go to the subscribers, so
// that a read for another purpose (history, a dialog's tool call) loses none.
// The cache, wake and slash bookkeeping stays in pollTranscript: it reacts
// to state changes and runs on the tick, and these reads only look.
void ClaudeSession::drain()
{
    sendEntries(tail_.poll());
}

void ClaudeSession::pollTranscript()
{
    auto fresh = tail_.poll();
    if (usage_.is_object() && usage_.value("cache", json()) != tail_.cache()) {
        usage_["cache"] = tail_.cache();
        usageChanged();
    }
    // The usage limit: hit (busy until the reset), gone on, or a turn end
    // the Stop hook should have followed.
    if (tail_.limitHits() < hitMark_ || tail_.limitClears() < clearMark_ || tail_.turnEndCount() < endMark_) { // another file
        syncMarks();
        limited_ = nullptr;
    }
    if (tail_.limitHits() != hitMark_) {
        hitMark_ = tail_.limitHits();
        clearMark_ = tail_.limitClears();
        if (tail_.limited().is_object() && tail_.limited().value("model", false)) {
            // A model's quota (Fable's): Claude Code does not go on by itself
            // at a reset, so the session goes idle and the app is asked once
            // to offer a model switch. A line from copied history is no news.
            if (freshLine(tail_.limitedAt())) {
                if (state_ == "busy" && !slashPending_) {
                    state_ = "idle";
                    turnOver();
                }
                stopCheck_.reset();
                attention("limited", tail_.limited().value("text", ""));
                changed();
            }
        } else if (tail_.limited().is_object()) {
            limited_ = tail_.limited();
            state_ = "busy";
            stopCheck_.reset();
            attention("limited", limited_.value("text", ""));
            changed();
        }
    } else if (tail_.limitClears() != clearMark_) {
        clearMark_ = tail_.limitClears();
        if (limited_.is_object()) {
            limited_ = nullptr;
            changed();
        }
    }
    if (tail_.turnEndCount() != endMark_) {
        endMark_ = tail_.turnEndCount();
        if (state_ == "busy" && !limited_.is_object() && !tail_.limited().is_object() && !slashPending_)
            stopCheck_ = Clock::now() + kStopWait;
    }
    // A background task or agent finished and Claude Code took it up: a
    // turn without UserPromptSubmit, ended by Stop as usual.
    if (tail_.wakeTurns() != wakeMark_) {
        wakeMark_ = tail_.wakeTurns();
        if (state_ == "idle") {
            state_ = "busy";
            wakeVoice();
            changed();
        }
    }
    if (slashPending_ && (localDone() > slashMark_ || (slashCompact_ && tail_.compactions() > compactMark_))) {
        slashPending_ = false;
        if (slashModel_) {
            const std::string &out = tail_.lastLocalOutput();
            if (out.empty() || out.rfind("Set model to", 0) == 0)
                modelDone("done");
            else
                modelDone("failed", out);
        }
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
    std::set<std::string> seen;
    for (const auto &file : fs::directory_iterator(dir, ec)) {
        const std::string name = file.path().filename().string();
        const std::string prefix = "agent-", metaSuffix = ".meta.json";
        if (name.size() <= prefix.size() + metaSuffix.size() || name.rfind(prefix, 0) != 0
            || name.compare(name.size() - metaSuffix.size(), metaSuffix.size(), metaSuffix) != 0)
            continue;
        const std::string id = name.substr(prefix.size(), name.size() - prefix.size() - metaSuffix.size());
        seen.insert(id);
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
    // Agents whose files are gone (and finished ones of a vanished folder)
    // are dropped; a running one that vanished ends.
    if (!ec) {
        for (auto it = subagents_.begin(); it != subagents_.end();) {
            if (seen.count(it->first)) {
                ++it;
                continue;
            }
            news = news || !it->second.entry.is_null();
            it = subagents_.erase(it);
        }
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
    return homeDir() + "/.claude/projects/" + name;
}

// The transcripts of `cwd`, newest first, without the sessions that run.
void ClaudeBackend::conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done)
{
    std::set<std::string> running;
    for (Session *session : host_.sessions())
        if (const auto *claude = dynamic_cast<ClaudeSession *>(session); claude && !claude->claudeSession().empty())
            running.insert(claude->claudeSession());
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    std::map<std::filesystem::path, std::uintmax_t> sizes;
    std::error_code ec;
    for (const auto &file : std::filesystem::directory_iterator(projectDir(cwd), ec))
        if (file.path().extension() == ".jsonl" && !running.count(file.path().stem().string())) {
            files.emplace_back(file.last_write_time(ec), file.path());
            sizes[file.path()] = file.file_size(ec);
        }
    if (titles_.size() > 5000)
        titles_.clear();
    std::sort(files.begin(), files.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    std::vector<Conversation> out;
    for (const auto &[time, path] : files) {
        if (out.size() >= kMaxConversations)
            break;
        // A transcript without a prompt of the owner's (only a /model, say)
        // is no conversation to go back to.
        // A title is read once per (mtime, size) of the file.
        auto &cached = titles_[path.string()];
        if (cached.size != sizes[path] || cached.mtime != time || !cached.known) {
            cached = {time, sizes[path], firstPrompt(path.string()), true};
        }
        std::string title = cached.title;
        if (title.empty())
            continue;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::file_clock::to_sys(time).time_since_epoch());
        out.push_back({path.stem().string(), std::move(title), seconds.count()});
    }
    done(std::move(out));
}

// The transcript and what Claude Code keeps beside it under the same id
// (a folder with subagents' transcripts and tool results); a running
// session's conversation stays.
void ClaudeBackend::forget(const std::string &cwd, const std::string &id, std::function<void(const std::string &)> done)
{
    if (!goodConversationId(id))
        return done("unknown-conversation");
    for (Session *session : host_.sessions())
        if (const auto *claude = dynamic_cast<ClaudeSession *>(session); claude && claude->claudeSession() == id)
            return done("running");
    const std::filesystem::path dir = projectDir(cwd);
    const std::filesystem::path transcript = dir / (id + ".jsonl");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(transcript, ec))
        return done("unknown-conversation");
    if (!std::filesystem::remove(transcript, ec) || ec)
        return done("failed");
    const std::filesystem::path beside = dir / id;
    if (std::filesystem::is_directory(std::filesystem::symlink_status(beside, ec)))
        std::filesystem::remove_all(beside, ec);
    host_.log("conversation " + id + " in " + cwd + " deleted");
    done({});
}

// `pvai run --detached`, which registers like any other wrapper.
json ClaudeBackend::claudeModes()
{
    return json::array({{{"id", "default"}, {"name", "Ask"}},
                        {{"id", "acceptEdits"}, {"name", "Accept edits"}},
                        {{"id", "plan"}, {"name", "Plan"}},
                        {{"id", "auto"}, {"name", "Auto"}},
                        {{"id", "bypassPermissions"}, {"name", "Bypass"}}});
}

json ClaudeBackend::modes() const
{
    return claudeModes();
}

// `claude auth status --json`: {"loggedIn": true, ...}.
void ClaudeBackend::refreshLogin(std::function<void(bool changed)> done)
{
    const std::string bin = binary();
    if (bin.empty())
        return done(false);
    host_.runProcess({bin, "auth", "status", "--json"}, std::chrono::seconds(30), [this, done](int, const std::string &output) {
        const json status = json::parse(output.substr(std::min(output.find('{'), output.size())), nullptr, false);
        const json now = status.is_object() && status.contains("loggedIn") && status["loggedIn"].is_boolean() ? status["loggedIn"] : json();
        const bool changed = now != loggedIn_;
        loggedIn_ = now;
        done(changed);
    });
}

std::string ClaudeBackend::binary() const
{
    return claudeBinary();
}

void ClaudeBackend::start(const StartRequest &request, Started done)
{
    if (!request.resume.empty()) {
        struct stat st;
        if (!goodConversationId(request.resume)
            || ::stat((projectDir(request.cwd) + "/" + request.resume + ".jsonl").c_str(), &st) != 0)
            return done({}, "unknown-conversation");
    }
    // The found program by its path: the daemon's PATH (started over ssh or
    // by a service) need not hold ~/.local/bin, where the app installs it.
    const std::string bin = binary();
    if (bin.empty())
        return done({}, "not-installed");
    const std::string id = request.id.empty() ? "w" + randomHex(4) : request.id;
    std::vector<std::string> argv = {selfExe(), "run", "--detached", "--id", id};
    if (!request.label.empty()) {
        argv.push_back("--name");
        argv.push_back(request.label);
    }
    argv.push_back("--");
    argv.push_back(bin);
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
