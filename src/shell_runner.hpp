// shell_runner.hpp - runs one shell command line at a time and streams its output.
//
// Each command is its own `sh -c SCRIPT sh CWD` process (see command_line.hpp buildScript): stdin from /dev/null,
// stdout + stderr into a pipe. A reader thread drains the pipe (so the HMI thread never blocks on a chatty
// command) and enforces the timeout; the app thread calls pump() from its poll Runnable to move the new bytes into
// the TermBuffer and, once the process has exited, to collect the exit status and the directory it ended in.
//
// QNX: a multi-threaded process (HmiMain3D) must not fork(), so the child is created with spawn() and an fd map.
// Host (tests): posix_spawn.
#ifndef ID6TERM_SHELL_RUNNER_HPP
#define ID6TERM_SHELL_RUNNER_HPP

#include <pthread.h>
#include <sys/types.h>
#include <string>
#include "term_buffer.hpp"
#include "command_line.hpp"

namespace id6term {

class ShellRunner {
public:
    ShellRunner();
    ~ShellRunner();

    // Starts `command` in `shell` (e.g. /bin/sh) with the working directory `cwd`. Returns false (and a reason in
    // *err) if a command is still running or the spawn failed.
    bool start(const std::string& command, const std::string& cwd, const std::string& shell, int timeoutS,
               std::string* err);
    bool running() const;
    // Asks the reader thread to kill the command (SIGTERM, then SIGKILL after 2 s). Safe from any thread.
    void stop();

    // App thread only. Appends new output to buf. When the command has ended, fills *result, sets *finished and
    // returns true exactly once for that run.
    bool pump(TermBuffer& buf, RunResult* result, bool* timedOut, bool* killed);

    const std::string& command() const { return m_command; }
    long long startedMs() const { return m_startMs; }

    static long long nowMs();
    // Is this `NAME=value` entry of our own environment passed on to the command? Drops what the fixed entries
    // override and the dynamic-linker injection (LD_PRELOAD etc.) that belongs to HmiMain3D alone.
    static bool inheritEnvEntry(const char* entry);

private:
    static void* threadMain(void* self);
    void readerLoop();
    void emit(TermBuffer& buf, const std::string& data);
    void joinThread();

    mutable pthread_mutex_t m_mu;
    std::string m_pending;          // bytes read, not yet pumped (under m_mu)
    std::string m_hold;             // from the last '\001' on: a sentinel candidate, shown only if it isn't one
    bool m_running;                 // a run is in progress (start .. pump reports finished)
    bool m_done;                    // reader thread finished (under m_mu)
    bool m_stopReq;                 // stop() was called (under m_mu)
    bool m_timedOut;
    bool m_killed;
    int m_exitStatus;               // raw waitpid status
    pid_t m_pid;
    int m_fd;                       // pipe read end
    pthread_t m_thread;
    bool m_threadStarted;
    long long m_startMs;
    int m_timeoutS;
    std::string m_command;
};

} // namespace id6term

#endif // ID6TERM_SHELL_RUNNER_HPP
