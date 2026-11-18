// command_line.hpp - what the terminal does with text, independent of the HMI and of QNX (host-testable):
//   * SpellerInput: the line being typed through the iDrive speller. The speller reports every press through the
//     input component's action, argument 8: one or more characters, or the control words "del" (backspace) and
//     "delall" (clear), as documented for the BMW Apps RHMI protocol and used by AAIdrive's InputState.
//   * History: the last N commands, newest first, persisted one per line; suggestions() returns the entries (and
//     the quick commands) that start with the current input, for the speller's suggestion list.
//   * buildScript() wraps a command for `sh -c` so the working directory persists between commands (each run is its
//     own shell) and so the exit status and final directory come back: the shell prints a sentinel line
//     "\001<cwd>\002<rc>" after the command. parseSentinel() splits that off the captured output again.
#ifndef ID6TERM_COMMAND_LINE_HPP
#define ID6TERM_COMMAND_LINE_HPP

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace id6term {

const char kSentinelStart = '\001';
const char kSentinelSep   = '\002';

class SpellerInput {
public:
    SpellerInput() {}
    const std::string& text() const { return m_text; }
    void clear() { m_text.clear(); }
    void set(const std::string& s) { m_text = s; }
    // One speller press. Returns true when the text changed.
    bool apply(const char* letter) {
        if (!letter) return false;
        if (!strcmp(letter, "delall")) {
            if (m_text.empty()) return false;
            m_text.clear();
            return true;
        }
        if (!strcmp(letter, "del") || !strcmp(letter, "delback")) {
            if (m_text.empty()) return false;
            m_text.erase(m_text.size() - 1);
            return true;
        }
        if (!*letter) return false;
        m_text += letter;
        return true;
    }
private:
    std::string m_text;
};

class History {
public:
    explicit History(size_t max = 50) : m_max(max) {}

    const std::vector<std::string>& entries() const { return m_entries; }   // newest first
    bool empty() const { return m_entries.empty(); }
    const std::string& last() const { static const std::string none; return m_entries.empty() ? none : m_entries[0]; }

    // Adds cmd at the front; an existing identical entry moves up instead of duplicating.
    void add(const std::string& cmd) {
        std::string c = trimmed(cmd);
        if (c.empty()) return;
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i] == c) { m_entries.erase(m_entries.begin() + i); break; }
        m_entries.insert(m_entries.begin(), c);
        while (m_entries.size() > m_max) m_entries.pop_back();
    }

    // Suggestions for the speller list: history entries, then quick commands, that start with `prefix`
    // (case-sensitive, like a shell), never the prefix itself, at most `max`.
    std::vector<std::string> suggestions(const std::string& prefix, const std::vector<std::string>& extra, size_t max) const {
        std::vector<std::string> out;
        addMatches(out, m_entries, prefix, max);
        addMatches(out, extra, prefix, max);
        return out;
    }

    // One command per line, newest first (same order as entries()).
    bool load(const char* path) {
        FILE* f = fopen(path, "r");
        if (!f) return false;
        std::vector<std::string> lines;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string l = trimmed(line);
            if (!l.empty()) lines.push_back(l);
        }
        fclose(f);
        m_entries.clear();
        for (size_t i = 0; i < lines.size() && m_entries.size() < m_max; ++i) {
            bool dup = false;
            for (size_t j = 0; j < m_entries.size(); ++j) if (m_entries[j] == lines[i]) { dup = true; break; }
            if (!dup) m_entries.push_back(lines[i]);
        }
        return true;
    }

    bool save(const char* path) const {
        FILE* f = fopen(path, "w");
        if (!f) return false;
        for (size_t i = 0; i < m_entries.size(); ++i) fprintf(f, "%s\n", m_entries[i].c_str());
        fclose(f);
        return true;
    }

    static std::string trimmed(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
        return s.substr(b, e - b);
    }

private:
    static void addMatches(std::vector<std::string>& out, const std::vector<std::string>& from,
                           const std::string& prefix, size_t max) {
        for (size_t i = 0; i < from.size() && out.size() < max; ++i) {
            const std::string& c = from[i];
            if (c.size() <= prefix.size() || c.compare(0, prefix.size(), prefix) != 0) continue;
            bool dup = false;
            for (size_t j = 0; j < out.size(); ++j) if (out[j] == c) { dup = true; break; }
            if (!dup) out.push_back(c);
        }
    }

    size_t m_max;
    std::vector<std::string> m_entries;
};

// The script handed to `sh -c SCRIPT sh CWD`: $1 is the directory to start in (so no quoting of the path is
// needed inside the script), the command runs as typed (a full shell line: pipes, &&, cd, variables all work),
// then the sentinel reports the exit status and the directory the command left the shell in.
inline std::string buildScript(const std::string& command) {
    std::string s;
    s += "cd \"$1\" 2>/dev/null\n";
    s += command;
    s += "\n__id6rc=$?\n";
    s += "printf '\\n\\001%s\\002%s\\n' \"$PWD\" \"$__id6rc\"\n";
    return s;
}

struct RunResult {
    bool haveSentinel;      // false when the process died / was killed before printing it
    int rc;
    std::string cwd;
    RunResult() : haveSentinel(false), rc(-1) {}
};

// Splits the last sentinel off `output` (in place: the sentinel and the '\n' printed before it are removed).
inline RunResult parseSentinel(std::string& output) {
    RunResult r;
    size_t p = output.rfind(kSentinelStart);
    if (p == std::string::npos) return r;
    std::string tail = output.substr(p + 1);
    size_t sep = tail.find(kSentinelSep);
    if (sep == std::string::npos) return r;
    r.cwd = tail.substr(0, sep);
    std::string rcs = tail.substr(sep + 1);
    size_t e = rcs.find_first_of("\r\n");
    if (e != std::string::npos) rcs.erase(e);
    r.rc = atoi(rcs.c_str());
    r.haveSentinel = true;
    size_t cut = p;
    if (cut > 0 && output[cut - 1] == '\n') --cut;     // the "\n" printf put in front of the sentinel
    output.erase(cut);
    return r;
}

// Built-ins the app answers itself instead of the shell.
enum Builtin { kNotBuiltin, kBuiltinClear, kBuiltinHelp, kBuiltinHistory };

inline Builtin classifyBuiltin(const std::string& command) {
    std::string c = History::trimmed(command);
    if (c == "clear" || c == "cls") return kBuiltinClear;
    if (c == "help" || c == "?") return kBuiltinHelp;
    if (c == "history") return kBuiltinHistory;
    return kNotBuiltin;
}

} // namespace id6term

#endif // ID6TERM_COMMAND_LINE_HPP
