#include "screen.h"

#include <algorithm>
#include <cctype>
#include <array>

namespace pvai {

namespace {

// Columns a character takes: 0 for combining marks and joiners, 2 for wide
// East Asian characters and most emoji, 1 otherwise. An approximation of
// wcwidth that is good enough to keep the columns of a dialog in place.
int width(char32_t cp)
{
    if ((cp >= 0x300 && cp <= 0x36f) || cp == 0x200b || cp == 0x200c || cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f)
        || (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) || (cp >= 0xac00 && cp <= 0xd7a3)
        || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60)
        || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) || (cp >= 0x1f900 && cp <= 0x1f9ff)
        || (cp >= 0x1f680 && cp <= 0x1f6ff) || (cp >= 0x20000 && cp <= 0x3fffd))
        return 2;
    return 1;
}

char32_t decode(const std::string &s)
{
    const unsigned char b = static_cast<unsigned char>(s[0]);
    char32_t cp = s.size() == 1 ? b : s.size() == 2 ? b & 0x1f : s.size() == 3 ? b & 0x0f : b & 0x07;
    for (std::size_t i = 1; i < s.size(); ++i)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3f);
    return cp;
}

std::string trim(const std::string &s)
{
    const auto from = s.find_first_not_of(' ');
    if (from == std::string::npos)
        return {};
    return s.substr(from, s.find_last_not_of(' ') - from + 1);
}

// A row that is only a rule or a box edge: "────", "▔▔▔▔", "╭───╮", "╰───╯".
bool isRule(const std::string &row)
{
    const std::string t = trim(row);
    if (t.empty())
        return false;
    for (std::size_t i = 0; i < t.size();) {
        const unsigned char b = static_cast<unsigned char>(t[i]);
        const std::size_t n = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
        const char32_t cp = decode(t.substr(i, n));
        if (!(cp >= 0x2500 && cp <= 0x259f) && cp != ' ' && cp != '-' && cp != '=')
            return false;
        i += n;
    }
    return true;
}

// Leading and trailing box edges ("│ text │") off a row; its indent stays.
std::string unbox(std::string row)
{
    static const std::string bar = "\xE2\x94\x82"; // │
    if (const auto from = row.find_first_not_of(' '); from != std::string::npos && row.compare(from, bar.size(), bar) == 0)
        row.replace(from, bar.size(), " ");
    if (row.size() >= bar.size() && row.compare(row.size() - bar.size(), bar.size(), bar) == 0)
        row.resize(row.size() - bar.size());
    const auto end = row.find_last_not_of(' ');
    return end == std::string::npos ? std::string() : row.substr(0, end + 1);
}

// Lines collected bottom up, as text top down: blank lines at the ends
// dropped, the indent all lines share removed (options keep theirs), cut
// to 2000 bytes.
std::string joinLines(std::vector<std::string> lines)
{
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    while (!lines.empty() && lines.front().empty())
        lines.erase(lines.begin());
    std::size_t indent = std::string::npos;
    for (const auto &line : lines)
        if (!line.empty())
            indent = std::min(indent, line.find_first_not_of(' '));
    std::string out;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!out.empty())
            out += "\n";
        out += it->empty() ? *it : it->substr(indent);
    }
    if (out.size() > 2000) {
        std::size_t at = 2000;
        while (at > 0 && (static_cast<unsigned char>(out[at]) & 0xc0) == 0x80)
            --at;
        out.resize(at);
    }
    return out;
}

} // namespace

void Screen::resize(int rows, int cols)
{
    rows_ = std::max(rows, 1);
    cols_ = std::max(cols, 2);
    main_.assign(rows_, std::vector<std::string>(cols_));
    alt_ = main_;
    r_ = c_ = savedR_ = savedC_ = 0;
    top_ = 0;
    bottom_ = rows_ - 1;
    wrapPending_ = false;
    lastR_ = lastC_ = -1;
}

std::string Screen::row(int r) const
{
    if (r < 0 || r >= rows_)
        return {};
    std::string out;
    for (const auto &cell : grid()[r])
        out += cell.empty() ? " " : cell == "\x01" ? "" : cell; // \x01: the right half of a wide character
    const auto end = out.find_last_not_of(' ');
    return end == std::string::npos ? std::string() : out.substr(0, end + 1);
}

