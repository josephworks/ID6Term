// id6term_app.cpp - libID6Term.so: a shell terminal as a native iDrive "PluginApp" for NBT Evo (ID6) head units.
//
// HmiMain3D's CAppManager loads every <ro|rw>/Plugins/<Name>/<Name>.manifest at boot. Ours names this library,
// the UI description (ID6Term.xml) and the text / image DBs. The XML declares the "Terminal" entry row in
// ConnectedDrive under "Vehicle Apps" (state 50 = ST_CDr_Selection, after the section label, focus 7514) and our screens:
//   hmiState 10  menu       Console / Enter command / Run last command again / Quick commands / Stop / Clear / About,
//                           titled "Terminal - Joseph Robinson", with a footer label "ID6Term <release> by Joseph Robinson"
//   hmiState 11  Console    status label (raDataModel 40), "Enter command" button, Richtext list (raListModel 41)
//                           holding the output of the last commands; the knob scrolls the text
//   hmiState 12  Command    the iDrive speller (<input>): letters arrive through raAction 26 (argument 8), the
//                           suggestion rows (history + quick commands) through raAction 28 (argument 1), the OK /
//                           result field through raAction 27
//   hmiState 13  Quick      eight preset commands, one button each (raActions 30..37)
//   hmiState 14  About      one Richtext text (TextID 17)
// Commands run in one shell session through ShellRunner (a long-lived sh in its own process group, output streamed,
// timeout): cd, variables and functions persist between commands like in a terminal. History is kept in
// /var/opt/id6term/history.
//
// Framework mechanics (all from the car-proven NBT-AAD app, see bmw_rhmi.hpp): our app object is a bmw::PluginApp
// built with the framework's own constructor; it gets a copy of the PluginApp vtable with onInit / onError /
// onFinalize replaced. Model updates happen on the app thread only (handlers and posted Runnables).
//
// Logs: slog major 0x4954 ('IT') and /dev/shmem/id6term-app.log (capped, fresh per HMI run).
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <new>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dlfcn.h>
#ifdef __QNX__
#include <sys/slog.h>
#include <sys/slogcodes.h>
#endif
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void*)-2)   // QNX value
#endif

#include "bmw_rhmi.hpp"
#include "term_buffer.hpp"
#include "command_line.hpp"
#include "shell_runner.hpp"

#define ID6TERM_AUTHOR  "Joseph Robinson"
#define ID6TERM_RELEASE "1.0.2"
#define ID6TERM_VERSION "id6term-" ID6TERM_RELEASE " (" __DATE__ " " __TIME__ ")"
#define ID6TERM_BANNER  "ID6Term " ID6TERM_VERSION " by " ID6TERM_AUTHOR

