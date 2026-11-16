// File upload (docs/PROTOCOL.md, "Files") against a real daemon: the upload
// connection, the inbox, `files` in a prompt, cleanup. A fake wrapper stands
// in for a Claude Code session.
//
// usage: pvai-upload-test /path/to/pvai
#include "support.h"

#include "inbox.h"

#include <openssl/evp.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <fstream>

using namespace pvai;
using namespace pvai::test;
namespace fs = std::filesystem;

namespace {

std::string g_pvai;

std::string sha256(const std::string &data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 15];
    }
    return out;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const std::string &path, const std::string &text)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// A file of `size` bytes with line ends and JSON in it: after the `upload`
// line none of it may be read as lines.
std::string content(std::size_t size, unsigned seed = 1)
{
    std::string data = "\n{\"type\":\"shutdown\",\"force\":true}\n\n";
    unsigned x = seed;
    while (data.size() < size) {
        x = x * 1103515245u + 12345u;
        data += static_cast<char>(x >> 16);
    }
    data.resize(size);
    return data;
}

json hello()
{
    return {{"type", "hello"}, {"client", "test-upload"}, {"version", "0"}};
}

json uploadLine(const std::string &session, const std::string &name, const std::string &data, const std::string &ref = "r1")
{
    return {{"type", "upload"}, {"ref", ref}, {"session", session}, {"name", name}, {"size", data.size()}, {"sha256", sha256(data)}};
}

// The answer of an upload connection: `uploaded` or `rejected`.
json answer(LineClient &c, int ms = 5000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        const json m = c.next(500);
        const std::string type = m.is_object() ? m.value("type", "") : "";
        if (type == "uploaded" || type == "rejected")
            return m;
    }
    return json::object();
}

// One whole upload on a connection of its own, the line and the bytes in one write.
json upload(const std::string &session, const std::string &name, const std::string &data, bool oneWrite = true)
{
    LineClient c;
    c.send(hello());
    const std::string line = uploadLine(session, name, data).dump() + "\n";
    if (oneWrite) {
        c.sendRaw(line + data);
    } else {
        c.sendRaw(line);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        c.sendRaw(data);
    }
    return answer(c);
}

std::vector<std::string> listing(const std::string &dir)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void hook(const std::string &id, json event)
{
    LineClient c;
    c.send({{"type", "hook"}, {"id", id}, {"event", std::move(event)}});
    c.next(300);
}

void ageFile(const std::string &path, int hours)
{
    timeval times[2];
    ::gettimeofday(&times[0], nullptr);
    times[0].tv_sec -= hours * 3600;
    times[1] = times[0];
    ::utimes(path.c_str(), times);
}

