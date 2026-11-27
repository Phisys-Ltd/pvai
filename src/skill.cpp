#include "skill.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pvai {

namespace {

// The frontmatter (between the first two `---` lines) holds `managed-by: pvai`.
bool isManaged(const std::string &text)
{
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.substr(0, 3) != "---")
        return false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "---")
            return false;
        if (line == "managed-by: pvai")
            return true;
    }
    return false;
}

std::string installOne(const std::string &dir, const std::string &content)
{
    namespace fs = std::filesystem;
    const std::string path = dir + "/SKILL.md";
    struct stat st;
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode))
            return "skill " + path + ": left alone (symlink or not a regular file)";
        std::ifstream in(path, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string old = buf.str();
        if (!isManaged(old))
            return "skill " + path + ": left alone (user file)";
        if (old == content)
            return "skill " + path + ": up to date";
    } else if (errno != ENOENT) {
        return "skill " + path + ": not installed: " + std::strerror(errno);
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return "skill " + path + ": not installed: " + ec.message();
    const std::string tmp = dir + "/.SKILL.md.pvai-" + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.flush();
        if (!out) {
            fs::remove(tmp, ec);
            return "skill " + path + ": not installed: cannot write";
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        fs::remove(tmp, ec);
        return "skill " + path + ": not installed: " + std::strerror(saved);
    }
    return "skill " + path + ": written";
}

} // namespace

std::vector<std::string> installSkills(const std::string &home)
{
    std::vector<std::string> lines;
    for (const char *rel : {"/.claude/skills/pvai", "/.agents/skills/pvai"}) {
        try {
            lines.push_back(installOne(home + rel, kSkillContent));
        } catch (const std::exception &e) {
            lines.push_back(std::string("skill ") + home + rel + ": not installed: " + e.what());
        }
    }
    return lines;
}

} // namespace pvai
