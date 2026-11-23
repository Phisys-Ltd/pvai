// Sharing one session with a guest (docs/PROTOCOL.md, "Sharing a session"):
// the pieces that need no daemon: durations, the rights a guest has, and the
// record of which prompts a guest sent, so that the history can say so.
#pragma once

#include "common.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pvai {

// The share's default lifetime, and the longest one a request may name.
constexpr long long kShareDefaultSeconds = 7 * 24 * 3600;
constexpr long long kShareMaxSeconds = 3650ll * 24 * 3600;
// What every owner is told when a share is made.
extern const char *const kShareWarning;

// "90", "90s", "30m", "12h", "7d", "2w" as seconds; nullopt for anything else
// or a value below 1 s or above kShareMaxSeconds.
std::optional<long long> parseDuration(const std::string &text);

// The requests a guest may make, as `welcome.guest.rights` names them.
json guestRights(bool dialogs);

// A prompt's text as the guest typed it and the transcript shows it: Claude
// Code's paste frame and the voice mark gone, trimmed.
std::string normalisePrompt(const std::string &text);
// Unix milliseconds of an ISO 8601 stamp ("2026-10-28T19:20:28.623Z"), -1 if it is none.
long long isoMillis(const std::string &stamp);

// ~/.config/pvai/guest-prompts.json (0600): per conversation, the prompts
// that guests sent: a hash of the normalised text, when it was typed, who.
// A prompt entry of the conversation that matches one by text, and is not
// older than it, gets `from`. Every record belongs to one entry at most, the
// oldest record first for equal texts, and keeps that entry's id, so the
// answer for an entry never changes. At most kMaxPerConversation records.
class GuestPrompts {
public:
    static constexpr std::size_t kMaxPerConversation = 500;
    // A record matches an entry up to this long after it was typed (a prompt
    // may wait in the queue, for a usage limit for one).
    static constexpr long long kMatchWindowMs = 24ll * 3600 * 1000;

    explicit GuestPrompts(std::string path);
    // `typed` is the prompt's text, `withFiles` the text with the paths of
    // its files (what Claude Code and Kimi show), empty without files.
    void record(const std::string &conversation, const std::string &typed, const std::string &withFiles, long long timeMs,
                const std::string &from);
    // Sets entry["from"] when entry is a prompt of this conversation that a guest sent.
    void annotate(const std::string &conversation, json &entry);
    void forget(const std::string &conversation);
    bool dirty() const { return dirty_; }
    // Writes the file when it changed (temp file and rename); false when that failed.
    bool save();
    std::size_t size(const std::string &conversation) const;

private:
    struct Record {
        std::string hash, fileHash, from, entry;
        long long time = 0;
    };
    std::string path_;
    std::map<std::string, std::vector<Record>> records_;
    bool dirty_ = false;
};

} // namespace pvai
