#include "text.h"

#include <cstdio>
#include <ctime>

namespace pvai {

std::string trim(const std::string &text, const char *set)
{
    const auto a = text.find_first_not_of(set);
    if (a == std::string::npos)
        return {};
    const auto b = text.find_last_not_of(set);
    return text.substr(a, b - a + 1);
}

std::string cut(std::string text, std::size_t limit)
{
    if (text.size() <= limit)
        return text;
    text.resize(utf8Boundary(text, limit));
    return text + "\xE2\x80\xA6"; // …
}

std::string oneLine(std::string_view text, std::size_t limit)
{
    std::string out;
    for (const char c : text)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f)
            out += c;
    if (out.size() > limit)
        out.resize(utf8Boundary(out, limit));
    return trim(out);
}

std::string str(const json &object, const char *key)
{
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>()
                                                                                 : std::string();
}

long long unixTime(const std::string &stamp)
{
    std::tm tm{};
    if (std::sscanf(stamp.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<long long>(timegm(&tm));
}

std::optional<std::string> firstString(const json &object, std::initializer_list<const char *> keys)
{
    if (!object.is_object())
        return std::nullopt;
    for (const char *key : keys)
        if (object.contains(key) && object[key].is_string())
            return object[key].get<std::string>();
    return std::nullopt;
}

std::string questionOf(const json &holder)
{
    std::string text;
    if (holder.is_object() && holder.contains("questions") && holder["questions"].is_array())
        for (const auto &question : holder["questions"])
            if (const std::string q = str(question, "question"); !q.empty())
                text += (text.empty() ? "" : "\n") + q;
    return text;
}

} // namespace pvai
