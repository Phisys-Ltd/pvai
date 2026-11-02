// What the transcript tail makes of the user lines Claude Code writes.
#include "support.h"
#include "transcript.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

using pvai::test::check;

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
    check(pvai::stripPasteFrame("\n\n<pasted_content id=\"8916\">\n[Message p1 from x]\nhi\n</pasted_content id=\"8916\">") == "[Message p1 from x]\nhi",
          "a paste frame is stripped");
    check(pvai::stripPasteFrame("plain") == "plain", "no frame, no change");

    const pvai::test::TestHome home("/tmp/pvai-transcript-", false);
    const std::string path = home.path + "/t.jsonl";
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
    const std::string idle = home.path + "/idle.jsonl";
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

    // A background task's notification (string content) after a finished
    // turn is no turn: without a Stop it would hold the session busy.
    const std::string notified = home.path + "/notified.jsonl";
    std::ofstream(notified) << userLine(20, "hello")
                            << R"({"type":"assistant","uuid":"u21","message":{"content":[{"type":"text","text":"hi"}]}})" "\n"
                            << R"({"type":"system","subtype":"turn_duration","uuid":"s20"})" "\n"
                            << userLine(22, "<task-notification><task-id>b1</task-id><status>completed</status></task-notification>");
    check(!pvai::turnRunning(notified), "an old task notification starts no turn");
    tail.follow(idle);
    got.clear();
    for (const auto &e : tail.poll())
        got += e.value("kind", "") + ":" + e.value("text", "") + "|";
    check(got == "prompt:hello|text:hi|command:/compact|", "a compaction is a /compact entry, its summary none: " + got);
    // A pasted prompt shows without its frame, like the plain one.
    const std::string pasted = home.path + "/pasted.jsonl";
    std::ofstream(pasted) << userLine(30, "\n\n<pasted_content id=\"8916\">\n[Message p1 from x]\nhi\n</pasted_content id=\"8916\">")
                          << userLine(31, "[Message p1 from x]\nhi");
    tail.follow(pasted);
    got.clear();
    for (const auto &e : tail.poll())
        got += e.value("kind", "") + ":" + e.value("text", "") + "|";
    check(got == "prompt:[Message p1 from x]\nhi|prompt:[Message p1 from x]\nhi|", "a pasted prompt is the plain one: " + got);
    // The text of the last local command's output.
    const std::string local = home.path + "/local.jsonl";
    std::ofstream(local) << userLine(20, "<command-name>/model</command-name>")
                         << userLine(21, "<local-command-stdout>Kept model as \x1b[1mOpus 5.5\x1b[22m\n</local-command-stdout>");
    tail.follow(local);
    tail.poll();
    check(tail.localOutputs() == 1 && tail.lastLocalOutput() == "Kept model as Opus 5.5",
          "the last local output, stripped and trimmed: [" + tail.lastLocalOutput() + "]");
    std::ofstream(local, std::ios::app) << userLine(22, "<local-command-stdout></local-command-stdout>");
    tail.poll();
    check(tail.localOutputs() == 2 && tail.lastLocalOutput().empty(), "an empty output clears it");
    // A line of the wrong shape is skipped; the lines after it still parse.
    const std::string bad = home.path + "/bad.jsonl";
    std::ofstream(bad) << R"({"type":1})" "\n"
                       << R"({"type":"user","message":5})" "\n"
                       << R"({"type":"system","subtype":"local_command"})" "\n"
                       << userLine(30, "after the bad lines");
    tail.follow(bad);
    got.clear();
    for (const auto &e : tail.poll())
        got += e.value("kind", "") + ":" + e.value("text", "") + "|";
    check(got == "prompt:after the bad lines|", "malformed lines are skipped: " + got);
    // The usage limit and the resumption after it.
    const std::string limit = home.path + "/limit.jsonl";
    std::ofstream(limit) << userLine(40, "long job")
                         << R"({"type":"queue-operation","operation":"dequeue"})" "\n"
                         << R"({"type":"assistant","uuid":"u41","timestamp":"t1","message":{"content":[{"type":"text","text":"You've hit your session limit\nresets 7:30pm"}]},)"
                            R"("isApiErrorMessage":true,"apiError":"usage_limit_reached","apiErrorStatus":429,"error":"rate_limit",)"
                            R"("quotaLimits":{"status":"rejected","resetsAt":1791826200,"rateLimitType":"five_hour"}})" "\n"
                         << R"({"type":"system","subtype":"turn_duration","durationMs":5})" "\n";
    tail.follow(limit);
    auto kinds = [&]() {
        std::string out;
        for (const auto &e : tail.poll())
            out += e.value("kind", "") + "|";
        return out;
    };
    check(kinds() == "prompt|limited|", "a limit line is a limited entry, an ordinary dequeue none");
    check(tail.limited().is_object() && tail.limited().value("resetsAt", 0LL) == 1791826200 && tail.limited().value("type", "") == "five_hour"
              && tail.limited().value("text", "") == "You've hit your session limit resets 7:30pm" && tail.limitHits() == 1 && tail.turnEndCount() == 1,
          "limited(): " + tail.limited().dump());
    check(tail.recent().back().value("resetsAt", 0LL) == 1791826200, "the entry carries resetsAt");
    std::ofstream(limit, std::ios::app) << R"({"type":"queue-operation","operation":"dequeue","timestamp":"t2"})" "\n";
    check(kinds() == "resumed|" && tail.limited().is_null() && tail.limitClears() == 1, "a dequeue ends the limit with a resumed entry");
    std::ofstream(limit, std::ios::app) << R"({"type":"assistant","uuid":"u50","isApiErrorMessage":true,"apiError":"usage_limit_reached","message":{"content":[{"type":"text","text":"Limit"}]}})" "\n"
                                        << R"({"type":"assistant","uuid":"u51","message":{"content":[{"type":"text","text":"Back."}]}})" "\n";
    check(kinds() == "limited|text|" && tail.limited().is_null() && tail.limitHits() == 2,
          "a limit line without quotaLimits, ended by the next ordinary answer");
    return pvai::test::finish();
}
