// test_command_line.cpp - SpellerInput, History (+ suggestions, persistence), the sh wrapper script and its sentinel.
#include "../src/command_line.hpp"
#include "test_util.hpp"
#include <unistd.h>

using namespace id6term;

int main() {
    {   // speller presses: letters append, "del" removes one, "delall" clears, no change -> false
        SpellerInput in;
        CHECK(in.apply("l"));
        CHECK(in.apply("s"));
        CHECK(in.apply(" -"));          // the speller may deliver several characters at once
        CHECK_EQ_STR(in.text(), "ls -");
        CHECK(in.apply("del"));
        CHECK_EQ_STR(in.text(), "ls ");
        CHECK(in.apply("delback"));
        CHECK_EQ_STR(in.text(), "ls");
        CHECK(in.apply("delall"));
        CHECK_EQ_STR(in.text(), "");
        CHECK(!in.apply("del"));
        CHECK(!in.apply("delall"));
        CHECK(!in.apply(""));
        CHECK(!in.apply(0));
    }
    {   // history: newest first, duplicates move up, trimmed, capped
        History h(3);
        h.add("  ls  ");
        h.add("pidin");
        h.add("ls");
        CHECK_EQ(h.entries().size(), 2);
        CHECK_EQ_STR(h.entries()[0], "ls");
        CHECK_EQ_STR(h.entries()[1], "pidin");
        CHECK_EQ_STR(h.last(), "ls");
        h.add("df");
        h.add("date");
        CHECK_EQ(h.entries().size(), 3);
        CHECK_EQ_STR(h.entries()[0], "date");
        CHECK_EQ_STR(h.entries()[2], "ls");      // date, df, ls: pidin fell off
        h.add("");
        h.add("   ");
        CHECK_EQ(h.entries().size(), 3);
    }
    {   // suggestions: history first, then extras, prefix match, never the prefix itself, de-duplicated, capped
        History h;
        h.add("ls /opt");
        h.add("ls");
        h.add("pidin");
        std::vector<std::string> quick;
        quick.push_back("ls -la /");
        quick.push_back("ls");
        quick.push_back("uname -a");
        std::vector<std::string> s = h.suggestions("ls", quick, 10);
        CHECK_EQ(s.size(), 2);
        CHECK_EQ_STR(s[0], "ls /opt");
        CHECK_EQ_STR(s[1], "ls -la /");
        s = h.suggestions("", quick, 4);
        CHECK_EQ(s.size(), 4);
        CHECK_EQ_STR(s[0], "pidin");
        CHECK_EQ_STR(s[3], "ls -la /");
        s = h.suggestions("zzz", quick, 10);
        CHECK(s.empty());
    }
    {   // save / load round trip keeps order and drops blanks / duplicates
        char path[] = "/tmp/id6term_hist_XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        close(fd);
        History h;
        h.add("a");
        h.add("b");
        CHECK(h.save(path));
        History h2;
        CHECK(h2.load(path));
        CHECK_EQ(h2.entries().size(), 2);
        CHECK_EQ_STR(h2.entries()[0], "b");
        CHECK_EQ_STR(h2.entries()[1], "a");
        FILE* f = fopen(path, "w");
        fprintf(f, "x\n\nx\n  y  \n");
        fclose(f);
        CHECK(h2.load(path));
        CHECK_EQ(h2.entries().size(), 2);
        CHECK_EQ_STR(h2.entries()[0], "x");
        CHECK_EQ_STR(h2.entries()[1], "y");
        unlink(path);
        CHECK(!h2.load("/nonexistent/dir/history"));
    }
    {   // the wrapper script: cd to $1, the command verbatim, then the sentinel with $PWD and the exit status
        std::string s = buildScript("echo hi | tr a-z A-Z");
        CHECK(s.find("cd \"$1\" 2>/dev/null\n") == 0);
        CHECK(s.find("\necho hi | tr a-z A-Z\n") != std::string::npos);
        CHECK(s.find("__id6rc=$?") != std::string::npos);
        CHECK(s.find("printf '\\n\\001%s\\002%s\\n' \"$PWD\" \"$__id6rc\"") != std::string::npos);
    }
    {   // sentinel parsing: strips it and the one newline printf put in front, reads cwd and rc
        std::string out = "hello\n\n\001/opt/aa\002" "3\n";   // "hello\n" from echo, "\n" from the sentinel printf
        RunResult r = parseSentinel(out);
        CHECK(r.haveSentinel);
        CHECK_EQ_STR(r.cwd, "/opt/aa");
        CHECK_EQ(r.rc, 3);
        CHECK_EQ_STR(out, "hello\n");

        out = "no newline before\001/\002" "0\n";       // command output without a trailing newline
        r = parseSentinel(out);
        CHECK(r.haveSentinel);
        CHECK_EQ_STR(out, "no newline before");
        CHECK_EQ(r.rc, 0);

        out = "\001fake\n\002" "x\n\001/real\002" "7\n";   // the command printed a \001 itself: the LAST one counts
        r = parseSentinel(out);
        CHECK(r.haveSentinel);
        CHECK_EQ_STR(r.cwd, "/real");
        CHECK_EQ(r.rc, 7);
        CHECK_EQ_STR(out, "\001fake\n\002" "x");

        out = "killed before the end";
        r = parseSentinel(out);
        CHECK(!r.haveSentinel);
        CHECK_EQ_STR(out, "killed before the end");

        out = "\001only start, no separator\n";
        r = parseSentinel(out);
        CHECK(!r.haveSentinel);
    }
    {   // built-ins
        CHECK_EQ(classifyBuiltin("clear"), kBuiltinClear);
        CHECK_EQ(classifyBuiltin(" cls "), kBuiltinClear);
        CHECK_EQ(classifyBuiltin("help"), kBuiltinHelp);
        CHECK_EQ(classifyBuiltin("?"), kBuiltinHelp);
        CHECK_EQ(classifyBuiltin("history"), kBuiltinHistory);
        CHECK_EQ(classifyBuiltin("clear screen"), kNotBuiltin);
        CHECK_EQ(classifyBuiltin("ls"), kNotBuiltin);
    }
    return testSummary("test_command_line");
}
