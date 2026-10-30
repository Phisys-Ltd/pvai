// Claude Code: each session is a `pvai run` wrapper around a claude process
// in a PTY. Prompts and answers to dialogs are typed into it; the hooks,
// the status line and the transcript report back.
#pragma once

#include "session.h"
#include "transcript.h"

#include <sys/types.h>

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

protected:
    void submit(const Prompt &prompt) override;
    void terminate();

private:
    void armEscapeCheck(bool forceIdle = false);
    void runEscapeChecks(Clock::time_point now);
    void pollTranscript();

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
    bool available() const override { return true; }

    void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) override;
    void start(const StartRequest &request, Started done) override;

    // A wrapper registered: its session, of the kind it says.
    std::unique_ptr<ClaudeSession> attach(Conn &wrapper, const json &registration);

    // Where Claude Code keeps the transcripts of `cwd`.
    static std::string projectDir(const std::string &cwd);

protected:
    std::string binary() const override { return findBinary("claude"); }
};

} // namespace pvai
