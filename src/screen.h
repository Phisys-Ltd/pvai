// What a terminal shows: a small model of the screen Claude Code draws into
// the pseudo-terminal (cursor moves, erases, scrolling, the alternate
// screen), enough to read the text that is visible now. Colours and other
// attributes are dropped.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pvai {

class Screen {
public:
    Screen(int rows = 24, int cols = 80) { resize(rows, cols); }

    // A new size empties the screen; Claude Code draws it again.
    void resize(int rows, int cols);
    void reset() { resize(rows_, cols_); }
    void feed(const char *data, std::size_t n);

    int rows() const { return rows_; }
    // One row's text, without trailing blanks.
    std::string row(int r) const;

private:
    using Grid = std::vector<std::vector<std::string>>; // one UTF-8 character per cell, "" = blank

    void put(const std::string &ch, int width);
    void lineFeed();
    void scrollUp(int top, int bottom, int n);
    void scrollDown(int top, int bottom, int n);
    void eraseCells(int r, int from, int to);
    void csi(char final);
    int param(std::size_t i, int fallback) const;
    Grid &grid() { return alternate_ ? alt_ : main_; }
    const Grid &grid() const { return alternate_ ? alt_ : main_; }

    int rows_ = 24, cols_ = 80;
    Grid main_, alt_;
    bool alternate_ = false;
    int r_ = 0, c_ = 0;
    int savedR_ = 0, savedC_ = 0;
    int top_ = 0, bottom_ = 23; // scrolling region
    bool wrapPending_ = false;

    enum class State { Text, Escape, EscapeSkip, Csi, Osc, OscEscape, String };
    State state_ = State::Text;
    std::string params_;
    bool private_ = false;
    std::string utf8_;
    std::size_t utf8Need_ = 0;
    int lastR_ = -1, lastC_ = -1; // the cell a combining mark joins
};

// The dialog Claude Code shows in place of its prompt, read off the screen:
// its text (title, options, footer) when the last lines carry a dialog's
// footer ("Enter to confirm", "Esc to cancel", ...), else empty.
std::string screenDialog(const Screen &screen);

// Claude Code's prompt box: the lines between the last two rules on the
// screen, the first starting with "❯", or with "!" in shell mode (`shell`
// set then: what is typed there runs as a command, no prompt may go in).
// False when the screen shows none.
bool promptBox(const Screen &screen, std::string &text, bool *shell = nullptr);

// The lower part of the screen as text (the last 20 lines with text, up to
// 2000 bytes): what an unknown dialog shows.
std::string screenSnapshot(const Screen &screen);

// Claude Code's permission mode as its footer shows it ("⏵⏵ accept edits
// on", "⏸ plan mode on", "⏵⏵ auto mode on", "bypass permissions on"):
// acceptEdits, plan, auto, bypassPermissions; default when the prompt box
// shows and none of them; empty when the screen does not tell.
std::string screenMode(const Screen &screen);

} // namespace pvai
