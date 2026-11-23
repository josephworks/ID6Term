// test_shell_runner.cpp - ShellRunner against the host /bin/sh: one session for all commands (cd, variables and
// functions persist), streamed output, exit status, stderr, timeout / stop interrupting only the command, escalation
// to SIGKILL, `exit` ending the session, background output after the command, busy refusal.
#include "../src/shell_runner.hpp"
#include "test_util.hpp"
#include <stdlib.h>
#include <unistd.h>

using namespace id6term;

// The shell under test: /bin/sh, or e.g. ID6TERM_TEST_SHELL=/usr/bin/mksh (pdksh lineage, like the head unit's sh).
static const char* const kShellEnv = getenv("ID6TERM_TEST_SHELL");
static const std::string kShell = kShellEnv && *kShellEnv ? kShellEnv : "/bin/sh";
// Pumps until the run ends or `maxMs` passed; returns true when it ended.
static bool runToEnd(ShellRunner& r, TermBuffer& buf, RunResult* res, bool* timedOut, bool* killed, int maxMs) {
    long long start = ShellRunner::nowMs();
    while (ShellRunner::nowMs() - start < maxMs) {
        if (r.pump(buf, res, timedOut, killed)) return true;
        usleep(20000);
    }
    return false;
}

// Runs one command to its end and returns its output (the buffer is cleared first).
static std::string run(ShellRunner& r, const std::string& cmd, RunResult* res, bool* newSession = 0) {
    TermBuffer buf;
    std::string err, out;
    bool to = false, k = false;
    if (!r.start(cmd, "/", kShell, 10, &err, newSession)) return "start failed: " + err;
    if (!runToEnd(r, buf, res, &to, &k, 5000)) return "did not end";
    buf.render(out);
    return out;
}

