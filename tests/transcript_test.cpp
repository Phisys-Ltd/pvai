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
    check(isMachinePrompt("[Message p1 from M4/w1, released by the owner]\nhi") && pvai::isAgentMessage("[Message p1 from M4/w1]"),
          "an agent message is the machine's");
    check(isMachinePrompt("Another Claude session sent a message:\n<agent-message>") && !pvai::isAgentMessage("<task-notification>\nx"),
          "a hand-back is, a task notification is the machine's but no agent message");
    check(!isMachinePrompt("what is [Message ] in C++?"), "the mark counts at the start only");
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
    // The history still shows what another session handed in; a title does not take it.
    const std::string agentFile = home.path + "/agent.jsonl";
    std::ofstream(agentFile) << userLine(60, "[Message p1 from M4/w1]\nhi there") << userLine(61, "<task-notification>x</task-notification>");
    pvai::TranscriptTail agentTail;
    agentTail.follow(agentFile);
    const auto agentEntries = agentTail.poll();
    check(agentEntries.size() == 1 && agentEntries[0].value("text", "") == "[Message p1 from M4/w1]\nhi there",
          "an agent message stays in the history, a task notification does not: " + std::to_string(agentEntries.size()));
    check(tail.localOutputs() == 0, "! output is no slash command's");
    check(tail.shellOutputs() == 2, "each ! output counts, also an empty one: " + std::to_string(tail.shellOutputs()));
    const std::string bangs = home.path + "/bangs.jsonl";
    std::ofstream(bangs) << userLine(1, "<bash-input>ssh pi5 uptime</bash-input>") << userLine(2, "<bash-stdout>up</bash-stdout><bash-stderr></bash-stderr>");
    check(!pvai::turnRunning(bangs), "! input starts no turn");
    check(pvai::turnRunning(path), "an owner prompt after them does, whether or not a turn ever ended");

    // The voice state of a transcript: the owner's last own prompt.
    {
        const std::string mic = "\xF0\x9F\x8E\x99 ";
        const std::string spoken = home.path + "/spoken.jsonl";
        std::ofstream(spoken) << userLine(1, "typed before") << userLine(2, mic + "what time is it") << userLine(3, "<system-reminder>x</system-reminder>")
                              << userLine(4, "[Message p1 from Mac]\nhello");
        check(pvai::lastPromptSpoken(spoken), "a 🎙 prompt, then an agent message: still spoken");
        const std::string typedAfter = home.path + "/typed.jsonl";
        std::ofstream(typedAfter) << userLine(1, mic + "spoken") << userLine(2, "<command-name>/model</command-name>") << userLine(3, "typed later");
        check(!pvai::lastPromptSpoken(typedAfter), "then a typed prompt: not spoken");
        const std::string slash = home.path + "/slash.jsonl";
        std::ofstream(slash) << userLine(1, mic + "spoken") << userLine(2, "<command-name>/model</command-name>") << userLine(3, "/model sonnet");
        check(pvai::lastPromptSpoken(slash), "slash and local command lines are no prompt");
        check(!pvai::lastPromptSpoken(home.path + "/none.jsonl"), "no file: not spoken");
    }

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

    // A /compact typed in the terminal (Claude Code 2.1.318): a plain
    // "/compact" line before the command lines; no turn end follows.
    const std::string slash = home.path + "/slash.jsonl";
    std::ofstream(slash) << userLine(30, "hello")
                         << R"({"type":"system","subtype":"turn_duration","uuid":"s4"})" "\n"
                         << userLine(31, "/compact") << userLine(32, "<command-name>/compact</command-name>")
                         << userLine(33, "<local-command-stdout>Compacted</local-command-stdout>");
    check(!pvai::turnRunning(slash), "a slash command typed in the terminal starts no turn");
    std::ofstream(slash, std::ios::app) << userLine(34, "/root/x is missing")
                                        << R"({"type":"assistant","uuid":"u35","message":{"content":[{"type":"text","text":"ok"}]}})" "\n";
    check(pvai::turnRunning(slash), "a prompt that starts with a slash runs once Claude answers");

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
    {
        // Claude Code's own queue: a prompt typed during a turn is enqueued with
        // its text and stays until a dequeue (oldest first) or a remove.
        const std::string file = home.path + "/queue-list.jsonl";
        const auto op = [](const char *operation, const char *text = nullptr) {
            pvai::json j = {{"type", "queue-operation"}, {"operation", operation}};
            if (text)
                j["content"] = text;
            return j.dump() + "\n";
        };
        std::ofstream(file) << op("enqueue", "eins") << op("dequeue") << op("enqueue", "zwei") << op("enqueue", "drei");
        pvai::TranscriptTail t;
        t.follow(file);
        t.poll();
        check(t.queued().size() == 2 && t.queued()[0] == "zwei" && t.queued()[1] == "drei", "a taken-up prompt left the queue, two wait");
        std::ofstream(file, std::ios::app) << op("remove", "drei");
        t.poll();
        check(t.queued().size() == 1 && t.queued()[0] == "zwei", "a remove takes its text out");
        std::ofstream(file, std::ios::app) << op("dequeue");
        t.poll();
        check(t.queued().empty(), "and the queue is empty");
    }
    {
        // A meta hand-back followed by Claude's answer is a turn of its own, like a notification.
        const std::string file = home.path + "/handback-wake.jsonl";
        std::ofstream(file) << macStart << macTool;
        pvai::TranscriptTail t;
        t.follow(file);
        t.poll();
        check(t.wakeTurns() == 1, "a meta hand-back and an answer: one wake turn");
    }

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
    // A model's own quota (Fable's): no quotaLimits, the reset is in apiErrorParams.
    const std::string fable = home.path + "/fable.jsonl";
    std::ofstream(fable) << userLine(60, "fable job")
                         << R"({"type":"assistant","uuid":"f61","timestamp":"t3","message":{"content":[{"type":"text","text":"You've reached your Fable limit. Run /usage-credits to continue or switch models with /model."}]},)"
                            R"("isApiErrorMessage":true,"apiError":"model_requires_usage_credits","error":"rate_limit","quotaLimits":null,)"
                            R"("apiErrorParams":{"rate_limit_info":{"status":"rejected","resetsAt":1791836400,"rateLimitType":"seven_day_overage_included"}}})" "\n"
                         << R"({"type":"system","subtype":"turn_duration","durationMs":5})" "\n";
    tail.follow(fable);
    check(kinds() == "prompt|limited|", "a Fable limit line is a limited entry");
    check(tail.limited().is_object() && tail.limited().value("resetsAt", 0LL) == 1791836400 && tail.limited().value("type", "") == "seven_day_overage_included"
              && tail.limited().value("model", false) && tail.limitHits() == 1,
          "a model's limit is read from apiErrorParams and flagged: " + tail.limited().dump());
    check(tail.recent().back().value("model", false) && tail.recent().back().value("resetsAt", 0LL) == 1791836400, "the entry carries the model flag");
    // An authentication failure, from the structured fields Claude Code 2.1.319 writes for a dead token
    // (its text says "Please run /login" too, but the text is never what counts). A limit is no login problem.
    const std::string auth = home.path + "/auth.jsonl";
    std::ofstream(auth) << userLine(70, "hi")
                        << R"({"type":"assistant","uuid":"a71","timestamp":"2026-11-09T09:42:37.504Z","message":{"model":"<synthetic>","content":[{"type":"text","text":"Failed to authenticate. API Error: 401 OAuth access token is invalid."}]},)"
                           R"("error":"authentication_failed","isApiErrorMessage":true,"apiErrorStatus":401})" "\n";
    tail.follow(auth);
    const std::string k1 = kinds();
    check(tail.authFailures() == 1 && tail.authFailedAt() == "2026-11-09T09:42:37.504Z" && tail.replies() == 0,
          "an authentication failure line is counted, no reply: " + k1 + " " + std::to_string(tail.authFailures()) + " " + std::to_string(tail.replies()));
    std::ofstream(auth, std::ios::app)
        << R"({"type":"assistant","uuid":"a72","message":{"content":[{"type":"text","text":"Please run /login · API Error: 401, Not logged in"}]}})" "\n"
        << R"({"type":"assistant","uuid":"a73","isSidechain":true,"isApiErrorMessage":true,"apiErrorStatus":401,"message":{"content":[{"type":"text","text":"x"}]}})" "\n";
    check(kinds() == "text|" && tail.authFailures() == 1 && tail.replies() == 1,
          "an answer that merely says it, and a subagent's error, are no login problem; the answer is a reply");
    std::ofstream(auth, std::ios::app)
        << R"({"type":"assistant","uuid":"a74","isApiErrorMessage":true,"apiError":"usage_limit_reached","apiErrorStatus":429,"message":{"content":[{"type":"text","text":"Limit"}]}})" "\n"
        << R"({"type":"assistant","uuid":"a75","isApiErrorMessage":true,"apiErrorStatus":401,"message":{"content":[{"type":"text","text":"Unauthorized"}]}})" "\n";
    kinds();
    check(tail.authFailures() == 2 && tail.replies() == 1, "a limit is none; a 401 API error is one");

    // A transcript is written by another program: null or wrongly typed
    // fields end a line's reading, never the daemon's.
    const std::string odd = home.path + "/odd.jsonl";
    std::ofstream(odd)
        << R"({"type":null,"isSidechain":null,"isMeta":null,"message":null})" "\n"
        << R"({"type":"user","isSidechain":"no","isMeta":3,"isCompactSummary":null,"timestamp":null,"message":{"content":null}})" "\n"
        << R"({"type":"user","uuid":null,"timestamp":5,"message":{"content":[{"type":"text","text":null},{"type":null}]}})" "\n"
        << R"({"type":"assistant","isSidechain":null,"isApiErrorMessage":null,"apiError":7,"error":null,"timestamp":null,"message":{"content":[{"type":"tool_use","id":null,"name":null,"input":null},{"type":"text","text":5}],"usage":{"cache_creation":{"ephemeral_1h_input_tokens":null}}}})" "\n"
        << R"({"type":"assistant","isApiErrorMessage":true,"apiError":"usage_limit_reached","timestamp":null,"quotaLimits":null,"message":{"content":[{"type":"text","text":null}]}})" "\n"
        << R"({"type":"system","subtype":null,"isSidechain":null,"compactMetadata":null})" "\n"
        << R"({"type":"system","subtype":"compact_boundary","compactMetadata":null})" "\n"
        << R"({"type":"queue-operation","operation":null,"content":null,"reason":null})" "\n"
        << R"({"type":"attachment","attachment":{"type":null,"prompt":null}})" "\n"
        << userLine(90, "after the odd ones") << "\n";
    bool threw = false;
    try {
        pvai::turnRunning(odd);
        pvai::lastPromptSpoken(odd);
        pvai::TranscriptTail oddTail;
        oddTail.follow(odd);
        const auto taken = oddTail.poll();
        std::string last;
        for (const auto &e : taken)
            last = e.value("kind", "") + ":" + e.value("text", "");
        check(last == "prompt:after the odd ones", "a line after the odd ones is still read: " + last);
        check(pvai::turnRunning(odd), "the turn state is still read from such a file");
        check(oddTail.compactions() == 1, "a compaction with null metadata still counts");
    } catch (const std::exception &e) {
        threw = true;
        std::printf("threw: %s\n", e.what());
    }
    check(!threw, "null fields in a transcript throw nothing");
    return pvai::test::finish();
}
