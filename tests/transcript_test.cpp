// What the transcript tail makes of the user lines Claude Code writes.
#include "support.h"
#include "transcript.h"

#include <cstdio>
#include <ctime>
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
    const std::string bangs = home.path + "/bangs.jsonl";
    std::ofstream(bangs) << userLine(1, "<bash-input>ssh pi5 uptime</bash-input>") << userLine(2, "<bash-stdout>up</bash-stdout><bash-stderr></bash-stderr>");
    check(!pvai::turnRunning(bangs), "! input starts no turn");
    check(pvai::turnRunning(path), "an owner prompt after them does, whether or not a turn ever ended");

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
    // A turn Claude Code starts on a task notification (a sub-agent's
    // hand-back) is a turn once it answers; before that it is not.
    const std::string asst = R"({"type":"assistant","uuid":"u31","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{}}]}})" "\n";
    const std::string done = R"({"type":"system","subtype":"turn_duration","uuid":"s30"})" "\n";
    const std::string prev = userLine(30, "hello")
                           + R"({"type":"assistant","uuid":"u30","message":{"content":[{"type":"text","text":"hi"}]}})" "\n" + done;
    const std::string note = userLine(32, "<task-notification><task-id>b1</task-id><status>completed</status></task-notification>");
    const std::string noteAttachment =
        R"({"type":"attachment","isSidechain":false,"attachment":{"type":"queued_command","prompt":"<task-notification>\n<status>completed</status>\n</task-notification>"}})" "\n";
    const std::string toolResult =
        R"({"type":"user","uuid":"u33","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})" "\n";
    auto turnOf = [&](const std::string &name, const std::string &body) {
        const std::string file = home.path + "/" + name + ".jsonl";
        std::ofstream(file) << body;
        pvai::TranscriptTail t;
        t.follow(file);
        t.poll();
        const auto seen = t.turnRunningSeen();
        check(seen && *seen == pvai::turnRunning(file), name + ": the file and the tail agree");
        return pvai::turnRunning(file);
    };
    check(turnOf("wake1", prev + note + asst), "a task notification answered by Claude is a running turn");
    check(turnOf("wake1b", prev + noteAttachment + asst + toolResult), "the same as an attachment, with a tool result after");
    check(!turnOf("wake2", prev + note), "a task notification alone is no turn");
    check(!turnOf("wake2b", prev + noteAttachment), "an attachment notification alone is no turn");
    check(!turnOf("wake3", prev + note + asst + done), "a notification turn that ended is no turn");
    auto stamped = [&](long long ago) {
        char buf[32];
        const std::time_t at = std::time(nullptr) - ago;
        std::tm tm{};
        gmtime_r(&at, &tm);
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        pvai::json line = pvai::json::parse(noteAttachment);
        line["timestamp"] = buf;
        return line.dump() + "\n";
    };
    check(turnOf("wake5", prev + stamped(60)), "a notification 1 min old, not answered yet: running");
    check(!turnOf("wake6", prev + stamped(2 * 3600)), "a notification 2 h old, alone: not running");
    check(!turnOf("wake7", prev + stamped(60) + asst + done), "a fresh notification whose turn ended: not running");
    // The Mac sequence: a meta hand-back line starts the turn, no turn_duration
    // in the window, tool calls going on.
    auto at = [&](long long ago) {
        char buf[32];
        const std::time_t t = std::time(nullptr) - ago;
        std::tm tm{};
        gmtime_r(&t, &tm);
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        return std::string(buf);
    };
    const std::string macStart = pvai::json({{"type", "user"}, {"isMeta", true}, {"timestamp", at(40)},
                                             {"message", {{"role", "user"}, {"content", "Another Claude session sent a message: <agent-message from=\"M4/w1\">hi</agent-message>"}}}}).dump() + "\n";
    const std::string macTool = pvai::json({{"type", "assistant"}, {"isSidechain", false}, {"timestamp", at(30)},
                                            {"message", {{"content", pvai::json::array({{{"type", "tool_use"}, {"id", "t9"}, {"name", "Bash"}, {"input", pvai::json::object()}}})}}}}).dump() + "\n";
    const std::string macRest = pvai::json({{"type", "user"}, {"timestamp", at(25)}, {"message", {{"role", "user"}, {"content", pvai::json::array({{{"type", "tool_result"}, {"tool_use_id", "t9"}, {"content", "ok"}}})}}}}).dump() + "\n"
                              + R"({"type":"attachment","isSidechain":false,"attachment":{"type":"hook_success"}})" "\n"
                              + R"({"type":"queue-operation","operation":"enqueue"})" "\n"
                              + R"({"type":"api-request","uuid":"r1"})" "\n" + R"({"type":"last-prompt"})" "\n";
    check(turnOf("mac1", macStart + macTool + macRest), "the Mac sequence, no turn end anywhere: running");
    check(turnOf("mac2", prev + macStart + macTool + macRest), "the Mac sequence after a finished turn: running");
    check(turnOf("mac3", prev + macStart), "a fresh meta hand-back alone: running");
    check(!turnOf("mac4", prev + macStart + macTool + done), "and ended by turn_duration: not running");
    check(turnOf("wake4", prev + userLine(34, "do it")), "an owner prompt starts a turn at once");

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
