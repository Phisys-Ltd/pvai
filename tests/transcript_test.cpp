// What the transcript tail makes of the user lines Claude Code writes.
#include "transcript.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

std::string userLine(int n, const std::string &text)
{
    return pvai::json({{"type", "user"}, {"uuid", "u" + std::to_string(n)}, {"message", {{"role", "user"}, {"content", text}}}}).dump()
         + "\n";
}

} // namespace

int main()
{
    using pvai::isMachinePrompt;
    check(isMachinePrompt("<system-reminder>\nx</system-reminder>"), "a reminder is the machine's");
    check(isMachinePrompt("<task-notification>\n<task-id>a</task-id>"), "a task notification is the machine's");
    check(isMachinePrompt("<artifact-content-authored-by-others/>\nx"), "a self-closed tag is the machine's");
    check(isMachinePrompt("<bash-input>ls</bash-input>"), "! input is the machine's");
    check(!isMachinePrompt("<div>my own XML</div> what is wrong here?"), "an owner's prompt starting with a tag stays");
    check(!isMachinePrompt("<pasted_content id=\"x\">text</pasted_content>"), "a paste stays");
    check(!isMachinePrompt("<bash-inputs>x"), "only whole tag names");
    check(!isMachinePrompt("plain"), "plain text is the owner's");

    char dir[] = "/tmp/pvai-transcript-XXXXXX";
    if (!::mkdtemp(dir))
        return 1;
    const std::string path = std::string(dir) + "/t.jsonl";
    std::ofstream(path) << userLine(1, "<bash-input>ssh pi5 uptime</bash-input>")
                        << userLine(2, "<bash-stdout>\x1b[1mup\x1b[0m 3 days</bash-stdout><bash-stderr>warn\n</bash-stderr>")
                        << userLine(3, "<bash-stdout></bash-stdout><bash-stderr></bash-stderr>")
                        << userLine(4, "<div>mine</div>") << userLine(5, "<system-reminder>x</system-reminder>");
    pvai::TranscriptTail tail;
    tail.follow(path);
    const auto entries = tail.poll();
    std::string got;
    for (const auto &e : entries)
        got += e.value("kind", "") + ":" + e.value("text", "") + "|";
    check(got == "command:! ssh pi5 uptime|output:up 3 days\nwarn\n|prompt:<div>mine</div>|", "the entries: " + got);
    check(tail.localOutputs() == 0, "! output is no slash command's");
    check(tail.shellOutputs() == 2, "each ! output counts, also an empty one: " + std::to_string(tail.shellOutputs()));
    check(!pvai::turnRunning(path), "! input starts no turn");

    // An idle compaction after a finished turn (Claude Code 2.1.31x): the
    // summary is no prompt, and no turn runs.
    const std::string idle = std::string(dir) + "/idle.jsonl";
    pvai::json summary = pvai::json::parse(userLine(12, "This session is being continued from a previous conversation"));
    summary["isCompactSummary"] = true;
    summary["isVisibleInTranscriptOnly"] = true;
    std::ofstream(idle) << userLine(10, "hello")
                        << R"({"type":"assistant","uuid":"u11","message":{"content":[{"type":"text","text":"hi"}]}})" "\n"
                        << R"({"type":"system","subtype":"turn_duration","uuid":"s1"})" "\n"
                        << R"({"type":"system","subtype":"compact_boundary","uuid":"s2","compactMetadata":{"trigger":"auto"}})" "\n"
                        << summary.dump() << "\n"
                        << R"({"type":"system","subtype":"informational","uuid":"s3","content":"Compacted while idle"})" "\n";
    check(!pvai::turnRunning(idle), "an idle compaction starts no turn");
    tail.follow(idle);
    got.clear();
    for (const auto &e : tail.poll())
        got += e.value("kind", "") + ":" + e.value("text", "") + "|";
    check(got == "prompt:hello|text:hi|command:/compact|", "a compaction is a /compact entry, its summary none: " + got);
    std::remove(idle.c_str());
    std::remove(path.c_str());
    ::rmdir(dir);
    return failures ? 1 : 0;
}
