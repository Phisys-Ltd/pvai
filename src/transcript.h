// Follows a Claude Code session transcript (one JSON object per line, an
// internal format that changes between versions) and turns what the owner
// should see into the protocol's entries: prompt, text, tool, result,
// interrupted, limited, resumed. Everything else in the file is skipped.
#pragma once

#include "common.h"

#include <sys/types.h>

#include <deque>
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pvai {

// The one file a shell command shows, such as `sed -n 1,40p src/a.cpp`, so
// that an app can colour its output; empty when the command names none or
// several.
std::string commandFile(const std::string &command);

// A prompt nobody typed: a user line Claude Code wrote itself (reminders,
// task notifications, command echoes and output, ! shell input and output),
// by its known leading tag, or a message another Claude session or pvai
// handed in (isAgentMessage). An owner's prompt that starts with another tag
// is none. The one list for the voice state, the titles and the history.
bool isMachinePrompt(const std::string &text);
// A message from another session or a subagent's hand-back ("[Message p1
// from ...", "Another Claude session sent a message"): a machine prompt that
// the history still shows, as it says what came in.
bool isAgentMessage(const std::string &text);
// `text` without Claude Code's paste frame (<pasted_content id="..."> ...
// </pasted_content id="...">, also around only a part of the text) and
// without the whitespace around it. Unchanged when there is no frame.
std::string stripPasteFrame(const std::string &text);
// A line Claude Code keeps for itself: meta lines, and the summary a
// compaction leaves as a user line (isCompactSummary).
bool isBookkeeping(const json &line);

// Whether a turn runs now, from the end of a transcript: a prompt or an
// answer of Claude's after the last turn end (Claude Code writes a
// turn_duration line when a turn ends; an interrupt ends one too). False
// when the file says nothing either way. runAt: the unix time of the last
// line that said running (0 without a timestamp).
bool turnRunning(const std::string &path, long long *runAt = nullptr);

// Whether the owner's last own prompt in a transcript was spoken (starts
// with the 🎙 mark): not a sidechain, meta, slash, shell or machine line (a
// message from another session, a task notification). False without one.
bool lastPromptSpoken(const std::string &path);

class TranscriptTail {
public:
    static constexpr std::size_t kKeep = 500;           // entries kept for `history`
    static constexpr off_t kFirstRead = 8 * 1024 * 1024; // a long transcript is read from its last 8 MiB

    // Starts over on another file (a new session, or /clear).
    void follow(const std::string &path);
    const std::string &path() const { return path_; }
    // Reads what was appended and returns the new entries.
    std::vector<json> poll();
    const std::deque<json> &recent() const { return recent_; }
    // How many local slash commands (/compact, /context, /model...) have
    // finished: each ends with an output entry, even an empty one. No hook
    // fires for them.
    std::size_t localOutputs() const { return localOutputs_; }
    // Compactions (compact_boundary lines, manual or auto) seen so far.
    std::size_t compactions() const { return compactions_; }
    // Output lines of shell commands (!), one each, also when empty.
    // The text of the last local command's output (ANSI stripped, trimmed);
    // empty when it had none.
    const std::string &lastLocalOutput() const { return lastLocalOutput_; }
    std::size_t shellOutputs() const { return shellOutputs_; }
    // How many turns Claude Code started on its own: a task notification
    // (a background task or agent finished) followed by an answer of
    // Claude's, with no prompt of the owner's in between. Such a turn has
    // no UserPromptSubmit; a notification that only waits for the next
    // prompt starts none.
    std::size_t wakeTurns() const { return wakeTurns_; }
    // A hook ended or started a turn: a task notification read so far
    // belongs to it, even when Claude's answer to it is not in the file yet.
    void settleWakes() { wakePending_ = false; }
    // How long the prompt cache stays warm: {"ttl": seconds, "at": Unix time
    // of the last API request}, from the last answer of Claude's; null until
    // a request wrote to the cache (a read alone keeps the TTL it was
    // written with).
    const json &cache() const { return cache_; }
    // turnRunning() as far as the first read saw it; nullopt when the window
    // held no turn end and the file may have one further back.
    std::optional<bool> turnRunningSeen() const;

    // The usage limit the session waits on ({"resetsAt", "type", "text"}, the
    // first two null when the line has none), or null once Claude Code went
    // on (a dequeue, or an answer that is no API error). The counters tell
    // a caller that polls less often than lines come in that something
    // happened. A `resumed` entry follows a dequeue only while limited.
    const json &limited() const { return limited_; }
    // The timestamp of the line that set limited() (its ISO text; empty when none).
    const std::string &limitedAt() const { return limitedAt_; }
    std::size_t limitHits() const { return limitHits_; }
    std::size_t limitClears() const { return limitClears_; }
    // Login problems and working replies in the file, as Claude Code writes
    // them: an assistant line with `isApiErrorMessage` and an authentication
    // failure (`error` "authentication_failed", `apiErrorStatus` 401) counts in
    // authFailures(); any other assistant line of the conversation itself is a
    // reply. Only these structured fields count, never the words of a text, so
    // an answer that quotes the error is none.
    std::size_t authFailures() const { return authFailures_; }
    const std::string &authFailedAt() const { return authFailedAt_; }
    std::size_t replies() const { return replies_; }
    // How many turn_duration lines (a turn ended in the file) were read.
    std::size_t turnEndCount() const { return turnEndCount_; }
    // The texts Claude Code holds in its own queue: a prompt typed while a
    // turn runs is written as a `queue-operation` enqueue with its text and
    // leaves the queue with a dequeue (taken up, oldest first) or a remove.
    // One typed at a free prompt is in and out within milliseconds. Task
    // notifications queue the same way and are in the list too.
    const std::deque<std::string> &queued() const { return queued_; }
    // Texts whose `remove` said `absorbed_mid_turn`: Claude Code put the
    // prompt into the running turn itself (a `queued_command` attachment),
    // with no UserPromptSubmit. Taken (and cleared) by the caller.
    std::vector<std::string> takeAbsorbed() { return std::exchange(absorbed_, {}); }
    // Task ids whose task notification said killed, failed or completed, by
    // id, with the moment the line was read (file clock, to compare with
    // mtimes). Claude Code writes no `finished` for an agent stopped with
    // TaskStop; the notification is all that says it ended.
    const std::map<std::string, std::filesystem::file_time_type> &endedTasks() const { return endedTasks_; }

private:
    void resetState();
    void clearLimited();
    void take(const json &line, std::vector<json> &out);

    std::string path_;
    off_t offset_ = 0;
    bool first_ = true;
    std::string partial_;
    std::deque<json> recent_;
    std::map<std::string, std::string> toolNames_; // tool_use id -> name
    std::size_t localOutputs_ = 0;
    std::size_t compactions_ = 0;
    std::string lastLocalOutput_;
    std::size_t shellOutputs_ = 0;
    std::size_t wakeTurns_ = 0;
    bool wakePending_ = false; // a task notification, no answer yet
    json cache_;
    json limited_;
    std::string limitedAt_;
    std::size_t limitHits_ = 0, limitClears_ = 0, turnEndCount_ = 0;
    std::size_t authFailures_ = 0, replies_ = 0;
    std::string authFailedAt_;
    bool turnRunning_ = false, turnEnds_ = false, windowed_ = false;
    long long noteAt_ = 0; // an unanswered task notification, written then
    std::deque<std::string> queued_;
    std::vector<std::string> absorbed_;
    std::map<std::string, std::filesystem::file_time_type> endedTasks_;
};

} // namespace pvai