namespace {

using namespace id6term;

// ---- ids shared with res/ID6Term.xml and res/make_db.py ---------------------------------------------------------
const unsigned kMenuState    = 10;
const unsigned kConsoleState = 11;
const unsigned kInputState   = 12;
const unsigned kActRunLast   = 21;
const unsigned kActStop      = 22;
const unsigned kActClear     = 23;
const unsigned kActLetter    = 26;      // <input action="26">: argument 8 = the letter / "del" / "delall"
const unsigned kActOk        = 27;      // <input resultAction="27">
const unsigned kActSuggest   = 28;      // <input suggestAction="28">: argument 1 = row index
const unsigned kActQuickFirst = 30;     // raActions 30..37 = kQuickCommands[0..7]
const unsigned kQuickCount   = 8;
const unsigned kStatusModel  = 40;      // raDataModel (Richtext label in the console)
const unsigned kConsoleModel = 41;      // raListModel Richtext: the output text
const unsigned kInputResultModel = 43;  // raDataModel: the line typed so far
const unsigned kSuggestModel = 44;      // raListModel: suggestion rows
const unsigned kAboutModel   = 45;      // raListModel Richtext
const unsigned kBrandModel   = 47;      // raDataModel: menu footer "ID6Term <version> by Joseph Robinson"
const unsigned kTextAbout    = 17;
const unsigned kEvConsole    = 55;      // <focusEvent id="55" target="11"/>
const unsigned kEnterButton  = 81;      // <button id="81"> in the console: busy ring while a command runs
const unsigned kPropWaiting  = 8;       // LABEL_WAITINGANIMATION
const unsigned kHandlerFocus = 1;
const unsigned kArgLetter    = 8;
const unsigned kArgIndex     = 1;

// Must match texts 30..37 in make_db.py.
const char* const kQuickCommands[kQuickCount] = {
    "uname -a", "pidin arg", "ifconfig", "df", "ls -la /", "ls /mnt /fs /dev/shmem", "sloginfo | tail -n 40", "date"
};

const char* const kDataDir     = "/var/opt/id6term";          // persistence partition (rw, survives reboots)
const char* const kDataDirAlt  = "/dev/shmem";                // fallback when /var/opt is not writable
const char* const kDisableName = "disable";                   // <dataDir>/disable: do not create the app
const char* const kHistoryName = "history";
const char* const kConfigName  = "config";                    // shell=/bin/sh, timeout=20, cwd=/, kbd=lower
const char* const kKbdName     = "kbd";                       // letter case set with the `kbd` built-in
const char* const kLogPath     = "/dev/shmem/id6term-app.log";
const size_t      kLogCap      = 512 * 1024;
const char* const kDefaultShell = "/bin/sh";
const int         kDefaultTimeoutS = 20;
const size_t      kConsoleMaxBytes = 6000;    // Richtext cell: the newest lines that fit
const size_t      kSuggestMax  = 10;
const unsigned    kPollRunningMs = 250;
const unsigned    kPollIdleMs    = 1000;

// ---- logging -------------------------------------------------------------------------------------------------------
void alog(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[600];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int n = snprintf(line, sizeof(line), "[%5ld.%03ld] [ID6Term] %s\n", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000), msg);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    struct stat st;
    if (stat(kLogPath, &st) == 0 && (size_t)st.st_size > kLogCap) unlink(kLogPath);
    int fd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        ssize_t w = write(fd, line, (size_t)n);
        (void)w;
        close(fd);
    }
#ifdef __QNX__
    slogf(_SLOG_SETCODE(0x4954, 1), _SLOG_INFO, "[ID6Term] %s", msg);
#endif
}

bool fileExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// ---- persistent data -----------------------------------------------------------------------------------------------
std::string g_dataDir;

void initDataDir() {
    if (mkdir(kDataDir, 0777) == 0 || errno == EEXIST) {
        if (access(kDataDir, W_OK) == 0) { g_dataDir = kDataDir; return; }
    }
    alog("%s not writable (errno %d), using %s", kDataDir, errno, kDataDirAlt);
    g_dataDir = kDataDirAlt;
}

std::string dataPath(const char* name) {
    return g_dataDir + "/" + name;
}

struct Config {
    std::string shell;
    int timeoutS;
    std::string cwd;
    LetterCase kbd;
    Config() : shell(kDefaultShell), timeoutS(kDefaultTimeoutS), cwd("/"), kbd(kCaseLower) {}
};

void loadConfig(Config& c) {
    FILE* f = fopen(dataPath(kConfigName).c_str(), "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string l = History::trimmed(line);
            if (l.empty() || l[0] == '#') continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = l.substr(0, eq), v = History::trimmed(l.substr(eq + 1));
            if (k == "shell" && !v.empty()) c.shell = v;
            else if (k == "timeout") c.timeoutS = atoi(v.c_str());
            else if (k == "cwd" && !v.empty()) c.cwd = v;
            else if (k == "kbd") parseLetterCase(v, &c.kbd);
        }
        fclose(f);
    }
    f = fopen(dataPath(kKbdName).c_str(), "r");            // the last `kbd <mode>` wins over the config
    if (f) {
        char line[32];
        if (fgets(line, sizeof(line), f)) parseLetterCase(History::trimmed(line), &c.kbd);
        fclose(f);
    }
}

