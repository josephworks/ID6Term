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
const int kKillGraceMs = 2000;
const size_t kHoldFlushBytes = 4096;

// Environment for the child: a sane PATH for the head unit (the HMI's own may be minimal), HOME, TERM, plus
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
    : m_running(false), m_done(false), m_stopReq(false), m_timedOut(false), m_killed(false), m_exitStatus(0),
      m_pid(-1), m_fd(-1), m_threadStarted(false), m_startMs(0), m_timeoutS(20) {
    pthread_mutex_init(&m_mu, 0);
}

ShellRunner::~ShellRunner() {
    if (m_running) {
        stop();
        joinThread();
    }
    if (m_fd >= 0) close(m_fd);
    pthread_mutex_destroy(&m_mu);
}

bool ShellRunner::running() const {
    return m_running;
}

bool ShellRunner::start(const std::string& command, const std::string& cwd, const std::string& shell, int timeoutS,
                        std::string* err) {
    if (m_running) { if (err) *err = "a command is still running"; return false; }
    if (m_threadStarted) joinThread();                   // the previous run's reader, if pump() never reaped it

    int fds[2];
    if (pipe(fds) != 0) { if (err) *err = std::string("pipe: ") + strerror(errno); return false; }
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) {
        if (err) *err = std::string("/dev/null: ") + strerror(errno);
        close(fds[0]); close(fds[1]);
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    std::string script = buildScript(command);
    const char* argv[] = { "sh", "-c", script.c_str(), "sh", cwd.empty() ? "/" : cwd.c_str(), 0 };
    std::string envStorage;
    char** envp = buildEnv(envStorage);

    pid_t pid = -1;
#ifdef __QNX__
    int fdmap[3] = { devnull, fds[1], fds[1] };
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
    posix_spawn_file_actions_adddup2(&fa, devnull, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    if (posix_spawn(&pid, shell.c_str(), &fa, &attr, const_cast<char* const*>(argv), envp) != 0) pid = -1;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
#endif
    int spawnErrno = errno;
    delete[] envp;
    close(devnull);
    close(fds[1]);
    if (pid <= 0) {
        if (err) *err = std::string("spawn ") + shell + ": " + strerror(spawnErrno);
        close(fds[0]);
        return false;
    }

    pthread_mutex_lock(&m_mu);
    m_pending.clear();
    m_hold.clear();
    m_done = false;
    m_stopReq = false;
    m_timedOut = false;
    m_killed = false;
    m_exitStatus = 0;
    m_pid = pid;
    m_fd = fds[0];
    m_startMs = nowMs();
    m_timeoutS = timeoutS;
    m_command = command;
    m_running = true;
    pthread_mutex_unlock(&m_mu);

    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setstacksize(&ta, 64 * 1024);
    int rc = pthread_create(&m_thread, &ta, &threadMain, this);
    pthread_attr_destroy(&ta);
    if (rc != 0) {
        if (err) *err = std::string("pthread_create: ") + strerror(rc);
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int st;
        waitpid(pid, &st, 0);
        close(m_fd);
        m_fd = -1;
        m_running = false;
        return false;
    }
    m_threadStarted = true;
    return true;
}

void ShellRunner::stop() {
    pthread_mutex_lock(&m_mu);
    m_stopReq = true;
    pthread_mutex_unlock(&m_mu);
}

void* ShellRunner::threadMain(void* self) {
    static_cast<ShellRunner*>(self)->readerLoop();
    return 0;
}

void ShellRunner::readerLoop() {
    char buf[1024];
    bool eof = false;
    long long killAt = 0;                   // when SIGTERM was sent; SIGKILL follows after the grace period
    while (!eof) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(m_fd, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = kSelectMs * 1000;
        int n = select(m_fd + 1, &rf, 0, 0, &tv);
        if (n > 0) {
            ssize_t r = read(m_fd, buf, sizeof(buf));
            if (r > 0) {
                pthread_mutex_lock(&m_mu);
                m_pending.append(buf, (size_t)r);
                pthread_mutex_unlock(&m_mu);
            } else if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) {
                eof = true;
            }
        } else if (n < 0 && errno != EINTR) {
            eof = true;
        }
        if (eof) break;

        long long now = nowMs();
        pthread_mutex_lock(&m_mu);
        bool stopReq = m_stopReq;
        pthread_mutex_unlock(&m_mu);
        bool timedOut = m_timeoutS > 0 && now - m_startMs > (long long)m_timeoutS * 1000;
        if ((stopReq || timedOut) && !killAt) {
            if (timedOut) m_timedOut = true;
            m_killed = true;
            killAt = now;
            kill(-m_pid, SIGTERM);
            kill(m_pid, SIGTERM);
        } else if (killAt && now - killAt > kKillGraceMs) {
            kill(-m_pid, SIGKILL);
            kill(m_pid, SIGKILL);
            killAt = now + 1000000;        // once
        }
    }
    // Make sure nothing of the process group survives (a backgrounded child could hold the pipe open forever
    // otherwise - we got EOF, so the shell itself is gone or closed its end).
    int status = 0;
    pid_t w;
    long long waitStart = nowMs();
    for (;;) {
        w = waitpid(m_pid, &status, WNOHANG);
        if (w == m_pid || (w < 0 && errno != EINTR)) break;
        if (nowMs() - waitStart > kKillGraceMs) {
            kill(-m_pid, SIGKILL);
            kill(m_pid, SIGKILL);
            m_killed = true;
            w = waitpid(m_pid, &status, 0);
            break;
        }
        usleep(20000);
    }
    pthread_mutex_lock(&m_mu);
    m_exitStatus = (w == m_pid) ? status : -1;
    m_done = true;
    pthread_mutex_unlock(&m_mu);
}

