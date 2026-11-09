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

// The oldest `pvai run` that answers `answer`, `clearInput`, `mode`, `model`
// and `restart` as the daemon expects (0.5.44: the `key` class of `input`,
// the screen after a prompt that was not typed). Typing works with any.
constexpr const char *kMinWrapperVersion = "0.5.44";

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
    // The wrapper keeps the owner's label for its next register.
    void labelPushed(const std::string &label) override { wrapper_.send({{"type", "label"}, {"label", label}}); }
    // At the prompt (idle, or blocked by what the box holds), under a
    // wrapper that can (pvai 0.5.30 and later).
    bool clearable() const override { return canClear_ && (state_ == "idle" || (state_ == "waiting" && blocked_)); }
    std::string clear() override;
    json history(int limit) override;
    std::vector<Prompt> unsent() const override;
    std::string conversationId() const override { return claudeSession_; }
    bool wrapperTooOld() const override;
    const std::string &wrapperVersion() const { return wrapperVersion_; }
    // A wrapper that reports no version at all (before 0.5.52): no /model through it.
    bool wrapperUnversioned() const override { return wrapperVersion_.empty(); }
    void tick(Clock::time_point now) override;
    std::optional<Clock::time_point> deadline() const override;

    // From the hook, the status line and the wrapper.
    void onHook(const json &event);
    void onStatus(const json &status);
    void onInput(bool escape, const std::string &key = {});
    void onDraft(bool dirty);
    void onDialog(const std::string &kind);
    void onScreen(const std::string &text, bool prompt, const std::string &mode = {});
    std::string setMode(const std::string &mode, const json &ref) override;
    std::string setModel(const std::string &model, const json &ref) override;
    void onExited(int status, const std::string &screen);
    void onTyped(bool ok, const std::string &screen, const std::string &how = "");
    void onSubmitted(bool ok, const std::string &why = "");
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
    void checkClaudeQueue();
    bool canSubmit() const override { return !(inFlight_ && queuedInClaude_); }
    void scanSubagents(Clock::time_point now);
    void catchUp();
    void syncMarks();
    static bool limitAhead(const json &limited);
    void drain(); // reads the transcript and hands the new entries to the subscribers
    void remember() const;
    void claimKept();
    void openScreenDialog();
    bool ours(const std::string &claudeSession, const std::string &source) const;
    void noteMode(const std::string &mode);
    void pressModeKey();
    void modeDone(const std::string &state, const std::string &error = {});
    void delivered();
    void dropQueued(const std::string &text);
    void notDelivered(const std::string &screen, bool retry);
    void lost(const Prompt &prompt, const std::string &why);
    void unblock();
    void retryIfBlocked();
    void endSlash();

    Conn &wrapper_;
    bool canRestart_ = false;
    std::string claudeSession_;
    std::string wrapperVersion_; // `register.version`, empty before 0.5.52
    std::string transcript_;
    TranscriptTail tail_;
    Clock::time_point nextTail_ = Clock::now();
    // A slash command went in: it ends with its output in the transcript,
    // not with a Stop.
    bool slashPending_ = false;
    bool slashModel_ = false; // the pending slash command is the one setModel queued
    std::string modelTarget_;
    json modelRef_;
    std::optional<Clock::time_point> modelAt_; // the /model in Claude Code is due its output by then
    void modelDone(const std::string &state, const std::string &error = {});
    void markSlash(const Prompt &prompt);
    bool typedUnchecked_ = false; // the wrapper answers `typed` also for a prompt sent anyway (0.5.71)
    bool uncheckedInFlight_ = false; // the prompt in flight was sent anyway
    bool submitCheck_ = false; // the wrapper checks that Return took (`submitCheck`, 0.5.63)
    std::size_t slashMark_ = 0;
    // A pending /compact also ends with any compaction in the transcript.
    bool slashCompact_ = false;
    std::size_t compactMark_ = 0;
    std::string slashText_;
    bool slashShell_ = false; // the pending command is a shell command (!): its output ends it
    std::size_t localDone() const { return slashShell_ ? tail_.shellOutputs() : tail_.localOutputs(); }
    std::string lastEvent_; // the last hook event's key, to drop one that comes twice
    Clock::time_point lastEventAt_{};
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
    long long typedAt_ = 0; // unix seconds when the last prompt was typed
    // The prompt in flight stands in Claude Code's own queue (the transcript
    // has its enqueue and no dequeue): it was typed while a turn still ran,
    // the box emptied, and no UserPromptSubmit comes until Claude Code takes
    // the queue up. It stays in flight, nothing is typed after it, and it is
    // reported when it is not taken up in kQueuedWait after the turn's end.
    bool queuedInClaude_ = false;
    std::optional<Clock::time_point> queuedUntil_;
    static constexpr std::chrono::seconds kQueuedWait{20};
    // Turns Claude Code started on its own, seen so far (see wakeTurns()).
    std::size_t wakeMark_ = 0;
    // The usage limit the session waits on (see Session.limited in the
    // protocol), and how much of the tail's limit and turn-end counts has
    // been reacted to.
    json limited_;
    std::size_t hitMark_ = 0, clearMark_ = 0, endMark_ = 0;
    // A turn ended in the transcript while busy: when to look whether the
    // Stop hook came (it does not for every end).
    std::optional<Clock::time_point> stopCheck_;
    static constexpr std::chrono::seconds kStopWait{3};
    static constexpr std::chrono::seconds kLimitGrace{180}; // past the reset, a limit is given up
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
    bool canClear_ = false;
    bool canPace_ = false; // the wrapper takes `paced` keys (0.5.33)
    // An AskUserQuestion dialog: a choice, the next question, a typed answer.
    std::string answerQuestion(const std::string &choice);
    void questionAnswered(bool advance);
    std::optional<Prompt> inFlight_;
    std::optional<Clock::time_point> typedDeadline_;
    bool snapshotPending_ = false;
    bool blocked_ = false;
    bool retryOnPrompt_ = false;
    std::optional<Clock::time_point> typedPauseUntil_; // no retry typing before this (failed typed check)
    std::string failedScreen_;
    int sameFails_ = 0;
    bool typedPause() const;
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
    bool quiet() const;
    bool hasRunningSubagent() const;
    std::map<std::string, Subagent> subagents_;
    std::filesystem::file_time_type created_ = std::filesystem::file_time_type::clock::now();
    Clock::time_point nextSubagents_ = Clock::now();
    Clock::time_point lastTail_ = Clock::now(), lastSubagents_ = Clock::now();
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
    std::string restart() override;
    std::optional<StartRequest> replacement() const override { return replacement_; }