// ---- state --------------------------------------------------------------------------------------------------------
bmw::PluginApp* g_app = 0;          // the created app
bmw::PluginApp* g_runApp = 0;       // the app while it is initialised and not finalized (polling target)
Config      g_cfg;
TermBuffer  g_buf(200, 160);
ShellRunner g_runner;
History     g_history(50);
SpellerInput g_input;
std::vector<std::string> g_suggestions;
std::string g_cwd = "/";
unsigned    g_publishedGen = (unsigned)-1;
bool        g_busyShown = false;
int         g_lastRc = 0;
bool        g_haveLastRc = false;
unsigned    g_sessions = 0;         // shell sessions started

// ---- model helpers --------------------------------------------------------------------------------------------------
// A 1-column raListModel table of strings: rows[i] becomes cell (i, 0) as a BasicString<char> Variant (type 0x22).
void updateStringList(bmw::PluginApp* app, unsigned modelId, const std::vector<std::string>& rows) {
    bmw::Map<unsigned, bmw::Variant> cells;
    bmw::DataTableBase table(&cells, 1);
    for (size_t i = 0; i < rows.size(); ++i) {
        unsigned key = (unsigned)i;
        bmw::Variant& v = cells[key];
        delete v.m_obj;                                  // fresh cell: NULL
        v.m_obj = new bmw::Variant::ObjectContainer< bmw::BasicString<char> >(bmw::BasicString<char>(rows[i].c_str()));
    }
    app->updateModel(modelId, table);
}

std::string statusText() {
    char s[96];
    if (g_runner.running()) {
        long secs = (long)((ShellRunner::nowMs() - g_runner.startedMs()) / 1000);
        snprintf(s, sizeof(s), "running %lds: %s", secs, g_runner.command().c_str());
    } else if (g_haveLastRc) {
        snprintf(s, sizeof(s), "%s $  [exit %d]", g_cwd.c_str(), g_lastRc);
    } else {
        snprintf(s, sizeof(s), "%s $  ready", g_cwd.c_str());
    }
    if (strlen(s) > 60) { s[57] = '.'; s[58] = '.'; s[59] = '.'; s[60] = 0; }
    return s;
}

void publishConsole(bmw::PluginApp* app, bool force) {
    if (!force && g_buf.generation() == g_publishedGen) return;
    std::string text;
    g_buf.render(text, kConsoleMaxBytes);
    if (text.empty()) text = ID6TERM_BANNER "\nType a command with 'Enter command'.";
    std::vector<std::string> one(1, text);
    updateStringList(app, kConsoleModel, one);
    app->updateModel(kStatusModel, statusText().c_str());
    g_publishedGen = g_buf.generation();
}

void showBusy(bmw::PluginApp* app, bool on) {
    if (on == g_busyShown) return;
    bmw::Parcel p(on);
    app->setProperty(kEnterButton, kPropWaiting, p);
    g_busyShown = on;
}

void showSuggestions(bmw::PluginApp* app) {
    std::vector<std::string> quick(kQuickCommands, kQuickCommands + kQuickCount);
    g_suggestions = g_history.suggestions(g_input.text(), quick, kSuggestMax);
    updateStringList(app, kSuggestModel, g_suggestions);
}

void showInput(bmw::PluginApp* app) {
    app->updateModel(kInputResultModel, g_input.text().c_str());
}

void showAbout(bmw::PluginApp* app) {
    bmw::Map<unsigned, bmw::Variant> cells;
    bmw::DataTableBase table(&cells, 1);
    unsigned key = 0;
    bmw::Variant& v = cells[key];
    delete v.m_obj;
    v.m_obj = new bmw::Variant::ObjectContainer<bmw::TextID>(bmw::TextID(kTextAbout));
    app->updateModel(kAboutModel, table);
}