void ShellRunner::joinThread() {
    if (!m_threadStarted) return;
    pthread_join(m_thread, 0);
    m_threadStarted = false;
}

void ShellRunner::emit(TermBuffer& buf, const std::string& data) {
    if (!data.empty()) buf.append(data.data(), data.size());
}

bool ShellRunner::pump(TermBuffer& buf, RunResult* result, bool* timedOut, bool* killed) {
    if (!m_running) return false;
    std::string chunk;
    bool done;
    pthread_mutex_lock(&m_mu);
    chunk.swap(m_pending);
    done = m_done;
    pthread_mutex_unlock(&m_mu);

    if (!chunk.empty()) {
        std::string data;
        data.swap(m_hold);
        data += chunk;
        size_t p = data.rfind(kSentinelStart);
        if (p == std::string::npos) {
            emit(buf, data);
        } else {
            size_t cut = (p > 0 && data[p - 1] == '\n') ? p - 1 : p;   // keep printf's leading "\n" with the sentinel
            emit(buf, data.substr(0, cut));
            m_hold = data.substr(cut);
            if (m_hold.size() > kHoldFlushBytes) {           // not a sentinel after all: a command printing \001
                emit(buf, m_hold);
                m_hold.clear();
            }
        }
    }
    if (!done) return false;

    joinThread();
    RunResult r = parseSentinel(m_hold);
    emit(buf, m_hold);                                        // whatever was not the sentinel
    m_hold.clear();
    if (!r.haveSentinel) {
        if (WIFEXITED(m_exitStatus)) r.rc = WEXITSTATUS(m_exitStatus);
        else if (WIFSIGNALED(m_exitStatus)) r.rc = 128 + WTERMSIG(m_exitStatus);
    }
    if (result) *result = r;
    if (timedOut) *timedOut = m_timedOut;
    if (killed) *killed = m_killed;
    close(m_fd);
    m_fd = -1;
    m_pid = -1;
    m_running = false;
    return true;
}

} // namespace id6term
