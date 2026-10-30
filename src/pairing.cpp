#include "pairing.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace pvai {

namespace {

bool isHex(const std::string &text)
{
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

std::string percentEncode(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'
            || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace

std::string cleanName(std::string_view name)
{
    std::string out;
    for (const char c : name)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f)
            out += c;
    if (out.size() > kDeviceNameMax) {
        std::size_t cut = kDeviceNameMax;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    const auto a = out.find_first_not_of(' ');
    if (a == std::string::npos)
        return "device";
    return out.substr(a, out.find_last_not_of(' ') - a + 1);
}

std::optional<PairRequest> parsePair(std::string_view line)
{
    if (line.size() > kPairLineMax)
        return std::nullopt;
    return parsePair(json::parse(line, nullptr, false));
}

std::optional<PairRequest> parsePair(const json &message)
{
    if (!message.is_object() || !message.contains("type") || message["type"] != "pair")
        return std::nullopt;
    if (!message.contains("name") || !message["name"].is_string() || !message.contains("proof")
        || !message["proof"].is_string())
        return std::nullopt;
    PairRequest request;
    request.proof = message["proof"].get<std::string>();
    if (request.proof.size() != 64 || !isHex(request.proof))
        return std::nullopt;
    request.name = cleanName(message["name"].get<std::string>());
    return request;
}

std::string base32(const unsigned char *data, std::size_t size)
{
    static const char *alphabet = "abcdefghijklmnopqrstuvwxyz234567";
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            out += alphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0)
        out += alphabet[(buffer << (5 - bits)) & 31];
    return out;
}

std::optional<std::string> base32Decode(std::string_view text)
{
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        int value;
        if (c >= 'a' && c <= 'z')
            value = c - 'a';
        else if (c >= '2' && c <= '7')
            value = c - '2' + 26;
        else
            return std::nullopt;
        buffer = (buffer << 5) | static_cast<unsigned>(value);
        bits += 5;
        if (bits >= 8) {
            out += static_cast<char>((buffer >> (bits - 8)) & 0xff);
            bits -= 8;
        }
    }
    if (bits >= 5 || (buffer & ((1u << bits) - 1)) != 0)
        return std::nullopt;
    return out;
}

std::string pairCode()
{
    unsigned char bytes[16];
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char *>(bytes), sizeof bytes))
        throw std::runtime_error("no /dev/urandom");
    return base32(bytes, sizeof bytes);
}

std::string pairLink(const std::string &host, int port, const std::string &fingerprint, const std::string &code,
                     const std::string &name)
{
    return "pvai://pair?host=" + percentEncode(host) + "&port=" + std::to_string(port) + "&fp=" + fingerprint
        + "&code=" + code + "&name=" + percentEncode(name);
}

bool sameSecret(const std::string &a, const std::string &b)
{
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string devicesPath()
{
    return configDir() + "/devices.json";
}

Devices::Devices(std::string path)
    : path_(std::move(path))
{
}

std::vector<Device> Devices::load() const
{
    std::vector<Device> out;
    std::ifstream in(path_);
    if (!in)
        return out;
    std::stringstream all;
    all << in.rdbuf();
    const json list = json::parse(all.str(), nullptr, false);
    if (!list.is_array())
        return out;
    for (const auto &entry : list) {
        if (!entry.is_object() || !entry.contains("fingerprint") || !entry["fingerprint"].is_string())
            continue;
        Device device;
        device.fingerprint = entry["fingerprint"].get<std::string>();
        device.name = entry.value("name", "");
        device.added = entry.value("added", 0LL);
        device.lastSeen = entry.value("lastSeen", 0LL);
        out.push_back(device);
    }
    return out;
}

bool Devices::save(const std::vector<Device> &devices) const
{
    json list = json::array();
    for (const auto &d : devices)
        list.push_back({{"fingerprint", d.fingerprint}, {"name", d.name}, {"added", d.added}, {"lastSeen", d.lastSeen}});
    const std::string temp = path_ + ".new";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const bool ok = writeAll(fd, list.dump(2) + "\n") && ::fsync(fd) == 0;
    ::close(fd);
    return ok && ::rename(temp.c_str(), path_.c_str()) == 0;
}

std::optional<Device> Devices::find(const std::string &fingerprint) const
{
    for (const auto &device : load())
        if (device.fingerprint == fingerprint)
            return device;
    return std::nullopt;
}

bool Devices::add(const Device &device)
{
    auto devices = load();
    std::erase_if(devices, [&](const Device &d) { return d.fingerprint == device.fingerprint; });
    devices.push_back(device);
    return save(devices);
}

std::vector<Device> Devices::remove(const std::string &nameOrFingerprint)
{
    auto devices = load();
    std::vector<Device> removed;
    std::erase_if(devices, [&](const Device &d) {
        const bool match = d.fingerprint == nameOrFingerprint || d.name == nameOrFingerprint;
        if (match)
            removed.push_back(d);
        return match;
    });
    if (!removed.empty())
        save(devices);
    return removed;
}

void Devices::seen(const std::string &fingerprint, long long when)
{
    auto devices = load();
    for (auto &d : devices)
        if (d.fingerprint == fingerprint)
            d.lastSeen = when;
    save(devices);
}

bool Lockout::blocked(const std::string &address, Clock::time_point now) const
{
    const auto it = entries_.find(address);
    return it != entries_.end() && now < it->second.until;
}

bool Lockout::fail(const std::string &address, Clock::time_point now)
{
    Entry &entry = entries_[address];
    entry.last = now;
    entry.failures.push_back(now);
    while (!entry.failures.empty() && now - entry.failures.front() > std::chrono::minutes(1))
        entry.failures.pop_front();
    if (static_cast<int>(entry.failures.size()) < kFailures)
        return false;
    entry.failures.clear();
    entry.until = now + entry.next;
    entry.next = std::min<std::chrono::seconds>(entry.next * 2, std::chrono::hours(1));
    return true;
}

void Lockout::prune(Clock::time_point now)
{
    std::erase_if(entries_, [&](const auto &item) {
        return now - item.second.last > std::chrono::hours(1) && now >= item.second.until;
    });
}

} // namespace pvai
