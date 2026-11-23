// command_line.hpp - what the terminal does with text, independent of the HMI and of QNX (host-testable):
//   * SpellerInput: the line being typed through the iDrive speller. The speller reports every press through the
//     input component's action, argument 8: one or more characters, or the control words "del" (backspace) and
//     "delall" (clear), as documented for the BMW Apps RHMI protocol and used by AAIdrive's InputState.
//   * History: the last N commands, newest first, persisted one per line; suggestions() returns the entries (and
//     the quick commands) that start with the current input, for the speller's suggestion list.
//   * buildCommandLine() wraps a command for the session shell (one long-lived `sh` reading its stdin, see
//     shell_runner.hpp) so the end of the command, its exit status and the shell's directory come back: after the
//     command the shell prints a sentinel line "<marker> <rc> <cwd>". parseSentinelLine() recognizes that line.
//     Only shell builtins are used (`eval`, `echo`): the head unit's /bin/sh has no `printf`.
#ifndef ID6TERM_COMMAND_LINE_HPP
#define ID6TERM_COMMAND_LINE_HPP

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace id6term {

// What the app does with the case of speller letters. The iDrive on-screen keyboard opens in upper case and there
// is no known RHMI property to open it in lower case, so by default capitals are folded to lower case.
enum LetterCase {
    kCaseLower,     // A-Z -> a-z (default; shell commands are lower case)
    kCaseInvert,    // swap: the upper-case keyboard types lower case, the keyboard's lower case types capitals
    kCaseAsIs       // keep what the keyboard sends
};

inline const char* letterCaseName(LetterCase c) {
    return c == kCaseInvert ? "invert" : c == kCaseAsIs ? "asis" : "lower";
}

inline bool parseLetterCase(const std::string& s, LetterCase* out) {
    if (s == "lower") { *out = kCaseLower; return true; }
    if (s == "invert") { *out = kCaseInvert; return true; }
    if (s == "asis" || s == "as-is" || s == "keep") { *out = kCaseAsIs; return true; }
    return false;
}

class SpellerInput {
public:
    SpellerInput() : m_case(kCaseLower) {}
    const std::string& text() const { return m_text; }
    void clear() { m_text.clear(); }
    void set(const std::string& s) { m_text = s; }
    LetterCase letterCase() const { return m_case; }
    void setLetterCase(LetterCase c) { m_case = c; }
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
        if (m_case == kCaseAsIs || letter[1]) {
            m_text += letter;                   // several characters at once: text, not a key press - keep it
        } else {
            char c = letter[0];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            else if (m_case == kCaseInvert && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            m_text += c;
        }
        return true;
    }
private:
    std::string m_text;
    LetterCase m_case;
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

// s as one single-quoted shell word ('it'\''s').
inline std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'') q += "'\\''";
        else q += s[i];
    }
    q += '\'';
    return q;
}

// Written once to a new session shell's stdin: start in `cwd` (or / if it is gone), and the wrapper that runs every
// command:
//   * A non-interactive shell exits on a syntax error in eval (pdksh / mksh even under `command`), which would end
//     the session. So the command is first parsed - not run - as the body of a throwaway function, in a subshell.
//     (`set -n` is no good: mksh runs the command anyway.)
//   * Then `command eval`: the command runs in the shell itself (so cd, variables, functions and aliases stay for the
//     next line), and `command` keeps a failing special builtin (shift, export, a redirection error) from exiting it.
//   * stdin is /dev/null: nothing here is interactive, and a command must not read the shell's own command stream.
//   * Stop and the timeout signal the whole process group with INT (then TERM): the command dies of it (a caught
//     signal is reset to the default in every command the shell starts - an ignored one, trap '', would not be), and
//     the trap's `return` abandons the rest of the line like Ctrl-C in a terminal, while the session shell survives.
//     The trap does nothing between commands.
//   * Positional parameters are shifted away so $1 etc. are empty in the command.
inline std::string buildSessionInit(const std::string& cwd) {
    // The trap clears the flag before returning: pdksh runs pending traps only after the foreground child has
    // exited, so INT and TERM can both be pending; the second must not `return` (= exit) at the top level.
    std::string s = "trap '[ -n \"${__id6in-}\" ] && { __id6in=; return 130; }' INT TERM\n";
    s += "__id6run() { __id6in=1; __id6c=$1; shift; (eval \"__id6chk() { :; $__id6c\n}\") </dev/null || return; "
         "command eval \"$__id6c\" </dev/null; }\n";
    s += "cd " + shellQuote(cwd.empty() ? std::string("/") : cwd) + " 2>/dev/null || cd /\n";
    return s;
}

// One command for the session shell, as one line: the wrapper above with the command as one quoted word. Then the
// sentinel: an empty line, so the sentinel starts a line even when the output did not end with '\n', and
// "<marker> <rc> <cwd>".
inline std::string buildCommandLine(const std::string& command, const std::string& marker) {
    std::string s = "__id6run " + shellQuote(command) + "; __id6rc=$?; __id6in=; echo ''; echo \"";
    s += marker;
    s += " $__id6rc $PWD\"\n";
    return s;
}

struct RunResult {
    bool haveSentinel;      // false when the shell died / was killed before printing it
    bool shellExited;       // the session shell is gone (exit, exec, killed): the next command gets a new one
    int rc;
    std::string cwd;
    RunResult() : haveSentinel(false), shellExited(false), rc(-1) {}
};

// Is `line` (without its '\n') the sentinel "<marker> <rc> <cwd>"? The rc must be a number, so the shell echoing
// its input (set -v) does not count; set -x traces start with "+ " and the marker must start the line.
inline bool parseSentinelLine(const std::string& line, const std::string& marker, RunResult* r) {
    if (marker.empty() || line.compare(0, marker.size(), marker) != 0) return false;
    size_t p = marker.size();
    if (p >= line.size() || line[p] != ' ') return false;
    ++p;
    size_t digits = p;
    if (p < line.size() && line[p] == '-') ++p;
    size_t numStart = p;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9') ++p;
    if (p == numStart || p >= line.size() || line[p] != ' ') return false;
    std::string cwd = line.substr(p + 1);
    while (!cwd.empty() && cwd[cwd.size() - 1] == '\r') cwd.erase(cwd.size() - 1);
    if (cwd.empty()) return false;
    r->rc = atoi(line.substr(digits, p - digits).c_str());
    r->cwd = cwd;
    r->haveSentinel = true;
    return true;
}

// Built-ins the app answers itself instead of the shell.
enum Builtin { kNotBuiltin, kBuiltinClear, kBuiltinHelp, kBuiltinHistory, kBuiltinKbd };

inline Builtin classifyBuiltin(const std::string& command) {
    std::string c = History::trimmed(command);
    if (c == "clear" || c == "cls") return kBuiltinClear;
    if (c == "help" || c == "?") return kBuiltinHelp;
    if (c == "history") return kBuiltinHistory;
    if (c == "kbd" || c.compare(0, 4, "kbd ") == 0) return kBuiltinKbd;
    return kNotBuiltin;
}

} // namespace id6term

#endif // ID6TERM_COMMAND_LINE_HPP
