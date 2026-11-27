// The agent skill pvai ships, installed into the user's home on daemon start.
#pragma once

#include <string>
#include <vector>

namespace pvai {

// skills/pvai/SKILL.md, embedded at build time (cmake/embed_skill.cmake).
extern const char *const kSkillContent;

// One line, shown to the owner where the skill is announced (welcome, terminal).
extern const char *const kSkillSummary;

// The outcome for one target. status: "written", "up to date", "user file",
// "symlink", "error" (detail = the reason); checkSkills() reports "installed",
// "outdated" and "missing" instead of "written"/"up to date".
struct SkillResult {
    std::string path; // .../SKILL.md
    std::string status;
    std::string detail;
};

// Writes the skill to <home>/.claude/skills/pvai/SKILL.md and
// <home>/.agents/skills/pvai/SKILL.md where missing or an outdated pvai-managed
// copy (frontmatter key `managed-by: pvai`); a file without the marker and a
// symlink are left alone. Never throws; returns one result per target.
std::vector<SkillResult> installSkills(const std::string &home);

// The same targets, read-only: nothing is written.
std::vector<SkillResult> checkSkills(const std::string &home);

// The log line for a result, e.g. "skill <path>: up to date".
std::string skillLogLine(const SkillResult &result);


} // namespace pvai