void Screen::eraseCells(int r, int from, int to)
{
    auto &line = grid()[r];
    for (int c = std::max(from, 0); c < std::min(to, cols_); ++c)
        line[c].clear();
}

void Screen::scrollUp(int top, int bottom, int n)
{
    auto &g = grid();
    for (int i = 0; i < n; ++i) {
        g.erase(g.begin() + top);
        g.insert(g.begin() + bottom, std::vector<std::string>(cols_));
    }
}

void Screen::scrollDown(int top, int bottom, int n)
{
    auto &g = grid();
    for (int i = 0; i < n; ++i) {
        g.erase(g.begin() + bottom);
        g.insert(g.begin() + top, std::vector<std::string>(cols_));
    }
}

void Screen::lineFeed()
{
    wrapPending_ = false;
    if (r_ == bottom_)
        scrollUp(top_, bottom_, 1);
    else if (r_ < rows_ - 1)
        ++r_;
}

void Screen::put(const std::string &ch, int w)
{
    if (w == 0) {
        if (lastR_ >= 0)
            grid()[lastR_][lastC_] += ch;
        return;
    }
    if (wrapPending_ || c_ + w > cols_) {
        c_ = 0;
        lineFeed();
    }
    auto &line = grid()[r_];
    line[c_] = ch;
    lastR_ = r_;
    lastC_ = c_;
    if (w == 2 && c_ + 1 < cols_)
        line[c_ + 1] = "\x01";
    c_ += w;
    if (c_ >= cols_) {
        c_ = cols_ - 1;
        wrapPending_ = true;
    }
}

int Screen::param(std::size_t i, int fallback) const
{
    std::size_t start = 0;
    for (std::size_t k = 0; k < i; ++k) {
        start = params_.find(';', start);
        if (start == std::string::npos)
            return fallback;
        ++start;
    }
    const std::size_t end = params_.find(';', start);
    const std::string p = params_.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos)
        return fallback;
    return std::min(std::stoi(p.substr(0, 6)), 100000);
}

void Screen::csi(char final)
{
    const int n = std::max(param(0, 1), 1);
    auto clampCursor = [&] {
        r_ = std::clamp(r_, 0, rows_ - 1);
        c_ = std::clamp(c_, 0, cols_ - 1);
        wrapPending_ = false;
    };
    if (private_) {
        if (final == 'h' || final == 'l') {
            const int mode = param(0, 0);
            if (mode == 1049 || mode == 1047 || mode == 47) {
                const bool on = final == 'h';
                if (on && !alternate_) {
                    savedR_ = r_;
                    savedC_ = c_;
                    alt_.assign(rows_, std::vector<std::string>(cols_));
                } else if (!on && alternate_) {
                    r_ = savedR_;
                    c_ = savedC_;
                }
                alternate_ = on;
                lastR_ = -1;
            }
        }
        return;
    }
    switch (final) {
    case 'A': r_ -= n; clampCursor(); break;
    case 'B': case 'e': r_ += n; clampCursor(); break;
    case 'C': case 'a': c_ += n; clampCursor(); break;
    case 'D': c_ -= n; clampCursor(); break;
    case 'E': r_ += n; c_ = 0; clampCursor(); break;
    case 'F': r_ -= n; c_ = 0; clampCursor(); break;
    case 'G': case '`': c_ = n - 1; clampCursor(); break;
    case 'd': r_ = n - 1; clampCursor(); break;
    case 'H': case 'f':
        r_ = std::max(param(0, 1), 1) - 1;
        c_ = std::max(param(1, 1), 1) - 1;
        clampCursor();
        break;
    case 'J': {
        const int mode = param(0, 0);
        if (mode == 0) {
            eraseCells(r_, c_, cols_);
            for (int r = r_ + 1; r < rows_; ++r)
                eraseCells(r, 0, cols_);
        } else if (mode == 1) {
            for (int r = 0; r < r_; ++r)
                eraseCells(r, 0, cols_);
            eraseCells(r_, 0, c_ + 1);
        } else {
            for (int r = 0; r < rows_; ++r)
                eraseCells(r, 0, cols_);
        }
        break;
    }
    case 'K': {
        const int mode = param(0, 0);
        eraseCells(r_, mode == 0 ? c_ : 0, mode == 1 ? c_ + 1 : cols_);
        break;
    }
    case 'X': eraseCells(r_, c_, c_ + n); break;
    case '@': {
        auto &line = grid()[r_];
        for (int i = 0; i < n && c_ < cols_; ++i) {
            line.insert(line.begin() + c_, std::string());
            line.pop_back();
        }
        break;
    }
    case 'P': {
        auto &line = grid()[r_];
        for (int i = 0; i < n && c_ < cols_; ++i) {
            line.erase(line.begin() + c_);
            line.emplace_back();
        }
        break;
    }
    case 'L': if (r_ >= top_ && r_ <= bottom_) scrollDown(r_, bottom_, std::min(n, bottom_ - r_ + 1)); break;
    case 'M': if (r_ >= top_ && r_ <= bottom_) scrollUp(r_, bottom_, std::min(n, bottom_ - r_ + 1)); break;
    case 'S': scrollUp(top_, bottom_, std::min(n, bottom_ - top_ + 1)); break;
    case 'T': scrollDown(top_, bottom_, std::min(n, bottom_ - top_ + 1)); break;
    case 'r': {
        const int top = std::max(param(0, 1), 1) - 1, bottom = std::min(param(1, rows_), rows_) - 1;
        if (top < bottom) {
            top_ = top;
            bottom_ = bottom;
        }
        r_ = c_ = 0;
        wrapPending_ = false;
        break;
    }
    case 's': savedR_ = r_; savedC_ = c_; break;
    case 'u': r_ = savedR_; c_ = savedC_; clampCursor(); break;
    default: break; // m (colours) and the rest change no text
    }
}

