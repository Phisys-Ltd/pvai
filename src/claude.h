// Claude Code: each session is a `pvai run` wrapper around a claude process
// in a PTY. Prompts and answers to dialogs are typed into it; the hooks,
// the status line and the transcript report back.
#pragma once

#include "session.h"
#include "transcript.h"

#include <sys/types.h>

#include <filesystem>
#include <map>

namespace pvai {

class ClaudeSession : public Session {
public:
    // `wrapper` is the `pvai run` connection; the session ends with it.
    ClaudeSession(Host &host, Conn &wrapper, const json &registration);

    std::string assistant() const override { return "claude"; }
    json describe() const override;
    const std::string &claudeSession() const { return claudeSession_; }

    std::string answer(const std::string &choice) override;
    void interrupt() override;
    // Once Claude Code's own session id is known (`--resume` needs it), and
    // only under a wrapper that can (pvai 0.5.0 and later).
    bool restartable() const override { return canRestart_ && !claudeSession_.empty(); }
    std::string restart() override;
    json history(int limit) override;
    void tick(Clock::time_point now) override;
    std::optional<Clock::time_point> deadline() const override;

    // From the hook, the status line and the wrapper.
    void onHook(const json &event);
    void onStatus(const json &status);
    void onInput(bool escape);
    void onDraft(bool dirty);
    void onDialog(const std::string &kind);
    void onScreen(const std::string &text, bool prompt, const std::string &mode = {});
    std::string setMode(const std::string &mode, const json &ref) override;
    void onExited(int status, const std::string &screen);
    void onTyped(bool ok, const std::string &screen);
    void onSnapshot(const std::string &screen);
    bool confirmsDelivery() const override { return verify_; }

protected:
    void submit(const Prompt &prompt) override;
    void terminate();
    bool stopping_ = false; // `stop` asked for the end

private:
    void armEscapeCheck(bool forceIdle = false);
    void runEscapeChecks(Clock::time_point now);
    void pollTranscript();
    void scanSubagents(Clock::time_point now);
    void catchUp();
    void remember() const;
    void openScreenDialog();
    bool ours(const std::string &claudeSession, const std::string &source) const;
    void noteMode(const std::string &mode);
    void pressModeKey();
    void modeDone(const std::string &state, const std::string &error = {});
    void delivered();
    void notDelivered(const std::string &screen, bool retry);
    void unblock();

    Conn &wrapper_;
    bool canRestart_ = false;
    std::string claudeSession_;
    std::string transcript_;
    TranscriptTail tail_;
    Clock::time_point nextTail_ = Clock::now();
    // A slash command went in: it ends with its output in the transcript,
    // not with a Stop.
    bool slashPending_ = false;
    std::size_t slashMark_ = 0;
    std::string lastEvent_; // the last hook event, to drop one that comes twice
    // After an Esc in the terminal: where the transcript ended then, and
    // when to look whether Claude Code wrote an interrupt behind it.
    off_t escapeOffset_ = -1;
    std::vector<Clock::time_point> escapeChecks_;
    // The app asked to stop the turn: if no interrupt shows up in the
    // transcript, the turn is given up anyway and the queue dropped.
    bool forceIdle_ = false;
    // A prompt went in; UserPromptSubmit should confirm it by then. Claude
    // Code without a login answers "Not logged in" and fires no hook.
    std::optional<Clock::time_point> submitDeadline_;
    // Turns Claude Code started on its own, seen so far (see wakeTurns()).
    std::size_t wakeMark_ = 0;
    // The dialog the screen shows now (see onScreen), when a turn's one
    // counts, and the state it interrupted.
    std::string screenText_;
    std::optional<Clock::time_point> screenAt_;
    std::string screenFrom_ = "idle";
    Clock::time_point began_ = Clock::now();
    // Delivery under a wrapper that verifies (pvai 0.5.15 and later): the
    // prompt typed now, until UserPromptSubmit (or for a slash command the
    // wrapper's `typed`) confirms it; the wrapper's answer is due by
    // typedDeadline_. A prompt that did not go in is queued again in front
    // and the session blocked: waiting on a "screen" dialog showing the
    // screen, until the prompt box is free again (retryOnPrompt_: at once,
    // else only after the owner answered from the app).
    bool verify_ = false;
    std::optional<Prompt> inFlight_;
    std::optional<Clock::time_point> typedDeadline_;
    bool snapshotPending_ = false;
    bool blocked_ = false;
    bool retryOnPrompt_ = false;
    bool promptFree_ = false;
    bool sendUnchecked_ = false; // answer `send`: the next prompt without the check
    std::optional<Clock::time_point> retryAt_;
    // A mode change under way: Shift+Tab until the footer shows the mode,
    // at most once round the cycle.
    bool screenModes_ = false; // the wrapper reports the footer's mode (0.5.25)
    std::string modeTarget_, modeFrom_, modeSeen_;
    json modeRef_;
    int modePresses_ = 0;
    std::optional<Clock::time_point> modeDeadline_;
    // Subagents, from <transcript without .jsonl>/subagents/agent-<id>.meta.json:
    // by id, the meta file's mtime when last read and the protocol entry
    // (null once finished or given up).
    struct Subagent {
        std::filesystem::file_time_type mtime;
        json entry;
    };
    std::map<std::string, Subagent> subagents_;
    std::filesystem::file_time_type created_ = std::filesystem::file_time_type::clock::now();
    Clock::time_point nextSubagents_ = Clock::now();
};

// `pvai run` in the owner's terminal: theirs to end.
class TerminalClaudeSession : public ClaudeSession {
public:
    using ClaudeSession::ClaudeSession;
    bool detached() const override { return false; }
    std::string stop() override { return "not-detached"; }
};

// Started by the daemon (`pvai run --detached`), no terminal of its own.
class DetachedClaudeSession : public ClaudeSession {
public:
    using ClaudeSession::ClaudeSession;
    bool detached() const override { return true; }
    std::string stop() override;
};

class ClaudeBackend : public Backend {
public:
    using Backend::Backend;

    std::string name() const override { return "claude"; }
    std::string title() const override { return "Claude Code"; }
    json models() const override { return {"opus", "sonnet", "haiku"}; }
    // Sessions come from wrappers whatever the daemon's PATH says.
    bool available() const override { return true; }
    bool installedHere() const override { return !binary().empty(); }
    void refreshLogin(std::function<void(bool changed)> done) override;
    json modes() const override;
    static json claudeModes();

    void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) override;
    void start(const StartRequest &request, Started done) override;

    // A wrapper registered: its session, of the kind it says.
    std::unique_ptr<ClaudeSession> attach(Conn &wrapper, const json &registration);

    // Where Claude Code keeps the transcripts of `cwd`.
    static std::string projectDir(const std::string &cwd);

protected:
    std::string binary() const override;
};

} // namespace pvai