// ---- running commands -----------------------------------------------------------------------------------------------
void echoHelp() {
    char line[200];
    g_buf.addLine(ID6TERM_BANNER);
    snprintf(line, sizeof(line), "One %s session runs every line, like a terminal: cd, variables, functions persist.",
             g_cfg.shell.c_str());
    g_buf.addLine(line);
    snprintf(line, sizeof(line), "stdin is /dev/null. Timeout per command: %d s; Stop interrupts it earlier (Ctrl-C).",
             g_cfg.timeoutS);
    g_buf.addLine(line);
    g_buf.addLine("Built-ins: clear, help, history, kbd.  Speller: pick a suggestion to run it; empty OK = run last.");
    snprintf(line, sizeof(line), "kbd lower|invert|asis: letter case of the keyboard (now: %s).",
             letterCaseName(g_input.letterCase()));
    g_buf.addLine(line);
    g_buf.addLine("Config: " + dataPath(kConfigName) + " (shell=, timeout=, cwd=, kbd=)");
}

// `kbd`: shows or sets what happens to the case of speller letters (see LetterCase), persisted in <dataDir>/kbd.
void kbdBuiltin(const std::string& cmd) {
    std::string arg = History::trimmed(cmd.substr(3));
    LetterCase c;
    if (!arg.empty()) {
        if (!parseLetterCase(arg, &c)) {
            g_buf.addLine("kbd: unknown mode '" + arg + "' (lower, invert, asis)");
            return;
        }
        g_input.setLetterCase(c);
        FILE* f = fopen(dataPath(kKbdName).c_str(), "w");
        if (f) { fprintf(f, "%s\n", letterCaseName(c)); fclose(f); }
        alog("kbd -> %s", letterCaseName(c));
    }
    c = g_input.letterCase();
    const char* what = c == kCaseLower ? "letters are typed in lower case"
                     : c == kCaseInvert ? "case swapped: the keyboard's capitals type lower case and vice versa"
                     : "letters are typed as the keyboard sends them";
    g_buf.addLine(std::string("kbd ") + letterCaseName(c) + ": " + what);
}

void runCommand(bmw::PluginApp* app, const std::string& raw) {
    std::string cmd = History::trimmed(raw);
    if (cmd.empty()) { alog("run: empty command ignored"); return; }
    alog("run: '%s' (cwd %s)", cmd.c_str(), g_cwd.c_str());
    switch (classifyBuiltin(cmd)) {
    case kBuiltinClear:
        g_buf.clear();
        publishConsole(app, true);
        return;
    case kBuiltinHelp:
        g_buf.addLine(g_cwd + " $ " + cmd);
        echoHelp();
        publishConsole(app, true);
        return;
    case kBuiltinHistory: {
        g_buf.addLine(g_cwd + " $ " + cmd);
        const std::vector<std::string>& h = g_history.entries();
        for (size_t i = h.size(); i > 0; --i) g_buf.addLine("  " + h[i - 1]);
        publishConsole(app, true);
        return;
    }
    case kBuiltinKbd:
        g_buf.addLine(g_cwd + " $ " + cmd);
        kbdBuiltin(cmd);
        publishConsole(app, true);
        return;
    case kNotBuiltin:
        break;
    }
    if (g_runner.running()) {
        g_buf.addLine("[busy] still running: " + g_runner.command() + "  (Stop running command first)");
        publishConsole(app, true);
        return;
    }
    g_history.add(cmd);
    g_history.save(dataPath(kHistoryName).c_str());
    g_buf.addLine(g_cwd + " $ " + cmd);
    std::string err;
    bool fresh = false;
    if (!g_runner.start(cmd, g_cwd, g_cfg.shell, g_cfg.timeoutS, &err, &fresh)) {
        g_buf.addLine("[error] " + err);
        alog("run: start failed: %s", err.c_str());
    } else {
        if (fresh) {
            if (g_sessions) g_buf.addLine("[new shell session in " + g_cwd + "]");
            ++g_sessions;
            alog("shell session %u started (%s)", g_sessions, g_cfg.shell.c_str());
        }
        showBusy(app, true);
    }
    publishConsole(app, true);
}

