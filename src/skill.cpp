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

SkillResult result(const std::string &path, const char *status, const std::string &detail = std::string())
{
    return {path, status, detail};
}

// Looks at the target; returns true with `out` set when the file must be
// (re)written (`missing` tells new from outdated), false when it is final.
bool inspect(const std::string &path, const std::string &content, SkillResult &out, bool &missing)
{
    struct stat st;
    missing = false;
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            out = result(path, "symlink");
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string old = buf.str();
        if (!isManaged(old)) {
            out = result(path, "user file");
            return false;
        }
        if (old == content) {
            out = result(path, "up to date");
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        out = result(path, "error", std::strerror(errno));
        return false;
    }
    missing = true;
    return true;
}

SkillResult installOne(const std::string &dir, const std::string &content)
{
    namespace fs = std::filesystem;
    const std::string path = dir + "/SKILL.md";
    SkillResult out;
    bool missing = false;
    if (!inspect(path, content, out, missing))
        return out;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return result(path, "error", ec.message());
    const std::string tmp = dir + "/.SKILL.md.pvai-" + std::to_string(::getpid());
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << content;
        o.flush();
        if (!o) {
            fs::remove(tmp, ec);
            return result(path, "error", "cannot write");
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        fs::remove(tmp, ec);
        return result(path, "error", std::strerror(saved));
    }
    return result(path, "written");
}

const char *const kTargets[] = {"/.claude/skills/pvai", "/.agents/skills/pvai"};

} // namespace

const char *const kSkillSummary =
    "pvai skill: spoken prompts and the \xF0\x9F\x94\x8A line, shell commands for the owner as copyable ! blocks, "
    "messages to other sessions and hosts, a suggested work style with cheaper agents for builds and research.";

std::vector<SkillResult> installSkills(const std::string &home)
{
    std::vector<SkillResult> results;
    for (const char *rel : kTargets) {
        try {
            results.push_back(installOne(home + rel, kSkillContent));
        } catch (const std::exception &e) {
            results.push_back(result(home + rel + "/SKILL.md", "error", e.what()));
        }
    }
    return results;
}

std::vector<SkillResult> checkSkills(const std::string &home)
{
    std::vector<SkillResult> results;
    for (const char *rel : kTargets) {
        const std::string path = home + rel + "/SKILL.md";
        try {
            SkillResult r;
            bool missing = false;
            if (inspect(path, kSkillContent, r, missing))
                r = result(path, missing ? "missing" : "outdated");
            else if (r.status == "up to date")
                r.status = "installed";
            results.push_back(r);
        } catch (const std::exception &e) {
            results.push_back(result(path, "error", e.what()));
        }
    }
    return results;
}

std::string skillLogLine(const SkillResult &r)
{
    std::string text = r.status;
    if (r.status == "user file")
        text = "left alone (user file)";
    else if (r.status == "symlink")
        text = "left alone (symlink or not a regular file)";
    else if (r.status == "error")
        text = "not installed: " + r.detail;
    return "skill " + r.path + ": " + text;
}

} // namespace pvai
