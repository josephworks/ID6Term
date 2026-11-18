// term_buffer.hpp - the console text: a bounded ring of lines fed from raw process output.
//
// Pure C++ (no QNX / BMW dependencies) so it is host-testable. Raw bytes go in through append(); lines come out
// through render(), newest last, as one string with '\n' separators for a Richtext list cell.
//   * '\n' ends a line; '\r' alone (or "\r\n") is treated as a line end too, "\r\n" counting once.
//   * Tabs become spaces (next multiple of 8). Other control characters and ANSI "ESC [ ... <letter>" sequences are
//     dropped, so a stray `ls --color` or a progress bar does not litter the HMI text.
//   * Lines longer than maxLineBytes are wrapped (hard split) so one giant line cannot blow the model size.
//   * At most maxLines lines are kept; the oldest go first. render() can be further capped by bytes.
#ifndef ID6TERM_TERM_BUFFER_HPP
#define ID6TERM_TERM_BUFFER_HPP

#include <stddef.h>
#include <string>
#include <deque>

namespace id6term {

class TermBuffer {
public:
    explicit TermBuffer(size_t maxLines = 200, size_t maxLineBytes = 160)
        : m_maxLines(maxLines), m_maxLineBytes(maxLineBytes), m_escape(0), m_lastWasCR(false), m_generation(0) {}

    void append(const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) put((unsigned char)data[i]);
        ++m_generation;
    }
    void append(const std::string& s) { append(s.data(), s.size()); }

    // A whole line from the app itself (prompt echo, status), always on its own line.
    void addLine(const std::string& line) {
        if (!m_partial.empty()) commitPartial();
        m_lastWasCR = false;
        m_escape = 0;
        std::string l = line;
        pushLine(l);
        ++m_generation;
    }

    void clear() {
        m_lines.clear();
        m_partial.clear();
        m_escape = 0;
        m_lastWasCR = false;
        ++m_generation;
    }

    // Lines joined with '\n', oldest first; if maxBytes > 0 only the newest lines that fit are returned.
    void render(std::string& out, size_t maxBytes = 0) const {
        out.clear();
        size_t total = 0;
        size_t first = 0;
        size_t count = m_lines.size() + (m_partial.empty() ? 0 : 1);
        if (maxBytes) {
            size_t i = count;
            while (i > 0) {
                const std::string& l = lineAt(i - 1);
                size_t add = l.size() + (i < count ? 1 : 0);
                if (total + add > maxBytes) break;
                total += add;
                --i;
            }
            first = i;
        }
        for (size_t i = first; i < count; ++i) {
            if (i > first) out += '\n';
            out += lineAt(i);
        }
    }

    size_t lineCount() const { return m_lines.size() + (m_partial.empty() ? 0 : 1); }
    bool empty() const { return m_lines.empty() && m_partial.empty(); }
    unsigned generation() const { return m_generation; }   // bumps on every change: cheap "is it dirty" check

private:
    const std::string& lineAt(size_t i) const { return i < m_lines.size() ? m_lines[i] : m_partial; }

    void put(unsigned char c) {
        if (m_escape) {                                    // inside ESC [ ... <final byte 0x40..0x7e>
            if (m_escape == 1) { m_escape = (c == '[') ? 2 : 0; return; }
            if (c >= 0x40 && c <= 0x7e) m_escape = 0;
            return;
        }
        if (c == 0x1b) { m_escape = 1; return; }
        if (c == '\n') {
            if (m_lastWasCR) { m_lastWasCR = false; return; }   // "\r\n" already ended the line
            commitPartial();
            return;
        }
        if (c == '\r') { m_lastWasCR = true; commitPartial(); return; }
        m_lastWasCR = false;
        if (c == '\t') {
            size_t col = m_partial.size();
            size_t spaces = 8 - (col % 8);
            m_partial.append(spaces, ' ');
        } else if (c < 0x20 || c == 0x7f) {
            return;                                        // other control characters
        } else {
            m_partial += (char)c;
        }
        if (m_partial.size() >= m_maxLineBytes) commitPartial();
    }

    void commitPartial() {
        pushLine(m_partial);
        m_partial.clear();
    }

    void pushLine(std::string& l) {
        m_lines.push_back(std::string());
        m_lines.back().swap(l);
        while (m_lines.size() > m_maxLines) m_lines.pop_front();
    }

    size_t m_maxLines;
    size_t m_maxLineBytes;
    std::deque<std::string> m_lines;
    std::string m_partial;
    int m_escape;
    bool m_lastWasCR;
    unsigned m_generation;
};

} // namespace id6term

#endif // ID6TERM_TERM_BUFFER_HPP
