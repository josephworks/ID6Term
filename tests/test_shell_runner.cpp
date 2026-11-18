// test_shell_runner.cpp - ShellRunner against the host /bin/sh: streamed output, exit status, cwd tracking, stderr,
// timeout kill (including a backgrounded child holding the pipe), stop(), busy refusal, back-to-back runs.
#include "../src/shell_runner.hpp"
#include "test_util.hpp"
#include <stdlib.h>
#include <unistd.h>

using namespace id6term;

// Pumps until the run ends or `maxMs` passed; returns true when it ended.
static bool runToEnd(ShellRunner& r, TermBuffer& buf, RunResult* res, bool* timedOut, bool* killed, int maxMs) {
    long long start = ShellRunner::nowMs();
    while (ShellRunner::nowMs() - start < maxMs) {
        if (r.pump(buf, res, timedOut, killed)) return true;
        usleep(20000);
    }
    return false;
}

int main() {
    std::string out, err;
    {   // stdout + stderr, exit status, sentinel stripped
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo out; echo err 1>&2; (exit 3)", "/", "/bin/sh", 10, &err));
        CHECK(r.running());
        RunResult res; bool to = false, k = false;
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        CHECK(!r.running());
        CHECK(res.haveSentinel);
        CHECK_EQ(res.rc, 3);
        CHECK(!to);
        CHECK(!k);
        buf.render(out);
        CHECK_EQ_STR(out, "out\nerr");
        CHECK_EQ_STR(res.cwd, "/");
    }
    {   // `exit N` ends the shell before the sentinel: the status still comes back, from waitpid
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo bye; exit 5", "/", "/bin/sh", 10, &err));
        RunResult res; bool to = false, k = false;
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        CHECK(!res.haveSentinel);
        CHECK_EQ(res.rc, 5);
        CHECK(!k);
        buf.render(out);
        CHECK_EQ_STR(out, "bye");
    }
    {   // cd persists: the sentinel reports the directory the command ended in, which the next run starts from
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("cd /tmp && pwd", "/", "/bin/sh", 10, &err));
        RunResult res; bool to, k;
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        CHECK_EQ_STR(res.cwd, "/tmp");
        buf.render(out);
        CHECK_EQ_STR(out, "/tmp");
        buf.clear();
        CHECK(r.start("pwd", res.cwd, "/bin/sh", 10, &err));
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        buf.render(out);
        CHECK_EQ_STR(out, "/tmp");
        CHECK_EQ(res.rc, 0);
    }
    {   // output is streamed while the command runs, and a command may print \001 without confusing the sentinel
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo first; printf 'x\\001y\\n'; sleep 1; echo second", "/", "/bin/sh", 10, &err));
        long long start = ShellRunner::nowMs();
        bool sawFirstEarly = false;
        RunResult res; bool to, k;
        bool ended = false;
        while (ShellRunner::nowMs() - start < 5000) {
            if (r.pump(buf, &res, &to, &k)) { ended = true; break; }
            buf.render(out);
            if (out.find("first") != std::string::npos && ShellRunner::nowMs() - start < 800) sawFirstEarly = true;
            usleep(20000);
        }
        CHECK(ended);
        CHECK(sawFirstEarly);
        buf.render(out);
        CHECK_EQ_STR(out, "first\nxy\nsecond");       // \001 is a control char the buffer drops
        CHECK(res.haveSentinel);
    }
    {   // timeout: killed, no sentinel, flagged; a backgrounded child that keeps the pipe open goes with it
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo start; sleep 30 & sleep 30; echo never", "/", "/bin/sh", 1, &err));
        RunResult res; bool to = false, k = false;
        long long t0 = ShellRunner::nowMs();
        CHECK(runToEnd(r, buf, &res, &to, &k, 8000));
        long long took = ShellRunner::nowMs() - t0;
        CHECK(to);
        CHECK(k);
        CHECK(!res.haveSentinel);
        CHECK(took < 5000);
        buf.render(out);
        CHECK_EQ_STR(out, "start");
    }
    {   // stop(): same, on request
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo a; sleep 30; echo b", "/", "/bin/sh", 60, &err));
        RunResult res; bool to = false, k = false;
        usleep(300000);
        CHECK(!r.start("echo busy", "/", "/bin/sh", 10, &err));   // refused while running
        CHECK(err.find("still running") != std::string::npos);
        r.stop();
        CHECK(runToEnd(r, buf, &res, &to, &k, 8000));
        CHECK(k);
        CHECK(!to);
        CHECK(!res.haveSentinel);
        buf.render(out);
        CHECK_EQ_STR(out, "a");
    }
    {   // a shell that does not exist fails at start, nothing is left running
        ShellRunner r;
        CHECK(!r.start("echo x", "/", "/nonexistent/sh", 10, &err));
        CHECK(!r.running());
        CHECK(!err.empty());
    }
    {   // environment filter: loader injection and the overridden names stay out, everything else passes
        CHECK(!ShellRunner::inheritEnvEntry("LD_PRELOAD=libKD.so.1"));
        CHECK(!ShellRunner::inheritEnvEntry("LD_LIBRARY_PATH=/opt/hmi/ID5/lib"));
        CHECK(!ShellRunner::inheritEnvEntry("LD_DEBUG=all"));
        CHECK(!ShellRunner::inheritEnvEntry("LD_BIND_NOW=1"));
        CHECK(!ShellRunner::inheritEnvEntry("PATH=/x"));
        CHECK(!ShellRunner::inheritEnvEntry("HOME=/x"));
        CHECK(!ShellRunner::inheritEnvEntry("TERM=xterm"));
        CHECK(!ShellRunner::inheritEnvEntry("ID6TERM=0"));
        CHECK(!ShellRunner::inheritEnvEntry(""));
        CHECK(!ShellRunner::inheritEnvEntry(0));
        CHECK(ShellRunner::inheritEnvEntry("TZ=UTC"));
        CHECK(ShellRunner::inheritEnvEntry("LDAP_X=1"));        // only the loader's own names, not every LD* prefix
        CHECK(ShellRunner::inheritEnvEntry("HOMEDIR=/x"));
    }
    {   // ...and the child really does not see them (what printed "unknown symbol: kdMain" on the car)
        setenv("LD_PRELOAD", "", 1);          // empty: harmless for the test binary itself, but present in environ
        setenv("ID6TERM_TEST_PASS", "yes", 1);
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo \"[${LD_PRELOAD+set}] [$ID6TERM_TEST_PASS] [$ID6TERM] [$TERM]\"", "/", "/bin/sh", 10, &err));
        RunResult res; bool to, k;
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        unsetenv("LD_PRELOAD");
        buf.render(out);
        CHECK_EQ_STR(out, "[] [yes] [1] [dumb]");
    }
    return testSummary("test_shell_runner");
}
