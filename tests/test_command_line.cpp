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
    {   // shell quoting: one single-quoted word, embedded quotes survive
        CHECK_EQ_STR(shellQuote("ls -la /"), "'ls -la /'");
        CHECK_EQ_STR(shellQuote("echo 'a'"), "'echo '\\''a'\\'''");
        CHECK_EQ_STR(shellQuote(""), "''");
    }
    {   // the session init: the interrupt trap, the wrapper, cd to the start directory (quoted) with / as fallback
        std::string s = buildSessionInit("/fs/my dir");
        CHECK(s.find("trap '[ -n \"${__id6in-}\" ] && { __id6in=; return 130; }' INT TERM\n") == 0);
        CHECK(s.find("__id6run() {") != std::string::npos);
        CHECK(s.find("cd '/fs/my dir' 2>/dev/null || cd /\n") != std::string::npos);
        CHECK(buildSessionInit("").find("cd '/' 2>/dev/null") != std::string::npos);
        CHECK(s.find("printf") == std::string::npos);     // not a builtin of the head unit's sh
    }
    {   // one command line: the wrapper with the quoted command, then the sentinel with the exit status and $PWD
        std::string s = buildCommandLine("echo 'hi' | tr a-z A-Z", "__M__");
        CHECK_EQ_STR(s, "__id6run 'echo '\\''hi'\\'' | tr a-z A-Z'; __id6rc=$?; __id6in=; echo ''; "
                        "echo \"__M__ $__id6rc $PWD\"\n");
        CHECK(s.find("printf") == std::string::npos);
    }
    {   // sentinel lines: marker at the line start, a number, the directory (may contain spaces)
        RunResult r;
        CHECK(parseSentinelLine("__M__ 3 /opt/a b", "__M__", &r));
        CHECK(r.haveSentinel);
        CHECK_EQ(r.rc, 3);
        CHECK_EQ_STR(r.cwd, "/opt/a b");
        RunResult r2;
        CHECK(parseSentinelLine("__M__ 130 /\r", "__M__", &r2));
        CHECK_EQ(r2.rc, 130);
        CHECK_EQ_STR(r2.cwd, "/");
        RunResult no;
        CHECK(!parseSentinelLine("__M__ $__id6rc $PWD", "__M__", &no));     // set -v echoing the input
        CHECK(!parseSentinelLine("+ echo '__M__ 0 /'", "__M__", &no));      // set -x trace
        CHECK(!parseSentinelLine("__M__ 0", "__M__", &no));
        CHECK(!parseSentinelLine("__M__ 0 ", "__M__", &no));
        CHECK(!parseSentinelLine("__M__0 /", "__M__", &no));
        CHECK(!parseSentinelLine("__X__ 0 /", "__M__", &no));
        CHECK(!parseSentinelLine("", "__M__", &no));
        CHECK(!no.haveSentinel);
    }
    {   // letter case: the upper-case on-screen keyboard types lower case by default; invert swaps; asis keeps
        SpellerInput in;
        CHECK_EQ(in.letterCase(), kCaseLower);
        in.apply("L"); in.apply("S"); in.apply(" "); in.apply("-"); in.apply("l");
        CHECK_EQ_STR(in.text(), "ls -l");
        in.clear();
        in.setLetterCase(kCaseInvert);
        in.apply("L"); in.apply("s"); in.apply("1");
        CHECK_EQ_STR(in.text(), "lS1");
        in.clear();
        in.setLetterCase(kCaseAsIs);
        in.apply("L"); in.apply("s");
        CHECK_EQ_STR(in.text(), "Ls");
        in.clear();
        in.setLetterCase(kCaseLower);
        in.apply("Echo $PATH");                        // several characters at once: text, kept as it is
        CHECK_EQ_STR(in.text(), "Echo $PATH");
        LetterCase c = kCaseAsIs;
        CHECK(parseLetterCase("lower", &c));
        CHECK_EQ(c, kCaseLower);
        CHECK(parseLetterCase("invert", &c));
        CHECK_EQ(c, kCaseInvert);
        CHECK(parseLetterCase("asis", &c));
        CHECK_EQ(c, kCaseAsIs);
        CHECK(!parseLetterCase("UPPER", &c));
        CHECK_EQ_STR(letterCaseName(kCaseInvert), "invert");
    }
    {   // built-ins
        CHECK_EQ(classifyBuiltin("clear"), kBuiltinClear);
        CHECK_EQ(classifyBuiltin(" cls "), kBuiltinClear);
        CHECK_EQ(classifyBuiltin("help"), kBuiltinHelp);
        CHECK_EQ(classifyBuiltin("?"), kBuiltinHelp);
        CHECK_EQ(classifyBuiltin("history"), kBuiltinHistory);
        CHECK_EQ(classifyBuiltin("kbd"), kBuiltinKbd);
        CHECK_EQ(classifyBuiltin("kbd invert"), kBuiltinKbd);
        CHECK_EQ(classifyBuiltin("kbdx"), kNotBuiltin);
        CHECK_EQ(classifyBuiltin("clear screen"), kNotBuiltin);
        CHECK_EQ(classifyBuiltin("ls"), kNotBuiltin);
    }
    return testSummary("test_command_line");
}