// Called from the poll Runnable: output (also of background jobs while idle) and completion of the running command.
void serviceRunner(bmw::PluginApp* app) {
    RunResult r;
    bool timedOut = false, killed = false;
    std::string cmd = g_runner.command();
    if (g_runner.pump(g_buf, &r, &timedOut, &killed)) {
        char tail[160];
        if (killed && r.shellExited)
            snprintf(tail, sizeof(tail), "[%s, shell killed; the next command starts a new session]",
                     timedOut ? "timeout" : "stopped");
        else if (timedOut) snprintf(tail, sizeof(tail), "[timeout after %d s, interrupted]", g_cfg.timeoutS);
        else if (killed) snprintf(tail, sizeof(tail), "[interrupted]");
        else if (r.shellExited)
            snprintf(tail, sizeof(tail), "[shell exited (%d); the next command starts a new session]", r.rc);
        else if (r.rc == 0) tail[0] = 0;
        else snprintf(tail, sizeof(tail), "[exit %d]", r.rc);
        if (tail[0]) g_buf.addLine(tail);
        if (r.haveSentinel && !r.cwd.empty() && r.cwd != g_cwd) {
            alog("cwd %s -> %s", g_cwd.c_str(), r.cwd.c_str());
            g_cwd = r.cwd;
        }
        g_lastRc = r.rc;
        g_haveLastRc = true;
        showBusy(app, false);
        alog("done: '%s' rc %d%s%s%s (%u lines in buffer)", cmd.c_str(), r.rc, timedOut ? " timeout" : "",
             killed ? " killed" : "", r.shellExited ? " shell-exited" : "", (unsigned)g_buf.lineCount());
        publishConsole(app, true);
    } else {
        publishConsole(app, false);
    }
}

// ---- bmw::Runnable stand-in (vtable: D1, D0, run) for post(): a static object, destructor slots are no-ops --------
struct PollRunnable { void** vptr; };
PollRunnable g_poll;
void* g_pollVtbl[5];
void runnableNoop(void*) {}

void pollRun(void* /*self*/) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return;                                   // finalized: stop polling
    bool wasRunning = g_runner.running();
    serviceRunner(app);
    if (wasRunning || g_runner.running()) app->updateModel(kStatusModel, statusText().c_str());
    app->post(reinterpret_cast<bmw::Runnable*>(&g_poll), g_runner.running() ? kPollRunningMs : kPollIdleMs);
}

void startPolling(bmw::PluginApp* app) {
    void** base = reinterpret_cast<void**>(dlsym(RTLD_DEFAULT, "_ZTVN3bmw8RunnableE"));
    if (base) memcpy(g_pollVtbl, base, sizeof(g_pollVtbl));   // offset-to-top + typeinfo of bmw::Runnable
    else { g_pollVtbl[0] = 0; g_pollVtbl[1] = 0; }
    g_pollVtbl[2] = reinterpret_cast<void*>(&runnableNoop);
    g_pollVtbl[3] = reinterpret_cast<void*>(&runnableNoop);
    g_pollVtbl[4] = reinterpret_cast<void*>(&pollRun);
    g_poll.vptr = g_pollVtbl + 2;
    g_runApp = app;
    app->post(reinterpret_cast<bmw::Runnable*>(&g_poll), 1500);
    alog("poll posted (Runnable vtable %s)", base ? "from libCoreFramework" : "built locally");
}