void Screen::feed(const char *data, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char b = static_cast<unsigned char>(data[i]);
        switch (state_) {
        case State::Text:
            if (utf8Need_ > 0) {
                if ((b & 0xc0) == 0x80) {
                    utf8_ += static_cast<char>(b);
                    if (--utf8Need_ == 0)
                        put(utf8_, width(decode(utf8_)));
                    break;
                }
                utf8Need_ = 0; // a broken sequence: drop it, read this byte afresh
            }
            if (b == 0x1b) {
                state_ = State::Escape;
            } else if (b == '\r') {
                c_ = 0;
                wrapPending_ = false;
            } else if (b == '\n' || b == 0x0b || b == 0x0c) {
                lineFeed();
            } else if (b == '\b') {
                if (c_ > 0)
                    --c_;
                wrapPending_ = false;
            } else if (b == '\t') {
                c_ = std::min((c_ / 8 + 1) * 8, cols_ - 1);
            } else if (b >= 0x20 && b < 0x7f) {
                put(std::string(1, static_cast<char>(b)), 1);
            } else if (b >= 0xc0 && b < 0xf8) {
                utf8_ = std::string(1, static_cast<char>(b));
                utf8Need_ = b < 0xe0 ? 1 : b < 0xf0 ? 2 : 3;
            }
            break;
        case State::Escape:
            state_ = State::Text;
            if (b == '[') {
                state_ = State::Csi;
                params_.clear();
                private_ = false;
            } else if (b == ']') {
                state_ = State::Osc;
            } else if (b == 'P' || b == '_' || b == '^' || b == 'X') {
                state_ = State::String;
            } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == '%') {
                state_ = State::EscapeSkip;
            } else if (b == '7') {
                savedR_ = r_;
                savedC_ = c_;
            } else if (b == '8') {
                r_ = savedR_;
                c_ = savedC_;
                wrapPending_ = false;
            } else if (b == 'D') {
                lineFeed();
            } else if (b == 'E') {
                c_ = 0;
                lineFeed();
            } else if (b == 'M') {
                wrapPending_ = false;
                if (r_ == top_)
                    scrollDown(top_, bottom_, 1);
                else if (r_ > 0)
                    --r_;
            } else if (b == 'c') {
                reset();
            }
            break;
        case State::EscapeSkip:
            state_ = State::Text;
            break;
        case State::Csi:
            if (b >= 0x40 && b <= 0x7e) {
                state_ = State::Text;
                csi(static_cast<char>(b));
            } else if (b == '?' || b == '>' || b == '<' || b == '=') {
                private_ = true;
            } else if (b == 0x1b) {
                state_ = State::Escape;
            } else if (params_.size() < 64) {
                params_ += static_cast<char>(b);
            }
            break;
        case State::Osc:
        case State::String:
            if (b == 0x07)
                state_ = State::Text;
            else if (b == 0x1b)
                state_ = State::OscEscape;
            break;
        case State::OscEscape:
            state_ = b == '\\' ? State::Text : State::Osc;
            break;
        }
    }
}

