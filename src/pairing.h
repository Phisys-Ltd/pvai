// Direct access without the network in it: what a pairing client may send
// before it is known, the paired devices, and the lockout of addresses that
// keep failing. Kept apart so it can be fuzzed and tested on its own.
#pragma once

#include "common.h"

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pvai {

constexpr std::size_t kPairLineMax = 4096;
constexpr std::size_t kDeviceNameMax = 64;

struct PairRequest {
    std::string name;  // cleaned: no control characters, at most kDeviceNameMax bytes
    std::string proof; // 64 lowercase hex digits
};

// The one line a client in a pairing window may send; nullopt for anything
// else (too long, not JSON, wrong type, missing or malformed fields).
std::optional<PairRequest> parsePair(std::string_view line);
std::optional<PairRequest> parsePair(const json &message);

// A device name as shown and stored: control characters dropped, cut to
// kDeviceNameMax bytes at a character boundary, "device" when empty.
std::string cleanName(std::string_view name);

// RFC 4648 base32, lowercase, without padding.
std::string base32(const unsigned char *data, std::size_t size);
// Its inverse; nullopt for a character outside the alphabet or leftover
// bits that are not zero.
std::optional<std::string> base32Decode(std::string_view text);

// 128 random bits in base32: the pairing code.
std::string pairCode();

// pvai://pair?host=...&port=...&fp=...&code=...&name=...
std::string pairLink(const std::string &host, int port, const std::string &fingerprint, const std::string &code,
                     const std::string &name);

// pvai://share?host=...&port=...&fp=...&code=...&name=...&session=...
// (since 0.7.0): the pair link of one invited guest, bound to one session.
std::string shareLink(const std::string &host, int port, const std::string &fingerprint, const std::string &code,
                      const std::string &name, const std::string &session);

// Equal without telling where they differ.
bool sameSecret(const std::string &a, const std::string &b);

// A guest's share (since 0.7.0, docs/PROTOCOL.md "Sharing a session"): the one
// session its device may work with. `conversation` is the assistant's own
// conversation id as captured when the share was granted or last confirmed.
struct Guest {
    std::string assistant;
    std::string session;
    std::string conversation;
    long long expires = 0; // Unix time, 0: never
    bool dialogs = false;
    std::string state = "active"; // active | scope-moved | ended
};

struct Device {
    std::string fingerprint;
    std::string name;
    long long added = 0;    // Unix time
    long long lastSeen = 0; // Unix time, 0 before the first login
    std::optional<Guest> guest; // set: a guest's device, no owner device
};

// ~/.config/pvai/devices.json, read on every use so a `pvai revoke` from
// anywhere counts at once.
class Devices {
public:
    explicit Devices(std::string path);
    std::vector<Device> load() const;
    bool save(const std::vector<Device> &devices) const;
    std::optional<Device> find(const std::string &fingerprint) const;
    bool add(const Device &device);
    // By name or fingerprint; returns the removed devices.
    // `saved` (if given) tells whether the file was written; false: nothing is revoked.
    std::vector<Device> remove(const std::string &nameOrFingerprint, bool *saved = nullptr);
    void seen(const std::string &fingerprint, long long when);
    // Replaces the guest part of a device; false when there is no such
    // device or the file could not be written.
    bool setGuest(const std::string &fingerprint, const Guest &guest);
    // Only the guests, by name or fingerprint (an owner device with that name
    // stays); returns the removed ones; `saved` as in remove().
    std::vector<Device> removeGuests(const std::string &nameOrFingerprint, bool *saved = nullptr);

private:
    std::string path_;
};

std::string devicesPath();

// Failed handshakes and proofs per source address: after kFailures within
// a minute the address is dropped for 60 s, doubling up to an hour.
class Lockout {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kFailures = 5;

    bool blocked(const std::string &address, Clock::time_point now) const;
    // Counts a failure; true when the address is now blocked.
    bool fail(const std::string &address, Clock::time_point now);
    // Forgets addresses that have been quiet for an hour.
    void prune(Clock::time_point now);

private:
    struct Entry {
        std::deque<Clock::time_point> failures;
        Clock::time_point until{};
        std::chrono::seconds next{60};
        Clock::time_point last{};
    };
    std::map<std::string, Entry> entries_;
};

} // namespace pvai