// ---- action handlers --------------------------------------------------------------------------------------------------
// One line per argument: id, Variant type code, first payload bytes and, when it reads as text, the text. This is
// what tells us the type codes the HMI really uses for the speller letter and the list index.
struct ArgDump {
    char buf[256];
    size_t n;
    ArgDump() : n(0) { buf[0] = 0; }
    void put(const char* fmt, ...) {
        if (n + 1 >= sizeof(buf)) return;
        va_list ap;
        va_start(ap, fmt);
        int l = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
        va_end(ap);
        if (l > 0) n += (size_t)l < sizeof(buf) - n ? (size_t)l : sizeof(buf) - n - 1;
    }
    void operator()(const unsigned& k, const bmw::Variant& v) {
        bmw::VariantView vv = bmw::viewVariant(v);
        put(" %u:type=0x%x", k, vv.type);
        if (vv.payload) {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(vv.payload);
            put("[%02x%02x%02x%02x]", b[0], b[1], b[2], b[3]);
        }
        char s[64] = "";
        if (bmw::variantToString(v, s, sizeof(s))) put("='%s'", s);
    }
};

void logArgs(const char* what, const bmw::Map<unsigned, bmw::Variant>& args) {
    ArgDump d;
    args.forEach(d);
    alog("%s: %u arg(s)%s", what, args.count(), d.buf);
}

bool onLetter(void*, const bmw::Map<unsigned, bmw::Variant>& args) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return true;
    logArgs("speller", args);
    const bmw::Variant* v = args.find(kArgLetter);
    char letter[64];
    if (!v || !bmw::variantToString(*v, letter, sizeof(letter))) {
        alog("speller: no readable text in argument %u (see types above)", kArgLetter);
        return true;
    }
    if (g_input.apply(letter)) {
        showInput(app);
        showSuggestions(app);
    }
    return true;
}

// Parcel form of the speller action: only logged, to learn the Parcel layout should the Map one not deliver.
bool onLetterParcel(void*, const bmw::Parcel& p) {
    unsigned size = p.size(), avail = p.dataAvail();
    const unsigned char* raw = p.getRawData();
    char hex[3 * 48 + 1];
    size_t n = 0;
    for (unsigned i = 0; raw && i < size && i < 48; ++i) n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x ", raw[i]);
    hex[n] = 0;
    alog("speller parcel: size %u avail %u raw %s", size, avail, hex);
    return true;
}

bool onOk(void*, const bmw::Parcel&) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return true;
    std::string cmd = g_input.text();
    alog("speller OK: '%s'", cmd.c_str());
    if (History::trimmed(cmd).empty()) {
        if (g_history.empty()) return true;
        cmd = g_history.last();                           // empty line = run the last command, like Enter in a shell
    }
    g_input.clear();
    showInput(app);
    runCommand(app, cmd);
    app->triggerEvent(kEvConsole);
    return true;
}

bool onSuggest(void*, const bmw::Map<unsigned, bmw::Variant>& args) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return true;
    logArgs("suggestion", args);
    const bmw::Variant* v = args.find(kArgIndex);
    int idx = -1;
    if (!v || !bmw::variantToInt(*v, &idx)) {
        alog("suggestion: no index in argument %u", kArgIndex);
        return true;
    }
    int n = (int)g_suggestions.size();
    if (idx < 0 || idx >= n) {                            // an 8- / 16-bit container: upper bytes are garbage
        if ((idx & 0xff) < n) idx &= 0xff;
        else if ((idx & 0xffff) < n) idx &= 0xffff;
    }
    if (idx < 0 || idx >= n) { alog("suggestion: index %d out of range (%d rows)", idx, n); return true; }
    alog("suggestion %d: '%s'", idx, g_suggestions[idx].c_str());
    std::string cmd = g_suggestions[idx];
    g_input.clear();
    showInput(app);
    runCommand(app, cmd);
    app->triggerEvent(kEvConsole);
    return true;
}

