// Follows a Claude Code session transcript (one JSON object per line, an
// internal format that changes between versions) and turns what the owner
// should see into the protocol's entries: prompt, text, tool, result,
// interrupted. Everything else in the file is skipped.
#pragma once

#include "common.h"

#include <sys/types.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace pvai {

// The one file a shell command shows, such as `sed -n 1,40p src/a.cpp`, so
// that an app can colour its output; empty when the command names none or
// several.
std::string commandFile(const std::string &command);

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

private:
    void take(const json &line, std::vector<json> &out);

    std::string path_;
    off_t offset_ = 0;
    bool first_ = true;
    std::string partial_;
    std::deque<json> recent_;
    std::map<std::string, std::string> toolNames_; // tool_use id -> name
    std::size_t localOutputs_ = 0;
    std::size_t wakeTurns_ = 0;
    bool wakePending_ = false; // a task notification, no answer yet
    json cache_;
};

} // namespace pvai
