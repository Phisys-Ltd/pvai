# cmake -DIN=SKILL.md -DOUT=skill_data.cpp -P embed_skill.cmake
file(READ "${IN}" content)
file(WRITE "${OUT}" "// Generated from skills/pvai/SKILL.md; do not edit.\n#include \"skill.h\"\n\nnamespace pvai {\n\nconst char *const kSkillContent = R\"pvai_skill_(${content})pvai_skill_\";\n\n} // namespace pvai\n")
