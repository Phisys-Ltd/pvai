#include "share.h"

#include "inbox.h"
#include "session.h"
#include "transcript.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pvai {

const char *const kShareWarning =
    "The guest works with your rights: its prompts run as your user, and the agent can run any command. "
    "Share only with people you would trust with your account.";

std::optional<long long> parseDuration(const std::string &text)
{
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0])))
        return std::nullopt;
    char *end = nullptr;
    const long long number = std::strtoll(text.c_str(), &end, 10);
    long long unit = 1;
    if (*end) {
        if (end[1])
            return std::nullopt;
        switch (*end) {
        case 's': unit = 1; break;
        case 'm': unit = 60; break;
        case 'h': unit = 3600; break;
        case 'd': unit = 86400; break;
        case 'w': unit = 7 * 86400; break;
        default: return std::nullopt;
        }
    }
    if (number < 1 || number > kShareMaxSeconds / unit)
        return std::nullopt;
    return number * unit;
}

json guestRights(bool dialogs)
{
    json rights = {"sessions", "subscribe", "history", "prompt", "interrupt", "upload", "fetch"};
    if (dialogs)
        rights.push_back("answer");
    return rights;
}

std::string normalisePrompt(const std::string &text)
{
    std::string out = trim(stripPasteFrame(text));
    const std::size_t mark = std::strlen(kMicMark);
    if (out.compare(0, mark, kMicMark) == 0)
        out = trim(out.substr(mark));
    return out;
}

long long isoMillis(const std::string &stamp)
{
    const long long seconds = unixTime(stamp);
    if (seconds <= 0)
        return -1;
    long long millis = 0;
    const auto dot = stamp.find('.');
    if (dot != std::string::npos) {
        int digits = 0;
        for (std::size_t i = dot + 1; i < stamp.size() && digits < 3 && std::isdigit(static_cast<unsigned char>(stamp[i])); ++i, ++digits)
            millis = millis * 10 + (stamp[i] - '0');
        for (; digits < 3; ++digits)
            millis *= 10;
    }
    return seconds * 1000 + millis;
}

namespace {

std::string textHash(const std::string &normalised)
{
    return bytesSha256(normalised).substr(0, 16);
}

} // namespace

GuestPrompts::GuestPrompts(std::string path)
    : path_(std::move(path))
{
    std::ifstream in(path_);
    if (!in)
        return;
    std::stringstream all;
    all << in.rdbuf();
    const json file = json::parse(all.str(), nullptr, false);
    if (!file.is_object())
        return;
    for (const auto &[conversation, list] : file.items()) {
        if (!list.is_array())
            continue;
        std::vector<Record> records;
        for (const auto &entry : list) {
            if (!entry.is_object() || str(entry, "hash").empty())
                continue;
            Record r;
            r.hash = str(entry, "hash");
            r.fileHash = str(entry, "fileHash");
            r.from = str(entry, "from");
            r.entry = str(entry, "entry");
            r.time = integer(entry, "time");
            records.push_back(std::move(r));
        }
        if (records.size() > kMaxPerConversation)
            records.erase(records.begin(), records.end() - static_cast<long>(kMaxPerConversation));
        if (!records.empty())
            records_[conversation] = std::move(records);
    }
}

void GuestPrompts::record(const std::string &conversation, const std::string &typed, const std::string &withFiles, long long timeMs,
                          const std::string &from)
{
    if (conversation.empty() || from.empty())
        return;
    Record r;
    const std::string text = normalisePrompt(typed), full = normalisePrompt(withFiles);
    if (!text.empty())
        r.hash = textHash(text);
    if (!full.empty() && full != text)
        r.fileHash = textHash(full);
    if (r.hash.empty() && r.fileHash.empty())
        return;
    r.time = timeMs;
    r.from = from;
    auto &list = records_[conversation];
    list.push_back(std::move(r));
    if (list.size() > kMaxPerConversation)
        list.erase(list.begin(), list.end() - static_cast<long>(kMaxPerConversation));
    dirty_ = true;
}

void GuestPrompts::annotate(const std::string &conversation, json &entry)
{
    if (!entry.is_object() || str(entry, "kind") != "prompt")
        return;
    const auto found = records_.find(conversation);
    if (found == records_.end())
        return;
    const std::string id = str(entry, "id");
    auto &list = found->second;
    if (!id.empty())
        for (const Record &r : list)
            if (r.entry == id) {
                entry["from"] = r.from;
                return;
            }
    const std::string text = normalisePrompt(str(entry, "text"));
    if (text.empty())
        return;
    const std::string hash = textHash(text);
    const long long at = isoMillis(str(entry, "time"));
    for (Record &r : list) {
        if (!r.entry.empty() || (r.hash != hash && r.fileHash != hash))
            continue;
        if (at >= 0 && (at < r.time || at - r.time > kMatchWindowMs))
            continue; // not this one: an earlier prompt of the owner's with the same words, or far too late
        if (!id.empty()) {
            r.entry = id;
            dirty_ = true;
        }
        entry["from"] = r.from;
        return;
    }
}

void GuestPrompts::forget(const std::string &conversation)
{
    if (records_.erase(conversation))
        dirty_ = true;
}

std::size_t GuestPrompts::size(const std::string &conversation) const
{
    const auto it = records_.find(conversation);
    return it == records_.end() ? 0 : it->second.size();
}

bool GuestPrompts::save()
{
    if (!dirty_)
        return true;
    json file = json::object();
    for (const auto &[conversation, list] : records_) {
        json out = json::array();
        for (const Record &r : list) {
            json item = {{"hash", r.hash}, {"time", r.time}, {"from", r.from}};
            if (!r.fileHash.empty())
                item["fileHash"] = r.fileHash;
            if (!r.entry.empty())
                item["entry"] = r.entry;
            out.push_back(std::move(item));
        }
        file[conversation] = std::move(out);
    }
    AtomicOptions options;
    options.mode = 0600;
    options.sync = false;
    options.chmod = 0600;
    const bool ok = writeAtomically(path_, file.dump(1) + "\n", options) == Written::Ok;
    if (ok)
        dirty_ = false;
    return ok;
}

} // namespace pvai
