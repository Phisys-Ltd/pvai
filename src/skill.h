// The agent skill pvai ships, installed into the user's home on daemon start.
#pragma once

#include <string>
#include <vector>

namespace pvai {

// skills/pvai/SKILL.md, embedded at build time (cmake/embed_skill.cmake).
extern const char *const kSkillContent;

// Writes the skill to <home>/.claude/skills/pvai/SKILL.md and
// <home>/.agents/skills/pvai/SKILL.md where missing or an outdated pvai-managed
// copy (frontmatter key `managed-by: pvai`); a file without the marker and a
// symlink are left alone. Never throws; returns one log line per target.
std::vector<std::string> installSkills(const std::string &home);

} // namespace pvai
