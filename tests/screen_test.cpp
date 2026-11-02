// The screen model and the dialogs read off it, from drawn output.
#include "screen.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

void feed(pvai::Screen &screen, const std::string &data) { screen.feed(data.data(), data.size()); }

} // namespace

int main()
{
    pvai::Screen screen(12, 40);
    const std::string rule = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"; // ──────
    // A conversation that quotes a footer, then the prompt with a status line.
    feed(screen, "\x1b[2J\x1b[HSay: Enter to confirm\r\n\r\n" + rule + "\r\n\xE2\x9D\xAF \r\n" + rule + "\r\n  ? for shortcuts");
    check(screen.row(0) == "Say: Enter to confirm", "text lands in its row: " + screen.row(0));
    check(pvai::screenDialog(screen).empty(), "a footer in the conversation is no dialog");
    std::string box;
    check(pvai::promptBox(screen, box) && box == "\xE2\x9D\xAF", "the prompt box between its rules: [" + box + "]");

    // The prompt redrawn as a dialog: up, erase, draw, with colours.
    feed(screen, "\x1b[3A\r\x1b[J" + rule + "\r\n\x1b[1m Pick one\x1b[0m\r\n \xE2\x9D\xAF 1. Yes\r\n   2. No\r\n\r\n Enter to confirm \xC2\xB7 Esc to cancel");
    const std::string dialog = pvai::screenDialog(screen);
    check(dialog == "Pick one\n\xE2\x9D\xAF 1. Yes\n  2. No\n\nEnter to confirm \xC2\xB7 Esc to cancel", "the dialog: [" + dialog + "]");
    check(!pvai::promptBox(screen, box), "no prompt box under the dialog");

    // The selection moves: one cell rewritten in place.
    feed(screen, "\x1b[3A\x1b[2G \x1b[1B\x1b[2G\xE2\x9D\xAF");
    check(pvai::screenDialog(screen).find("  1. Yes\n\xE2\x9D\xAF 2. No") != std::string::npos,
          "a moved selection: [" + pvai::screenDialog(screen) + "]");

    // Closed: the prompt again.
    feed(screen, "\x1b[3A\r\x1b[J" + rule + "\r\n\xE2\x9D\xAF \r\n" + rule + "\r\n  ? for shortcuts");
    check(pvai::screenDialog(screen).empty(), "the closed dialog is gone");
    feed(screen, "\x1b[2A\x1b[3Ghi");
    check(pvai::promptBox(screen, box) && box == "\xE2\x9D\xAF hi", "typing into the box changes it: [" + box + "]");

    // Shell mode: the box starts with "!", a command in it is no free prompt.
    {
        pvai::Screen shell(8, 40);
        bool bang = false;
        feed(shell, "\x1b[2J\x1b[H" + rule + "\r\n!  cd /tmp && ls\r\n  -la\r\n" + rule + "\r\n  ! for shell mode");
        std::string text;
        check(pvai::promptBox(shell, text, &bang) && bang && text == "!  cd /tmp && ls\n  -la",
              "a box in shell mode: [" + text + "]");
        check(pvai::screenDialog(shell).empty(), "shell mode is no dialog");
        feed(shell, "\x1b[2;1H\x1b[J\xE2\x9D\xAF \r\n" + rule);
        check(pvai::promptBox(shell, text, &bang) && !bang, "back to the prompt: not shell mode");
        feed(shell, "\x1b[2;1H\x1b[J!important\r\n" + rule);
        check(!pvai::promptBox(shell, text, &bang), "\"!\" without a space is no shell box");
        feed(shell, "\x1b[2;1H\x1b[J!\xC2\xA0" "echo\x1b[8Gzwei\r\n" + rule);
        check(pvai::promptBox(shell, text, &bang) && bang, "as Claude Code draws it, with a no-break space: [" + text + "]");
    }

    // The permission mode in the footer under the prompt box.
    check(pvai::screenMode(screen) == "default", "no mode in the footer: default");
    feed(screen, "\r\x1b[K  \xE2\x8F\xB8 plan mode on (shift+tab to cycle)");
    check(pvai::screenMode(screen) == "plan", "plan mode on: " + pvai::screenMode(screen));
    feed(screen, "\r\x1b[K  \xE2\x8F\xB5\xE2\x8F\xB5 accept edits on (shift+tab to cycle)");
    check(pvai::screenMode(screen) == "acceptEdits", "accept edits on");
    feed(screen, "\r\x1b[K  \xE2\x8F\xB5\xE2\x8F\xB5 auto mode on (shift+tab to cycle)");
    check(pvai::screenMode(screen) == "auto", "auto mode on");

    // Scrolling at the bottom and the alternate screen.
    pvai::Screen small(3, 12);
    feed(small, "a\r\nb\r\nc\r\nd");
    check(small.row(0) == "b" && small.row(2) == "d", "a line feed at the bottom scrolls");
    feed(small, "\x1b[?1049h\x1b[HEsc to exit");
    check(pvai::screenDialog(small) == "Esc to exit", "the alternate screen is read: [" + pvai::screenDialog(small) + "] [" + small.row(0) + "]");
    feed(small, "\x1b[?1049l");
    check(small.row(2) == "d" && pvai::screenDialog(small).empty(), "and leaving it shows the main screen again");

    // A panel taller than the screen: no footer, its scroll mark at the bottom.
    {
        pvai::Screen panel(8, 40);
        feed(panel, "\x1b[2J\x1b[H\xE2\x96\x94\xE2\x96\x94\xE2\x96\x94\r\n  Settings  Usage\r\n\r\n  Current week (Fable)\r\n  10% used\r\n\r\n  Subagents\r\n\x1b[38G\xE2\x86\x93");
        const std::string text = pvai::screenDialog(panel);
        check(text == "Settings  Usage\n\nCurrent week (Fable)\n10% used\n\nSubagents", "a scrolled panel: [" + text + "]");
        feed(panel, "\x1b[2J\x1b[HSee below:\r\n\xE2\x86\x93 here");
        check(pvai::screenDialog(panel).empty(), "an arrow in a line of text is no panel");
    }

    // Wide characters take two cells.
    pvai::Screen wide(2, 10);
    feed(wide, "\xF0\x9F\x8E\x99x");
    check(wide.row(0) == "\xF0\x9F\x8E\x99x", "an emoji and the letter after it: " + wide.row(0));

    std::printf(failures ? "FAILED\n" : "all passed\n");
    return failures ? 1 : 0;
}
