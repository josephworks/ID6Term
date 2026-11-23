// shell_runner.hpp - a shell session: one long-lived shell process that runs the command lines one at a time.
//
// The shell (`/bin/sh`, no -c) reads its commands from a pipe on its stdin and writes stdout + stderr into a second
// pipe. Every line goes in as buildCommandLine() (command_line.hpp): the command is eval'ed by the shell itself, so
// cd, variables, functions, aliases, `set` options and background jobs carry over to the next line - like a real
// terminal. The shell's sentinel line after the command marks its end, exit status and directory.
//
// A reader thread drains the output pipe (so the HMI thread never blocks on a chatty command), recognizes the
// sentinel, and enforces the timeout; the app thread calls pump() from its poll Runnable to move new bytes into the
// TermBuffer and to collect a finished command's result. Output of background jobs arriving later is pumped too.
//
// Stop / timeout: SIGINT to the shell's process group, SIGTERM 1 s later. The shell traps both (buildSessionInit),
// so the command dies and the session survives. If the command is still alive 3 s after the SIGINT, SIGKILL ends the
// whole group, shell included; the next command then starts a new session in the last known directory. The same
// happens after `exit` or `exec`.
//
// QNX: a multi-threaded process (HmiMain3D) must not fork(), so the shell is created with spawn() and an fd map.
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

    // Runs `command` in the session shell, first starting `shell` (e.g. /bin/sh) in `cwd` if there is no live
    // session. Returns false (and a reason in *err) if a command is still running or the shell could not be
    // started. *newSession (optional) tells whether a new shell was started for this command.
    bool start(const std::string& command, const std::string& cwd, const std::string& shell, int timeoutS,
               std::string* err, bool* newSession = 0);
    bool running() const;               // a command is in progress (start .. pump reports it finished)
    bool sessionAlive() const;          // the shell process is up
    // Asks the reader thread to interrupt the running command (see above). Safe from any thread.
    void stop();
    // Ends the session: closes the shell's stdin and kills its process group. Does not wait for the reader.
    void shutdown();

    // App thread only. Appends new output to buf. When the running command has ended, fills *result / *timedOut /
    // *killed and returns true exactly once for that command.
    bool pump(TermBuffer& buf, RunResult* result, bool* timedOut, bool* killed);

    const std::string& command() const { return m_command; }
    long long startedMs() const { return m_startMs; }
    const std::string& marker() const { return m_marker; }

    static long long nowMs();
    // Is this `NAME=value` entry of our own environment passed on to the shell? Drops what the fixed entries
    // override and the dynamic-linker injection (LD_PRELOAD etc.) that belongs to HmiMain3D alone.
    static bool inheritEnvEntry(const char* entry);

private:
    bool spawnShell(const std::string& shell, std::string* err);
    bool writeAll(const std::string& data);
    void reapSession();                 // app thread: join the finished reader, close the fds
    static void* threadMain(void* self);
    void readerLoop();
    void onOutput(const char* data, size_t n);  // reader thread: sentinel detection
    void emitLocked(const std::string& data);   // reader thread, m_mu held: queue for pump()
    void flushHeldLocked();
    void killGroup(int sig);

    mutable pthread_mutex_t m_mu;
    std::string m_marker;           // "__ID6TERM_<nonce>__", starts the sentinel line
    // session (pid / fds / thread are set on the app thread while no reader runs)
    pid_t m_pid;
    int m_outFd;                    // read end of the shell's stdout + stderr
    int m_inFd;                     // write end of the shell's stdin
    pthread_t m_thread;
    bool m_threadStarted;
    bool m_alive;                   // shell up (under m_mu; cleared by the reader at EOF)
    int m_exitStatus;               // raw waitpid status of the ended shell (under m_mu)
    // the command in progress
    bool m_running;                 // app thread's view
    bool m_cmdActive;               // reader's view (under m_mu)
    bool m_cmdDone;                 // sentinel seen or shell ended (under m_mu)
    RunResult m_result;             // (under m_mu)
    bool m_stopReq;                 // (under m_mu)
    bool m_timedOut;                // (under m_mu)
    bool m_killed;                  // (under m_mu)
    long long m_startMs;
    int m_timeoutS;
    std::string m_command;
    // output
    std::string m_pending;          // bytes not yet pumped (under m_mu)
    std::string m_hold;             // reader: the start of a line that may still turn out to be the sentinel
    bool m_atLineStart;             // reader: m_hold starts a line
    bool m_nlHeld;                  // reader: a '\n' not queued yet (dropped if the sentinel's empty line follows)
};

} // namespace id6term

#endif // ID6TERM_SHELL_RUNNER_HPP
