// installSkills(): fresh home, outdated managed copy, user file, symlink.
#include "skill.h"
#include "support.h"

#include <fstream>
#include <sstream>

using namespace pvai;
using namespace pvai::test;
namespace fs = std::filesystem;

static std::string slurp(const std::string &p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream b;
    b << in.rdbuf();
    return b.str();
}

static void put(const std::string &p, const std::string &s)
{
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

int main()
{
    TestHome home;
    const std::string content = kSkillContent;
    check(content.find("\nmanaged-by: pvai\n") != std::string::npos, "embedded skill carries the marker");
    const std::string c = home.path + "/.claude/skills/pvai/SKILL.md";
    const std::string a = home.path + "/.agents/skills/pvai/SKILL.md";

    auto lines = installSkills(home.path);
    check(lines.size() == 2 && slurp(c) == content && slurp(a) == content, "fresh home: both written with the embedded content");
    check(lines[0].find("written") != std::string::npos, "log says written");
    lines = installSkills(home.path);
    check(lines[0].find("up to date") != std::string::npos && lines[1].find("up to date") != std::string::npos, "second run: up to date");

    put(c, "---\nname: pvai\nmanaged-by: pvai\n---\nold text\n");
    lines = installSkills(home.path);
    check(slurp(c) == content && lines[0].find("written") != std::string::npos, "managed copy with old content rewritten");

    const std::string mine = "---\nname: pvai\ndescription: mine\n---\nmy own\n";
    put(a, mine);
    lines = installSkills(home.path);
    check(slurp(a) == mine && lines[1].find("user file") != std::string::npos, "user file without marker untouched");

    fs::remove(c);
    put(home.path + "/target.md", "x");
    fs::create_symlink(home.path + "/target.md", c);
    lines = installSkills(home.path);
    check(fs::is_symlink(c) && slurp(home.path + "/target.md") == "x", "symlink left alone");

    TestHome ro; // unwritable parent: no throw, one line each
    fs::permissions(ro.path, fs::perms::owner_read | fs::perms::owner_exec);
    lines = installSkills(ro.path);
    fs::permissions(ro.path, fs::perms::owner_all);
    check(lines.size() == 2, "unwritable home: logged, no crash");
    return finish();
}