int main() {
    std::string out, err;
    {   // stdout + stderr, exit status, sentinel stripped
        ShellRunner r;
        TermBuffer buf;
        bool fresh = false;
        CHECK(r.start("echo out; echo err 1>&2; (exit 3)", "/", kShell, 10, &err, &fresh));
        CHECK(fresh);
        CHECK(r.running());
        RunResult res; bool to = false, k = false;
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        CHECK(!r.running());
        CHECK(res.haveSentinel);
        CHECK(!res.shellExited);
        CHECK_EQ(res.rc, 3);
        CHECK(!to);
        CHECK(!k);
        buf.render(out);
        CHECK_EQ_STR(out, "out\nerr");
        CHECK_EQ_STR(res.cwd, "/");
        CHECK(r.sessionAlive());
    }
    {   // one session: cd, variables, functions and options carry over to the next command (the car bug: `cd /mnt`
        // then `ls` listed /). The cwd argument only matters when a new session starts.
        ShellRunner r;
        RunResult res;
        bool fresh = false;
        CHECK_EQ_STR(run(r, "cd /tmp", &res, &fresh), "");
        CHECK(fresh);
        CHECK_EQ_STR(res.cwd, "/tmp");
        CHECK_EQ_STR(run(r, "pwd", &res, &fresh), "/tmp");
        CHECK(!fresh);
        CHECK_EQ_STR(res.cwd, "/tmp");
        CHECK_EQ_STR(run(r, "X=hello; greet() { echo \"hi $1\"; }", &res), "");
        CHECK_EQ_STR(run(r, "echo $X; greet there", &res), "hello\nhi there");
        CHECK_EQ_STR(run(r, "cd ..; ls -d \"$PWD\"", &res), "/");
        CHECK_EQ_STR(res.cwd, "/");
        CHECK_EQ_STR(run(r, "echo \"[$1][$#]\"", &res), "[][0]");     // the wrapper's own arguments are not visible
        CHECK_EQ_STR(run(r, "echo \"it's\" 'quoted' \"$X\"", &res), "it's quoted hello");
    }
    {   // errors are just errors: a syntax error, a failing special builtin, a redirection error - the session goes on
        ShellRunner r;
        RunResult res;
        run(r, "Y=1", &res);
        out = run(r, "echo \"unterminated", &res);
        CHECK(res.haveSentinel);
        CHECK(res.rc != 0);
        CHECK(!out.empty());
        run(r, "if then", &res);
        CHECK(res.haveSentinel);
        CHECK(res.rc != 0);
        run(r, "shift 5", &res);
        CHECK(res.haveSentinel);
        run(r, "echo x >/nonexistent/dir/x", &res);
        CHECK(res.haveSentinel);
        CHECK(res.rc != 0);
        CHECK_EQ_STR(run(r, "# only a comment", &res), "");
        CHECK(res.haveSentinel);
        CHECK_EQ_STR(run(r, "Y=$((Y+1)); echo $Y", &res), "2");     // ran exactly once, despite the syntax check
        CHECK_EQ_STR(run(r, "echo $Y }", &res), "2 }");
    }
    {   // `exit N` ends the session: no sentinel, status from waitpid; the next command gets a new shell in `cwd`
        ShellRunner r;
        RunResult res;
        CHECK_EQ_STR(run(r, "Z=gone; echo bye; exit 5", &res), "bye");
        CHECK(!res.haveSentinel);
        CHECK(res.shellExited);
        CHECK_EQ(res.rc, 5);
        TermBuffer buf;
        bool fresh = false, to, k;
        CHECK(r.start("echo \"[$Z]\"; pwd", "/tmp", kShell, 10, &err, &fresh));
        CHECK(fresh);
        CHECK(runToEnd(r, buf, &res, &to, &k, 5000));
        buf.render(out);
        CHECK_EQ_STR(out, "[]\n/tmp");
    }
    {   // output is streamed while the command runs; a partial line shows before its '\n'; lines that merely look
        // like the sentinel (set -x traces, wrong format) are output
        ShellRunner r;
        TermBuffer buf;
        std::string cmd = "echo first; echo -n part; sleep 1; echo ial; echo '" + r.marker() + " x /'; echo second";
        CHECK(r.start(cmd, "/", kShell, 10, &err));
        long long start = ShellRunner::nowMs();
        bool sawEarly = false;
        RunResult res; bool to, k;
        bool ended = false;
        while (ShellRunner::nowMs() - start < 5000) {
            if (r.pump(buf, &res, &to, &k)) { ended = true; break; }
            buf.render(out);
            if (out == "first\npart" && ShellRunner::nowMs() - start < 800) sawEarly = true;
            usleep(20000);
        }
        CHECK(ended);
        CHECK(sawEarly);
        buf.render(out);
        CHECK_EQ_STR(out, "first\npartial\n" + r.marker() + " x /\nsecond");
        CHECK(res.haveSentinel);
        CHECK_EQ(res.rc, 0);

        out = run(r, "set -x; echo traced", &res);
        CHECK(res.haveSentinel);
        CHECK_EQ(res.rc, 0);
        CHECK(out.find("traced") != std::string::npos);
        run(r, "set +x", &res);
        CHECK_EQ_STR(run(r, "echo clean", &res), "clean");
    }
    {   // timeout: only the command is interrupted (SIGINT), the rest of its line is dropped, the session survives
        ShellRunner r;
        TermBuffer buf;
        RunResult res;
        run(r, "KEEP=yes; cd /tmp", &res);
        CHECK(r.start("echo start; sleep 30; echo never", "/", kShell, 1, &err));
        bool to = false, k = false;
        long long t0 = ShellRunner::nowMs();
        CHECK(runToEnd(r, buf, &res, &to, &k, 8000));
        CHECK(ShellRunner::nowMs() - t0 < 3000);
        CHECK(to);
        CHECK(k);
        CHECK(res.haveSentinel);
        CHECK(!res.shellExited);
        CHECK_EQ(res.rc, 130);
        buf.render(out);
        CHECK_EQ_STR(out, "start");
        CHECK(r.sessionAlive());
        CHECK_EQ_STR(run(r, "echo $KEEP; pwd", &res), "yes\n/tmp");
    }
    {   // stop(): same, on request; a busy loop inside the shell itself is interrupted too
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("echo a; sleep 30; echo b", "/", kShell, 60, &err));
        RunResult res; bool to = false, k = false;
        usleep(300000);
        CHECK(!r.start("echo busy", "/", kShell, 10, &err));   // refused while running
        CHECK(err.find("still running") != std::string::npos);
        r.stop();
        CHECK(runToEnd(r, buf, &res, &to, &k, 8000));
        CHECK(k);
        CHECK(!to);
        CHECK(res.haveSentinel);
        CHECK_EQ(res.rc, 130);
        buf.render(out);
        CHECK_EQ_STR(out, "a");

        buf.clear();
        CHECK(r.start("while :; do :; done; echo never", "/", kShell, 60, &err));
        usleep(300000);
        r.stop();
        long long t0 = ShellRunner::nowMs();
        CHECK(runToEnd(r, buf, &res, &to, &k, 8000));
        CHECK(ShellRunner::nowMs() - t0 < 3000);
        CHECK(k);
        CHECK(!res.shellExited);
        buf.render(out);
        CHECK_EQ_STR(out, "");
        CHECK_EQ_STR(run(r, "echo still here", &res), "still here");
    }
    {   // a command that ignores INT but dies of the TERM 1 s later: with both traps pending (pdksh runs them only
        // after the child exited) the session must survive
        ShellRunner r;
        TermBuffer buf;
        RunResult res;
        run(r, "KEEP2=ok", &res);
        CHECK(r.start("sh -c \"trap '' INT; sleep 30\"; echo after", "/", kShell, 60, &err));
        usleep(300000);
        r.stop();
        bool to = false, k = false;
        CHECK(runToEnd(r, buf, &res, &to, &k, 8000));
        CHECK(k);
        CHECK(res.haveSentinel);
        CHECK(!res.shellExited);
        buf.render(out);
        CHECK(out.find("after") == std::string::npos);
        bool fresh = true;
        CHECK_EQ_STR(run(r, "echo $KEEP2", &res, &fresh), "ok");
        CHECK(!fresh);
    }
    {   // a command that ignores INT and TERM: SIGKILL for the whole group after 3 s, then a new session
        ShellRunner r;
        TermBuffer buf;
        CHECK(r.start("sh -c \"trap '' INT TERM; sleep 30\"", "/", kShell, 60, &err));
        usleep(300000);
        r.stop();
        RunResult res; bool to = false, k = false;
        long long t0 = ShellRunner::nowMs();
        CHECK(runToEnd(r, buf, &res, &to, &k, 10000));
        long long took = ShellRunner::nowMs() - t0;
        CHECK(took >= 2500 && took < 6000);
        CHECK(k);
        CHECK(!res.haveSentinel);
        CHECK(res.shellExited);
        bool fresh = false;
        CHECK_EQ_STR(run(r, "echo again", &res, &fresh), "again");
        CHECK(fresh);
    }
    {   // a background job keeps running after its command ended; its later output is pumped while idle
        ShellRunner r;
        RunResult res;
        CHECK_EQ_STR(run(r, "(sleep 1; echo late) &", &res), "");
        CHECK(!r.running());
        TermBuffer buf;
        bool to, k;
        long long t0 = ShellRunner::nowMs();
        while (ShellRunner::nowMs() - t0 < 3000) {
            CHECK(!r.pump(buf, &res, &to, &k));           // nothing "finishes" while idle
            buf.render(out);
            if (out == "late") break;
            usleep(20000);
        }
        CHECK_EQ_STR(out, "late");
    }
    {   // destroying the runner with a background job running does not hang
        long long t0 = ShellRunner::nowMs();
        {
            ShellRunner r;
            RunResult res;
            run(r, "sleep 30 &", &res);
        }
        CHECK(ShellRunner::nowMs() - t0 < 3000);
    }
    {   // a shell that does not exist fails at start, nothing is left running
        ShellRunner r;
        CHECK(!r.start("echo x", "/", "/nonexistent/sh", 10, &err));
        CHECK(!r.running());
        CHECK(!r.sessionAlive());
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
    {   // ...and the shell really does not see them (what printed "unknown symbol: kdMain" on the car)
        setenv("LD_PRELOAD", "", 1);          // empty: harmless for the test binary itself, but present in environ
        setenv("ID6TERM_TEST_PASS", "yes", 1);
        ShellRunner r;
        RunResult res;
        out = run(r, "echo \"[${LD_PRELOAD+set}] [$ID6TERM_TEST_PASS] [$ID6TERM] [$TERM]\"", &res);
        unsetenv("LD_PRELOAD");
        CHECK_EQ_STR(out, "[] [yes] [1] [dumb]");
    }
    return testSummary("test_shell_runner");
}
