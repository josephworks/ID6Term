// shell_runner.cpp - see shell_runner.hpp
#include "shell_runner.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <spawn.h>

extern char** environ;

namespace id6term {

namespace {

const int kSelectMs = 200;
const int kTermAfterMs = 1000;          // after the SIGINT: SIGTERM ...
const int kKillAfterMs = 3000;          // ... and SIGKILL for the whole group (the session ends)
const int kReapGraceMs = 2000;          // EOF but the shell has not exited yet
const int kDrainMs = 500;               // shell gone, background jobs still hold the pipe: stop listening
const size_t kHoldMaxBytes = 4096;      // a "sentinel candidate" longer than this is just output

// Environment for the shell: a sane PATH for the head unit (the HMI's own may be minimal), HOME, TERM, plus
// whatever the parent has that passes ShellRunner::inheritEnvEntry().
char** buildEnv(std::string& storage) {
    storage.clear();
    const char* fixed[] = {
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/os/bin:/opt/sys/bin:/opt/core/bin:/opt/car/bin:/fs/sda0/opt/mm/bin:/opt/hmi/ID5/bin",
        "HOME=/",
        "TERM=dumb",
        "ID6TERM=1",
        0
    };
    size_t count = 0;
    for (const char** p = fixed; *p; ++p) { storage.append(*p); storage += '\0'; ++count; }
    for (char** e = environ; e && *e; ++e) {
        if (!ShellRunner::inheritEnvEntry(*e)) continue;
        storage.append(*e); storage += '\0'; ++count;
    }
    char** envp = new char*[count + 1];
    size_t i = 0;
    for (size_t off = 0; off < storage.size() && i < count; ++i) {
        envp[i] = &storage[off];
        off += strlen(&storage[off]) + 1;
    }
    envp[i] = 0;
    return envp;
}

int rcFromStatus(int status) {
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

} // namespace

bool ShellRunner::inheritEnvEntry(const char* entry) {
    if (!entry || !*entry) return false;
    // Overridden by the fixed entries above.
    if (!strncmp(entry, "PATH=", 5) || !strncmp(entry, "HOME=", 5) || !strncmp(entry, "TERM=", 5) ||
        !strncmp(entry, "ID6TERM=", 8)) return false;
    // Dynamic-linker injection meant for HmiMain3D only. The HMI is an OpenKODE application started with
    // LD_PRELOAD=libKD.so.1 (and friends); inherited by /bin/sh and every command, the loader printed
    // "unknown symbol: kdMain referenced from libKD.so.1" into the console at each run (car, 2026-11-14), since
    // only the HMI executable defines kdMain. Commands get the system's plain library search instead.
    if (!strncmp(entry, "LD_PRELOAD=", 11) || !strncmp(entry, "LD_LIBRARY_PATH=", 16) ||
        !strncmp(entry, "LD_DEBUG", 8) || !strncmp(entry, "LD_BIND_NOW=", 12)) return false;
    return true;
}

long long ShellRunner::nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ShellRunner::ShellRunner()
    : m_pid(-1), m_outFd(-1), m_inFd(-1), m_threadStarted(false), m_alive(false), m_exitStatus(0),
      m_running(false), m_cmdActive(false), m_cmdDone(false), m_stopReq(false), m_timedOut(false), m_killed(false),
      m_startMs(0), m_timeoutS(20), m_atLineStart(true), m_nlHeld(false) {
    pthread_mutex_init(&m_mu, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char mk[48];
    snprintf(mk, sizeof(mk), "__ID6TERM_%08lx%05x__", (unsigned long)(ts.tv_nsec ^ (long)(size_t)this),
             (unsigned)(getpid() & 0xfffff));
    m_marker = mk;
}

ShellRunner::~ShellRunner() {
    shutdown();
    if (m_pid > 0) kill(m_pid, SIGKILL);
    reapSession();
    pthread_mutex_destroy(&m_mu);
}

bool ShellRunner::running() const {
    return m_running;
}

bool ShellRunner::sessionAlive() const {
    pthread_mutex_lock(&m_mu);
    bool a = m_alive;
    pthread_mutex_unlock(&m_mu);
    return a;
}

void ShellRunner::killGroup(int sig) {
    if (m_pid <= 0) return;
    kill(-m_pid, sig);
    kill(m_pid, sig);
}

bool ShellRunner::spawnShell(const std::string& shell, std::string* err) {
    int outp[2], inp[2];
    if (pipe(outp) != 0) { if (err) *err = std::string("pipe: ") + strerror(errno); return false; }
    if (pipe(inp) != 0) {
        if (err) *err = std::string("pipe: ") + strerror(errno);
        close(outp[0]); close(outp[1]);
        return false;
    }
    fcntl(outp[0], F_SETFD, FD_CLOEXEC);            // our ends: not for the shell or anything it starts
    fcntl(inp[1], F_SETFD, FD_CLOEXEC);

    const char* argv[] = { "sh", 0 };
    std::string envStorage;
    char** envp = buildEnv(envStorage);

    pid_t pid = -1;
#ifdef __QNX__
    int fdmap[3] = { inp[0], outp[1], outp[1] };
    struct inheritance inh;
    memset(&inh, 0, sizeof(inh));
    inh.flags = SPAWN_SETGROUP | SPAWN_SETSIGDEF | SPAWN_SETSIGMASK;
    inh.pgroup = SPAWN_NEWPGROUP;
    sigfillset(&inh.sigdefault);
    sigemptyset(&inh.sigmask);
    pid = spawn(shell.c_str(), 3, fdmap, &inh, const_cast<char* const*>(argv), envp);
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t def, mask;
    sigfillset(&def);
    sigdelset(&def, SIGKILL);
    sigdelset(&def, SIGSTOP);
    sigemptyset(&mask);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setsigmask(&attr, &mask);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setpgroup(&attr, 0);
    int prc = posix_spawn(&pid, shell.c_str(), &fa, &attr, const_cast<char* const*>(argv), envp);
    if (prc != 0) { pid = -1; errno = prc; }
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
#endif
    int spawnErrno = errno;
    delete[] envp;
    close(inp[0]);
    close(outp[1]);
    if (pid <= 0) {
        if (err) *err = std::string("spawn ") + shell + ": " + strerror(spawnErrno);
        close(outp[0]);
        close(inp[1]);
        return false;
    }

    m_pid = pid;
    m_outFd = outp[0];
    m_inFd = inp[1];
    m_hold.clear();
    m_atLineStart = true;
    m_nlHeld = false;
    pthread_mutex_lock(&m_mu);
    m_pending.clear();
    m_alive = true;
    m_exitStatus = 0;
    m_cmdActive = false;
    m_cmdDone = false;
    pthread_mutex_unlock(&m_mu);

    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setstacksize(&ta, 64 * 1024);
    int rc = pthread_create(&m_thread, &ta, &threadMain, this);
    pthread_attr_destroy(&ta);
    if (rc != 0) {
        if (err) *err = std::string("pthread_create: ") + strerror(rc);
        killGroup(SIGKILL);
        int st;
        waitpid(pid, &st, 0);
        close(m_outFd);
        close(m_inFd);
        m_outFd = m_inFd = -1;
        m_pid = -1;
        pthread_mutex_lock(&m_mu);
        m_alive = false;
        pthread_mutex_unlock(&m_mu);
        return false;
    }
    m_threadStarted = true;
    return true;
}

// Writing into a pipe whose reader (the shell) is gone raises SIGPIPE, which would kill HmiMain3D. The signal is
// blocked for this thread during the write and, if the write caused one, consumed before unblocking.
bool ShellRunner::writeAll(const std::string& data) {
    if (m_inFd < 0) return false;
    sigset_t pipeSet, oldMask, pend;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &oldMask);
    sigemptyset(&pend);
    sigpending(&pend);
    bool wasPending = sigismember(&pend, SIGPIPE) == 1;
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = write(m_inFd, data.data() + off, data.size() - off);
        if (w > 0) off += (size_t)w;
        else if (w < 0 && errno == EINTR) continue;
        else { ok = false; break; }
    }
    if (!ok && !wasPending) {
        sigemptyset(&pend);
        sigpending(&pend);
        if (sigismember(&pend, SIGPIPE) == 1) {
            struct timespec zero = { 0, 0 };
            sigtimedwait(&pipeSet, 0, &zero);
        }
    }
    pthread_sigmask(SIG_SETMASK, &oldMask, 0);
    return ok;
}

bool ShellRunner::start(const std::string& command, const std::string& cwd, const std::string& shell, int timeoutS,
                        std::string* err, bool* newSession) {
    if (newSession) *newSession = false;
    if (m_running) { if (err) *err = "a command is still running"; return false; }
    bool fresh = false;
    if (!sessionAlive()) {
        reapSession();                                    // the previous session's reader and fds, if any
        if (!spawnShell(shell, err)) return false;
        fresh = true;
    }

    pthread_mutex_lock(&m_mu);
    m_cmdActive = true;
    m_cmdDone = false;
    m_result = RunResult();
    m_stopReq = false;
    m_timedOut = false;
    m_killed = false;
    m_startMs = nowMs();
    m_timeoutS = timeoutS;
    pthread_mutex_unlock(&m_mu);
    m_command = command;
    m_running = true;

    std::string data = fresh ? buildSessionInit(cwd) : std::string();
    data += buildCommandLine(command, m_marker);
    if (!writeAll(data)) {
        int e = errno;
        pthread_mutex_lock(&m_mu);
        m_cmdActive = false;
        pthread_mutex_unlock(&m_mu);
        m_running = false;
        killGroup(SIGKILL);                               // whatever is left of it: the next command starts afresh
        if (err) *err = std::string("the shell session ended (") + strerror(e) + "), run the command again";
        return false;
    }
    if (newSession) *newSession = fresh;
    return true;
}

void ShellRunner::stop() {
    pthread_mutex_lock(&m_mu);
    m_stopReq = true;
    pthread_mutex_unlock(&m_mu);
}

void ShellRunner::shutdown() {
    if (m_inFd >= 0) { close(m_inFd); m_inFd = -1; }      // EOF on its stdin: an idle shell exits by itself
    if (sessionAlive()) killGroup(SIGHUP);                // like closing a terminal (nohup'ed jobs survive)
}

void* ShellRunner::threadMain(void* self) {
    static_cast<ShellRunner*>(self)->readerLoop();
    return 0;
}

void ShellRunner::emitLocked(const std::string& data) {
    if (data.empty()) return;
    if (m_nlHeld) { m_pending += '\n'; m_nlHeld = false; }
    m_pending += data;
}

void ShellRunner::flushHeldLocked() {
    if (m_nlHeld) { m_pending += '\n'; m_nlHeld = false; }
    m_pending += m_hold;
    m_hold.clear();
}

// Splits the stream into lines. A complete line that is the sentinel ends the command (and swallows the '\n' of the
// empty line echoed before it); every other line is queued, its '\n' held back until the next byte is known not to
// be the sentinel. A partial line is held only while it can still become the sentinel, so prompts / progress
// output without '\n' still show up immediately.
void ShellRunner::onOutput(const char* data, size_t n) {
    pthread_mutex_lock(&m_mu);
    m_hold.append(data, n);
    size_t pos = 0;
    for (;;) {
        size_t nl = m_hold.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = m_hold.substr(pos, nl - pos);
        pos = nl + 1;
        RunResult r;
        if (m_atLineStart && parseSentinelLine(line, m_marker, &r)) {
            m_nlHeld = false;
            if (m_cmdActive && !m_cmdDone) { m_result = r; m_cmdDone = true; }
        } else {
            if (m_nlHeld) m_pending += '\n';
            m_pending += line;
            m_nlHeld = true;
        }
        m_atLineStart = true;
    }
    m_hold.erase(0, pos);
    if (!m_hold.empty()) {
        bool candidate = m_atLineStart && m_hold.size() < kHoldMaxBytes &&
            (m_hold.size() <= m_marker.size() ? m_marker.compare(0, m_hold.size(), m_hold) == 0
                                              : m_hold.compare(0, m_marker.size(), m_marker) == 0);
        if (!candidate) {
            emitLocked(m_hold);
            m_hold.clear();
            m_atLineStart = false;
        }
    }
    pthread_mutex_unlock(&m_mu);
}

void ShellRunner::readerLoop() {
    char buf[1024];
    int stage = 0;                          // 0 nothing sent, 1 SIGINT, 2 SIGTERM, 3 SIGKILL
    long long sigAt = 0;
    bool shellGone = false;
    long long goneAt = 0;
    int status = -1;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(m_outFd, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = kSelectMs * 1000;
        int n = select(m_outFd + 1, &rf, 0, 0, &tv);
        bool eof = false;
        if (n > 0) {
            ssize_t r = read(m_outFd, buf, sizeof(buf));
            if (r > 0) onOutput(buf, (size_t)r);
            else if (r == 0 || (errno != EINTR && errno != EAGAIN)) eof = true;
        } else if (n < 0 && errno != EINTR) {
            eof = true;
        }
        if (eof) break;

        long long now = nowMs();
        if (!shellGone) {
            int st;
            pid_t w = waitpid(m_pid, &st, WNOHANG);
            if (w == m_pid || (w < 0 && errno == ECHILD)) {
                shellGone = true;
                goneAt = now;
                status = (w == m_pid) ? st : -1;
                killGroup(SIGHUP);          // its background jobs, as when a terminal closes
            }
        } else if (now - goneAt > kDrainMs) {
            break;                          // a job that survived SIGHUP still holds the pipe
        }
        if (shellGone) continue;

        pthread_mutex_lock(&m_mu);
        bool active = m_cmdActive && !m_cmdDone;
        bool stopReq = m_stopReq;
        long long startMs = m_startMs;
        int timeoutS = m_timeoutS;
        pthread_mutex_unlock(&m_mu);
        if (!active) { stage = 0; continue; }
        if (stage == 0) {
            bool timedOut = timeoutS > 0 && now - startMs > (long long)timeoutS * 1000;
            if (stopReq || timedOut) {
                pthread_mutex_lock(&m_mu);
                m_killed = true;
                if (!stopReq) m_timedOut = true;
                pthread_mutex_unlock(&m_mu);
                killGroup(SIGINT);
                stage = 1;
                sigAt = now;
            }
        } else if (stage == 1 && now - sigAt >= kTermAfterMs) {
            killGroup(SIGTERM);
            stage = 2;
        } else if (stage == 2 && now - sigAt >= kKillAfterMs) {
            killGroup(SIGKILL);
            stage = 3;
        }
    }
    if (!shellGone) {                       // EOF first: the shell closed its output or is just exiting
        long long waitStart = nowMs();
        for (;;) {
            int st;
            pid_t w = waitpid(m_pid, &st, WNOHANG);
            if (w == m_pid) { status = st; break; }
            if (w < 0 && errno != EINTR) { status = -1; break; }
            if (nowMs() - waitStart > kReapGraceMs) {
                killGroup(SIGKILL);
                status = (waitpid(m_pid, &st, 0) == m_pid) ? st : -1;
                break;
            }
            usleep(20000);
        }
        killGroup(SIGHUP);
    }
    pthread_mutex_lock(&m_mu);
    flushHeldLocked();
    m_exitStatus = status;
    if (m_cmdActive && !m_cmdDone) {        // the shell ended during the command: no sentinel
        m_result = RunResult();
        m_cmdDone = true;
    }
    m_alive = false;
    pthread_mutex_unlock(&m_mu);
}

void ShellRunner::reapSession() {
    if (m_threadStarted) {
        pthread_join(m_thread, 0);
        m_threadStarted = false;
    }
    if (m_outFd >= 0) { close(m_outFd); m_outFd = -1; }
    if (m_inFd >= 0) { close(m_inFd); m_inFd = -1; }
    m_pid = -1;
}

bool ShellRunner::pump(TermBuffer& buf, RunResult* result, bool* timedOut, bool* killed) {
    if (!m_threadStarted && !m_running) return false;
    std::string chunk;
    pthread_mutex_lock(&m_mu);
    chunk.swap(m_pending);
    bool done = m_running && m_cmdActive && m_cmdDone;
    RunResult r = m_result;
    bool to = m_timedOut, k = m_killed;
    bool alive = m_alive;
    int exitStatus = m_exitStatus;
    if (done) { m_cmdActive = false; m_cmdDone = false; }
    pthread_mutex_unlock(&m_mu);

    if (!chunk.empty()) buf.append(chunk.data(), chunk.size());
    if (!alive && m_threadStarted) reapSession();         // the reader has finished (m_alive is its last write)
    if (!done) return false;

    if (!r.haveSentinel) {
        r.shellExited = true;
        r.rc = rcFromStatus(exitStatus);
    }
    if (result) *result = r;
    if (timedOut) *timedOut = to;
    if (killed) *killed = k;
    m_running = false;
    return true;
}

} // namespace id6term
