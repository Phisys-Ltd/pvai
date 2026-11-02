// Text helpers shared by the backends, the screen reader and the pairing code.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace pvai {

using json = nlohmann::json;

// The longest text and tool result a transcript entry carries.
constexpr std::size_t kMaxText = 64 * 1024;
constexpr std::size_t kMaxResult = 4 * 1024;

// The largest index <= `limit` that does not sit inside a UTF-8 character.
// `limit` must be below text.size().
inline std::size_t utf8Boundary(std::string_view text, std::size_t limit)
{
    while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80)
        --limit;
    return limit;
}

// `text` without leading and trailing characters of `set`.
std::string trim(const std::string &text, const char *set = " \t\r\n");
// Cut to `limit` bytes at a character boundary, with … when cut.
std::string cut(std::string text, std::size_t limit);
// One line of at most `limit` bytes: control characters dropped, trimmed.
std::string oneLine(std::string_view text, std::size_t limit);
std::string str(const json &object, const char *key);
// Unix seconds of an ISO 8601 stamp ("2026-10-28T19:20:28.623Z"), 0 if it is none.
long long unixTime(const std::string &stamp);
// The first of `keys` that holds a string in `object`, if it is an object.
std::optional<std::string> firstString(const json &object, std::initializer_list<const char *> keys);
// The "question" of every entry of `holder["questions"]`, one per line.
std::string questionOf(const json &holder);

} // namespace pvai