std::string screenDialog(const Screen &screen)
{
    static const std::array<const char *, 8> footers = {
        "Enter to confirm", "Enter to continue", "Enter to select", "Enter to submit",
        "Esc to cancel",    "Esc to exit",       "Esc to go back",  "Esc to close",
    };
    // The footer is one of the last two lines with text: further up it is
    // something said in the conversation.
    int footer = -1, seen = 0;
    for (int r = screen.rows() - 1; r >= 0 && seen < 2 && footer < 0; --r) {
        const std::string row = screen.row(r);
        if (trim(row).empty())
            continue;
        ++seen;
        for (const char *marker : footers)
            if (row.find(marker) != std::string::npos)
                footer = r;
    }
    if (footer < 0)
        return {};
    // Upwards to the rule or box edge above the dialog, at most 30 lines.
    std::vector<std::string> lines;
    int blanks = 0;
    for (int r = footer; r >= 0 && footer - r < 30; --r) {
        const std::string row = screen.row(r);
        if (isRule(row)) {
            if (r != footer && !lines.empty())
                break;
            continue;
        }
        const std::string text = unbox(row);
        if (text.empty()) {
            if (++blanks >= 3)
                break;
            if (!lines.empty() && !lines.back().empty())
                lines.push_back({});
            continue;
        }
        blanks = 0;
        lines.push_back(text);
    }
    return joinLines(lines);
}

bool promptBox(const Screen &screen, std::string &text, bool *shell)
{
    static const std::string mark = "\xE2\x9D\xAF"; // ❯
    int below = -1;
    for (int r = screen.rows() - 1; r >= 0; --r) {
        if (!isRule(screen.row(r)))
            continue;
        if (below < 0 || below - r < 2 || below - r > 16) {
            below = r;
            continue;
        }
        const std::string first = screen.row(r + 1);
        const auto from = first.find_first_not_of(' ');
        // "!" and a space; Claude Code draws a no-break space there.
        const bool bang = from != std::string::npos
            && (first.compare(from, 2, "! ") == 0 || first.compare(from, 3, "!\xC2\xA0") == 0);
        if (from == std::string::npos || (!bang && first.compare(from, mark.size(), mark) != 0)) {
            below = r;
            continue;
        }
        if (shell)
            *shell = bang;
        text.clear();
        for (int k = r + 1; k < below; ++k)
            text += (k > r + 1 ? "\n" : "") + screen.row(k);
        return true;
    }
    return false;
}

std::string screenMode(const Screen &screen)
{
    static const std::pair<const char *, const char *> marks[] = {
        {"accept edits on", "acceptEdits"},
        {"plan mode on", "plan"},
        {"auto mode on", "auto"},
        {"bypass permissions on", "bypassPermissions"},
    };
    int seen = 0;
    for (int r = screen.rows() - 1; r >= 0 && seen < 6; --r) {
        std::string row = screen.row(r);
        if (trim(row).empty())
            continue;
        ++seen;
        for (char &c : row)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto &[mark, mode] : marks)
            if (row.find(mark) != std::string::npos)
                return mode;
    }
    std::string box;
    return promptBox(screen, box) ? "default" : "";
}

std::string screenSnapshot(const Screen &screen)
{
    std::vector<std::string> lines; // bottom up, as screenDialog collects them
    for (int r = screen.rows() - 1; r >= 0 && lines.size() < 20; --r) {
        const std::string row = screen.row(r);
        if (trim(row).empty()) {
            if (!lines.empty() && !lines.back().empty())
                lines.push_back({});
            continue;
        }
        lines.push_back(isRule(row) ? std::string("\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80") : row); // a rule as ───
    }
    return joinLines(lines);
}

} // namespace pvai