bool onRunLast(void*, const bmw::Parcel&) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return true;
    if (g_history.empty()) { alog("run last: history empty"); return true; }
    runCommand(app, g_history.last());
    app->triggerEvent(kEvConsole);
    return true;
}

bool onStop(void*, const bmw::Parcel&) {
    if (!g_runApp) return true;
    if (!g_runner.running()) { alog("stop: nothing running"); return true; }
    alog("stop: killing '%s'", g_runner.command().c_str());
    g_runner.stop();
    return true;
}

bool onClear(void*, const bmw::Parcel&) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return true;
    g_buf.clear();
    g_haveLastRc = false;
    publishConsole(app, true);
    return true;
}

template <unsigned N> bool onQuick(void*, const bmw::Parcel&) {
    bmw::PluginApp* app = g_runApp;
    if (!app) return true;
    runCommand(app, kQuickCommands[N]);
    app->triggerEvent(kEvConsole);
    return true;
}

bool onConsoleFocus(void*, const bmw::Parcel& p) {
    bool f = false;
    p.readBool(f);
    alog("console state %u focus -> %d", kConsoleState, (int)f);
    if (f && g_runApp) publishConsole(g_runApp, true);
    return true;
}

// The speller keeps an unfinished line across BACK / popups; it is cleared when a command runs. On entry the
// HMI's result field and suggestion list are re-synced with our text (the HMI may have dropped the models).
bool onInputFocus(void*, const bmw::Parcel& p) {
    bool f = false;
    p.readBool(f);
    alog("input state %u focus -> %d (line '%s')", kInputState, (int)f, g_input.text().c_str());
    if (f && g_runApp) {
        showInput(g_runApp);
        showSuggestions(g_runApp);
    }
    return true;
}

// ---- the app object -----------------------------------------------------------------------------------------------------
void* g_vtbl[2 + bmw::kVtblSlots];   // [offset-to-top, typeinfo, slots...] copied from bmw::PluginApp's vtable

void addAction(bmw::PluginApp* self, unsigned id, bool (*stub)(void*, const bmw::Parcel&)) {
    bmw::ActionDelegate d;
    d.object = self;
    d.stub = stub;
    self->addHandler(id, d);
}

void addMapAction(bmw::PluginApp* self, unsigned id, bool (*stub)(void*, const bmw::Map<unsigned, bmw::Variant>&)) {
    bmw::MapDelegate d;
    d.object = self;
    d.stub = stub;
    self->addHandlerWithMap(id, d);
}

void addFocus(bmw::PluginApp* self, unsigned stateId, bool (*stub)(void*, const bmw::Parcel&)) {
    bmw::ActionDelegate d;
    d.object = self;
    d.stub = stub;
    self->addHandler(stateId, kHandlerFocus, d);
}

bool appOnInit(bmw::PluginApp* self) {
    g_runApp = self;
    initDataDir();
    loadConfig(g_cfg);
    g_cwd = g_cfg.cwd;
    g_input.setLetterCase(g_cfg.kbd);
    g_history.load(dataPath(kHistoryName).c_str());

    addMapAction(self, kActLetter, &onLetter);
    addAction(self, kActLetter, &onLetterParcel);
    addAction(self, kActOk, &onOk);
    addMapAction(self, kActSuggest, &onSuggest);
    addAction(self, kActRunLast, &onRunLast);
    addAction(self, kActStop, &onStop);
    addAction(self, kActClear, &onClear);
    addAction(self, kActQuickFirst + 0, &onQuick<0>);
    addAction(self, kActQuickFirst + 1, &onQuick<1>);
    addAction(self, kActQuickFirst + 2, &onQuick<2>);
    addAction(self, kActQuickFirst + 3, &onQuick<3>);
    addAction(self, kActQuickFirst + 4, &onQuick<4>);
    addAction(self, kActQuickFirst + 5, &onQuick<5>);
    addAction(self, kActQuickFirst + 6, &onQuick<6>);
    addAction(self, kActQuickFirst + 7, &onQuick<7>);
    addFocus(self, kConsoleState, &onConsoleFocus);
    addFocus(self, kInputState, &onInputFocus);

    bool ok = self->onInit();
    showAbout(self);
    self->updateModel(kBrandModel, "ID6Term " ID6TERM_RELEASE " by " ID6TERM_AUTHOR);   // menu footer, one row
    publishConsole(self, true);
    showInput(self);
    showSuggestions(self);
    startPolling(self);
    alog("onInit: handlers registered, base onInit -> %d, shell %s, timeout %d s, cwd %s, kbd %s, %u history entries, data in %s",
         (int)ok, g_cfg.shell.c_str(), g_cfg.timeoutS, g_cwd.c_str(), letterCaseName(g_cfg.kbd),
         (unsigned)g_history.entries().size(), g_dataDir.c_str());
    return ok;
}