private:
    std::optional<StartRequest> replacement_;
};

class ClaudeBackend : public Backend {
public:
    using Backend::Backend;

    std::string name() const override { return "claude"; }
    std::string title() const override { return "Claude Code"; }
    json models() const override { return {"opus", "sonnet", "haiku", "fable"}; }
    // Sessions come from wrappers whatever the daemon's PATH says.
    bool available() const override { return true; }
    bool installedHere() const override { return !binary().empty(); }
    void refreshLogin(std::function<void(bool changed)> done) override;
    json describe() const override;
    // The fallback token (PROTOCOL.md, "Claude token login"): `set-token`
    // stored one; `done` runs when what that changes is known. Logout drops
    // the token and the decision.
    void tokenStored(std::function<void()> done);
    void dropToken();
    // "token" (the fallback is active, or the environment has a token),
    // "account", or "none" (the account is not logged in and there is no token).
    std::string loginMethod() const;
    json modes() const override;
    static json claudeModes();

    void conversations(const std::string &cwd, std::function<void(std::vector<Conversation>)> done) override;
    void forget(const std::string &cwd, const std::string &id, std::function<void(const std::string &)> done) override;
    void start(const StartRequest &request, Started done) override;

    // A wrapper registered: its session, of the kind it says.
    std::unique_ptr<ClaudeSession> attach(Conn &wrapper, const json &registration);

    // Where Claude Code keeps the transcripts of `cwd`.
    static std::string projectDir(const std::string &cwd);

protected:
    std::string binary() const override;

private:
    // `claude auth status --json` with the stored token in its environment
    // (never on the command line); sets loggedIn_.
    void statusWithToken(std::function<void()> done);

    // The cached decision: the account login as the last check without the
    // token saw it (empty: not known), and whether the fallback is active.
    std::optional<bool> account_;
    bool fallback_ = false;

    // Conversation titles by transcript path, read once per (mtime, size).
    struct Title {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
        std::string title;
        bool known = false;
    };
    std::map<std::string, Title> titles_;
};

} // namespace pvai
