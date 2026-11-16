// The inbox: files the app uploaded for a session (docs/PROTOCOL.md, "Files").
// ~/.cache/pvai/inbox/SESSIONID/, directories 0700, files 0600. Pure file
// handling; the daemon (daemon.cpp) owns the connections that feed it.
#pragma once

#include <openssl/evp.h>

#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace pvai {

constexpr unsigned long long kMaxUploadFile = 50ull * 1024 * 1024;     // one file
constexpr unsigned long long kMaxUploadSession = 100ull * 1024 * 1024; // one session's inbox
constexpr std::size_t kMaxUploadName = 120;                            // bytes of a stored name, without the stamp
constexpr std::size_t kMaxPromptFiles = 32;                            // `files` of one prompt
constexpr auto kInboxLife = std::chrono::hours(24);

std::string inboxRoot();                          // ~/.cache/pvai/inbox, created, canonical (realpath)
std::string inboxDir(const std::string &session); // empty for an id that is no path component

// The name as it is stored, without the timestamp: basename only, control
// characters and leading dots dropped, ".." made ".", at most
// kMaxUploadName bytes (the extension kept). Empty when nothing is left.
std::string sanitizeUploadName(const std::string &name);

// The SHA-256 of a file as lowercase hex; empty when it cannot be read.
std::string fileSha256(const std::string &path);

// `path` as a file of the session's inbox: the real path of a regular file
// inside the session's inbox directory; empty for anything else.
std::string inboxFile(const std::string &session, const std::string &path);

// Removes the session's inbox directory.
void removeInbox(const std::string &session);
// Files older than `maxAge` and empty session directories go; with `live`,
// the directories of sessions that are not in it too.
void cleanInbox(const std::set<std::string> *live, std::chrono::seconds maxAge);

// One upload: bytes go to NAME.part as they come, the size and the SHA-256
// are checked at the end and the file takes its name by one rename. Whatever
// is left of a failed upload is removed (also by the destructor).
class InboxUpload {
public:
    InboxUpload(std::string session, std::string name, unsigned long long size, std::string sha256);
    ~InboxUpload();
    InboxUpload(const InboxUpload &) = delete;
    InboxUpload &operator=(const InboxUpload &) = delete;

    // The reason to reject ("bad-name", "quota", "write-failed") or empty.
    std::string open();
    bool write(const char *data, std::size_t size); // false: the disk refused
    unsigned long long received() const { return received_; }
    // "checksum", "quota", "write-failed" or empty; then path() and name() say where it went.
    std::string finish();
    void abort();
    const std::string &path() const { return path_; }
    const std::string &name() const { return stored_; }

private:
    std::string session_, name_, sha_, dir_, part_, path_, stored_;
    unsigned long long size_, received_ = 0;
    int fd_ = -1;
    EVP_MD_CTX *md_ = nullptr;
};

// ---- The outbox: files an agent offered with a proposal ----
// ~/.cache/pvai/outbox/PROPOSALID/NN-NAME, directories 0700, files 0600
// (docs/PROTOCOL.md, "Agent messages"). The proposals live in memory only, so
// the daemon empties the outbox at its start.

constexpr std::size_t kMaxProposalFiles = 32;                            // files of one proposal
constexpr unsigned long long kMaxProposalBytes = 100ull * 1024 * 1024;   // one proposal's files together
constexpr unsigned long long kMaxOutboxBytes = 200ull * 1024 * 1024;     // all open proposals' files together

std::string outboxRoot(); // ~/.cache/pvai/outbox, created, canonical
std::string outboxDir(const std::string &proposal); // empty for an id that is no path component
void removeOutbox(const std::string &proposal);
void clearOutbox();                                    // every proposal directory
void cleanOutbox(const std::set<std::string> &live);  // the directories of proposals that are not in `live`

// One copied file.
struct OutboxFile {
    std::string name;   // the sanitised base name, without the NN- prefix
    unsigned long long size = 0;
    std::string sha256;
    std::string path;   // the copy
};

// Whether `path` is an absolute path to a regular file (links followed) that
// can be read: its size in `size`, or "bad-file".
std::string checkOutboxSource(const std::string &path, unsigned long long &size);
// Copies the source (the INDEX-th file, from 1) into the proposal's
// directory in chunks and hashes it on the way. "bad-file" when it cannot be
// read or written, "too-large" when it grew over `maxFile` meanwhile.
std::string copyToOutbox(const std::string &proposal, std::size_t index, const std::string &source,
                         unsigned long long maxFile, OutboxFile &out);

} // namespace pvai