bool gone(const std::string &path, int ms = 6000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (!fs::exists(path))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/pvai\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    g_pvai = fs::absolute(argv[1]).string();
    const TestHome home;
    ::setenv("PVAI_CODEX", "", 1);
    ::unsetenv(kEnvId);
    const std::string inbox = home.path + "/.cache/pvai/inbox";

    // ---- The name, without a daemon.
    check(sanitizeUploadName("report.pdf") == "report.pdf", "a plain name stays");
    check(sanitizeUploadName("../../etc/passwd") == "passwd", "../ is cut off: basename only");
    check(sanitizeUploadName("C:\\dir\\file.txt") == "file.txt", "a backslash path too");
    check(sanitizeUploadName("a\x01" "b\x07\x7f.txt") == "ab.txt", "control characters go");
    check(sanitizeUploadName(".hidden") == "hidden" && sanitizeUploadName("...x") == "x", "no leading dot");
    check(sanitizeUploadName("a..b.txt") == "a.b.txt", "no ..");
    check(sanitizeUploadName("..").empty() && sanitizeUploadName("").empty() && sanitizeUploadName("a/").empty()
              && sanitizeUploadName("\x01\x02").empty() && sanitizeUploadName("   ").empty(),
          "nothing left: empty");
    check(sanitizeUploadName("my file (1).txt") == "my file (1).txt", "spaces stay");
    {
        const std::string longName = std::string(300, 'a') + ".pdf";
        const std::string cut = sanitizeUploadName(longName);
        check(cut.size() == kMaxUploadName && cut.substr(cut.size() - 4) == ".pdf", "a long name is cut, the extension kept");
        std::string utf8;
        for (int i = 0; i < 100; ++i)
            utf8 += "\xC3\xA4"; // 200 bytes of two-byte characters
        const std::string cutUtf8 = sanitizeUploadName(utf8);
        check(cutUtf8.size() <= kMaxUploadName && cutUtf8.size() % 2 == 0, "and never in the middle of a character");
    }

    // An inbox left by an earlier run: an old file, an empty directory, a fresh file of an unknown session.
    writeFile(inbox + "/wold/old.txt", "old");
    ageFile(inbox + "/wold/old.txt", 25);
    fs::create_directories(inbox + "/wempty");
    writeFile(inbox + "/wghost/new.txt", "new");

    const int port = 20000 + static_cast<int>(::getpid() % 20000);
    DaemonProcess daemon;
    daemon.start(g_pvai, port, 500, false, {{"PVAI_UPLOAD_IDLE", "2"}, {"PVAI_INBOX_SWEEP", "3"}, {"PVAI_CODEX", ""}});
    check(!fs::exists(inbox + "/wold") && !fs::exists(inbox + "/wempty"), "at the start: old files and empty directories are gone");
    check(fs::exists(inbox + "/wghost/new.txt"), "a fresh file of a session not known yet stays at the start");

    LineClient app;
    app.send(hello());
    const json welcome = app.next("welcome");
    check(welcome.value("upload", json()) == json({{"maxFile", 52428800}, {"maxSession", 104857600}}), "welcome.upload: " + welcome.dump());

    auto wrapper = std::make_unique<LineClient>();
    wrapper->send({{"type", "register"}, {"version", kVersion}, {"id", "wup"}, {"cwd", "/x"}, {"label", "up"}});
    hook("wup", {{"hook_event_name", "SessionStart"}, {"session_id", "c-1"}, {"source", "startup"}});
    app.next("sessions", [](const json &m) {
        for (const auto &s : m.value("sessions", json::array()))
            if (s.value("id", "") == "wup" && s.value("state", "") == "idle")
                return true;
        return false;
    });
    const std::string dir = inbox + "/wup";

    // ---- A good upload: the line and the bytes in one write; the bytes hold newlines and JSON.
    const std::string data = content(100000);
    json done;
    {
        LineClient c;
        c.send(hello());
        c.sendRaw(uploadLine("wup", "report.txt", data, "u1").dump() + "\n" + data);
        done = answer(c);
        check(done.value("type", "") == "uploaded" && done.value("ref", "") == "u1" && done.value("session", "") == "wup"
                  && done.value("size", 0) == 100000,
              "uploaded: " + done.dump());
        check(c.next("uploaded", {}, 300).empty(), "once on the upload connection");
    }
    const std::string path = done.value("path", "");
    const std::string name = done.value("name", "");
    check(path == dir + "/" + name && name.size() > 16 && name[8] == '-' && name[15] == '-' && name.substr(16) == "report.txt",
          "the path is absolute, the name has a timestamp: " + path);
    check(readFile(path) == data, "the stored bytes are the sent ones");
    struct stat st{}, dst{};
    ::stat(path.c_str(), &st);
    ::stat(dir.c_str(), &dst);
    check((st.st_mode & 07777) == 0600 && (dst.st_mode & 07777) == 0700, "file 0600, directory 0700");
    check(listing(dir).size() == 1, "no .part is left");
    const json heard = app.next("uploaded");
    check(heard.value("path", "") == path && heard.value("ref", "") == "u1", "every app hears it on its main connection");
    check(app.next("uploaded", {}, 300).empty(), "once");

    // The line first, the bytes later.
    const json later = upload("wup", "later.bin", content(5000, 2), false);
    check(later.value("type", "") == "uploaded" && readFile(later.value("path", "")) == content(5000, 2), "the bytes may follow the line later");
    // An empty file.
    const json empty = upload("wup", "empty.txt", "");
    check(empty.value("type", "") == "uploaded" && readFile(empty.value("path", "")).empty(), "an empty file is fine");

    // ---- Refusals.
    {
        app.drain();
        const std::size_t before = listing(dir).size();
        std::string bad = uploadLine("wup", "x.txt", data).dump();
        LineClient c;
        c.send(hello());
        c.sendRaw(bad + "\n" + std::string(data.size(), 'z'));
        json r = answer(c);
        check(r.value("type", "") == "rejected" && r.value("reason", "") == "checksum" && r.value("ref", "") == "r1" && r.value("session", "") == "wup",
              "a wrong checksum: " + r.dump());
        check(listing(dir).size() == before, "and nothing is left, no .part either");
        check(app.next("uploaded", {}, 300).empty(), "no app hears of it");

        json line = uploadLine("wup", "x.txt", data);
        line["size"] = kMaxUploadFile + 1;
        LineClient big;
        big.send(hello());
        big.send(line);
        r = answer(big);
        check(r.value("reason", "") == "too-large", "more than 50 MiB: too-large: " + r.dump());

        LineClient unknown;
        unknown.send(hello());
        unknown.sendRaw(uploadLine("nosuch", "x.txt", data).dump() + "\n" + data);
        check(answer(unknown).value("reason", "") == "unknown-session", "an unknown session");
        check(upload("../x", "x.txt", data).value("reason", "") == "unknown-session", "a session id that is a path: unknown-session");

        LineClient noSha;
        noSha.send(hello());
        noSha.send({{"type", "upload"}, {"ref", 1}, {"session", "wup"}, {"name", "x"}, {"size", 3}, {"sha256", "nope"}});
        check(answer(noSha).value("reason", "") == "bad-request", "a malformed line: bad-request");

        for (const char *badName : {"", "..", "///", "\x01"}) {
            const json n = upload("wup", badName, "abc");
            check(n.value("reason", "") == "bad-name", std::string("a name with nothing left: bad-name for '") + (badName[0] == 1 ? "\\x01" : badName) + "'");
        }

        // Truncated: the connection ends before `size` bytes.
        LineClient shortClient;
        shortClient.send(hello());
        shortClient.sendRaw(uploadLine("wup", "short.txt", data).dump() + "\n" + data.substr(0, 400));
        shortClient.shutdownWrite();
        r = answer(shortClient);
        check(r.value("reason", "") == "short", "the connection ends early: short: " + r.dump());
        // Silent: no bytes for 2 s (PVAI_UPLOAD_IDLE in this test).
        LineClient idle;
        idle.send(hello());
        idle.sendRaw(uploadLine("wup", "idle.txt", data).dump() + "\n" + data.substr(0, 400));
        r = answer(idle, 6000);
        check(r.value("reason", "") == "short", "no bytes for the idle time: short: " + r.dump());
        for (const auto &file : listing(dir))
            check(file.size() < 5 || file.substr(file.size() - 5) != ".part", "no .part after a short upload: " + file);
        check(listing(dir).size() == before, "the inbox is as it was");
        // The daemon is alive and the main connection too.
        app.send({{"type", "ping"}});
        check(!app.next("pong").empty(), "the daemon goes on");
    }

    // ---- Names: sanitised, stamped, never overwritten.
    {
        const json up = upload("wup", "../../etc/passwd", "root");
        check(up.value("type", "") == "uploaded" && fs::path(up.value("path", "")).parent_path() == dir
                  && up.value("name", "").substr(16) == "passwd",
              "../../etc/passwd lands in the inbox as passwd: " + up.dump());
        const json control = upload("wup", "a\x01" "b\x07.txt", "x");
        check(control.value("name", "").substr(16) == "ab.txt", "control characters are dropped");
        std::set<std::string> names;
        for (int i = 0; i < 4; ++i) {
            const json same = upload("wup", "same.txt", "v" + std::to_string(i));
            names.insert(same.value("name", ""));
            check(readFile(same.value("path", "")) == "v" + std::to_string(i), "collision " + std::to_string(i) + ": its own bytes");
        }
        check(names.size() == 4, "four uploads of one name: four files");
        bool counter = false;
        for (const auto &n : names)
            counter = counter || n.find("same-2.txt") != std::string::npos;
        check(counter, "the counter goes before the extension");
        std::string longName(300, 'q');
        const json lng = upload("wup", longName + ".pdf", "x");
        check(lng.value("name", "").size() <= 16 + kMaxUploadName && lng.value("name", "").substr(lng.value("name", "").size() - 4) == ".pdf",
              "a long name is cut");
    }

    // ---- Quota: 100 MiB per session.
    LineClient quotaWrapper; // stays until the end: its session keeps its directory
    {
        quotaWrapper.send({{"type", "register"}, {"version", kVersion}, {"id", "wq"}, {"cwd", "/x"}, {"label", "q"}});
        quotaWrapper.next(200);
        const std::string big = content(49u * 1024 * 1024, 7);
        check(upload("wq", "a.bin", big).value("type", "") == "uploaded", "49 MiB fit");
        check(upload("wq", "b.bin", big).value("type", "") == "uploaded", "another 49 MiB fit: 98");
        const json over = upload("wq", "c.bin", content(3u * 1024 * 1024, 8));
        check(over.value("reason", "") == "quota", "the third pushes the inbox over 100 MiB: quota: " + over.dump());
        check(listing(inbox + "/wq").size() == 2, "and left nothing");
    }

    // ---- Files in a prompt.
    {
        const json two = upload("wup", "my file.txt", "spaced");
        const json plain = upload("wup", "plain.txt", "plain");
        const std::string spaced = two.value("path", ""), simple = plain.value("path", "");
        app.drain();
        app.send({{"type", "prompt"}, {"ref", 1}, {"session", "wup"}, {"text", "look"}, {"files", {simple}}});
        check(app.next("accepted").value("delivery", "") == "typed", "a prompt with files is accepted");
        check(wrapper->next("type").value("text", "") == "look\n\n" + simple, "Claude Code: the path after a blank line");
        hook("wup", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", "look"}});
        hook("wup", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
        wrapper->drain();
        app.send({{"type", "prompt"}, {"ref", 2}, {"session", "wup"}, {"text", ""}, {"files", {spaced, simple}}});
        check(app.next("accepted").value("ref", 0) == 2, "only files, no text: accepted");
        const std::string typed = wrapper->next("type").value("text", "");
        check(typed == "Attached:\n\"" + spaced + "\"\n" + simple, "one path per line, one with a space quoted: " + typed);
        hook("wup", {{"hook_event_name", "UserPromptSubmit"}, {"prompt", typed}});
        hook("wup", {{"hook_event_name", "Stop"}, {"last_assistant_message", "ok"}});
        wrapper->drain();

        auto refused = [&](const json &files, const std::string &why) {
            app.send({{"type", "prompt"}, {"ref", 3}, {"session", "wup"}, {"text", "x"}, {"files", files}});
            const json r = app.next("rejected");
            check(r.value("reason", "") == "bad-file" && r.value("ref", 0) == 3, "bad-file for " + why + ": " + r.dump());
        };
        refused({"/etc/passwd"}, "a file outside the inbox");
        refused({dir + "/../wq/a.bin"}, "a path that climbs out and into another session's inbox");
        fs::create_symlink("/etc/passwd", dir + "/link");
        refused({dir + "/link"}, "a symlink out of the inbox");
        refused({dir}, "the directory itself");
        refused({dir + "/nosuch"}, "a missing file");
        refused({"relative.txt"}, "a relative path");
        refused(json(5), "files that is no array");
        refused({5}, "an entry that is no string");
        refused({std::string(inbox + "/wq/" + listing(inbox + "/wq")[0])}, "a file of another session");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(wrapper->next("type", {}, 300).empty(), "nothing of it was typed");
        app.send({{"type", "prompt"}, {"ref", 4}, {"session", "wup"}, {"text", ""}});
        check(app.next("rejected").value("reason", "") == "empty", "no text and no files is still empty");
    }

    // ---- The CLI.
    {
        writeFile(home.path + "/cli.txt", "from the cli");
        RunResult r = runPvai(g_pvai, "upload wup " + home.path + "/cli.txt");
        const json answered = json::parse(r.out, nullptr, false);
        check(r.status == 0 && answered.value("type", "") == "uploaded" && readFile(answered.value("path", "")) == "from the cli",
              "pvai upload: " + r.out);
        r = runPvai(g_pvai, "upload nosuch " + home.path + "/cli.txt");
        check(r.status == 1 && r.out.find("unknown-session") != std::string::npos, "pvai upload to an unknown session fails: " + r.out);
    }

    // ---- Cleanup: the periodic pass (every 3 s here) and the end of a session.
    {
        writeFile(dir + "/stale.txt", "stale");
        ageFile(dir + "/stale.txt", 25);
        check(gone(dir + "/stale.txt"), "the sweep removes a file older than 24 h");
        check(fs::exists(dir), "and keeps the session's directory with its newer files");
        check(gone(inbox + "/wghost"), "the sweep removes the directory of a session that does not exist");
        fs::create_directories(inbox + "/wnone");
        check(gone(inbox + "/wnone"), "and an empty one");
        wrapper->drain();
        wrapper.reset();
    }
    app.next("sessions", [](const json &m) {
        for (const auto &s : m.value("sessions", json::array()))
            if (s.value("id", "") == "wup")
                return false;
        return true;
    });
    check(gone(dir), "when the session ends its inbox directory goes");
    check(fs::exists(inbox + "/wq"), "another session's directory stays");

    daemon.stop();
    return finish();
}