void appOnError(bmw::PluginApp* /*self*/, unsigned code, const bmw::BasicString<char>& text) {
    const char* s = text.raw();
    alog("onError %u: %s", code, s ? s : "(null)");
}

void appOnFinalize(bmw::PluginApp* self) {
    alog("onFinalize");
    g_runApp = 0;
    g_runner.shutdown();
    self->onFinalize();
}

bmw::PluginApp* createApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params, const bmw::Bundle& config) {
    alog("create(name=%s) %s", name ? name : "(null)", ID6TERM_VERSION);
    std::string disable = std::string(kDataDir) + "/" + kDisableName;
    if (fileExists(disable.c_str())) { alog("disabled by %s", disable.c_str()); return 0; }
    if (g_app) { alog("create: already created"); return 0; }

    void* mem = ::operator new(bmw::kPluginAppStorage);
    memset(mem, 0, bmw::kPluginAppStorage);
    bmw::PluginApp* app = new (mem) bmw::PluginApp(dispatcher, bmw::BasicString<char>(name ? name : "ID6Term"), 1, 0,
                                                   bmw::RHMI_INTERFACE_VERSION_1, params, config);

    // Own vtable: identical to bmw::PluginApp's, with our onInit / onError / onFinalize.
    void** base = *reinterpret_cast<void***>(app);
    memcpy(g_vtbl, base - 2, sizeof(g_vtbl));
    g_vtbl[2 + bmw::kSlotOnInit]     = reinterpret_cast<void*>(&appOnInit);
    g_vtbl[2 + bmw::kSlotOnError]    = reinterpret_cast<void*>(&appOnError);
    g_vtbl[2 + bmw::kSlotOnFinalize] = reinterpret_cast<void*>(&appOnFinalize);
    *reinterpret_cast<void***>(app) = g_vtbl + 2;

    g_app = app;
    alog("create: app %p (PluginApp vtable %p)", (void*)app, (void*)base);
    return app;
}

void destroyApp(void* app) {
    alog("destroy(%p)", app);
    if (!app) return;
    g_runApp = 0;
    void** vt = *reinterpret_cast<void***>(app);
    reinterpret_cast<void (*)(void*)>(vt[1])(app);   // deleting destructor (PluginApp::~PluginApp + operator delete)
    if (app == g_app) g_app = 0;
}

__attribute__((constructor)) void libraryLoaded() {
    alog("library loaded into pid %d, %s", (int)getpid(), ID6TERM_VERSION);
}

} // namespace

// ---- entry point looked up by ManagedPluginApp::init (dlsym "pluginAppInfo") -----------------------------------------
extern "C" {
struct Id6PluginAppInfo {
    uint16_t versionMajor;
    uint16_t versionMinor;
    const char* name;
    const char* vendor;
    uint32_t rhmiInterfaceVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
__attribute__((visibility("default"))) Id6PluginAppInfo pluginAppInfo = {
    1, 0, "Terminal", ID6TERM_AUTHOR, 1, &createApp, &destroyApp
};
}
