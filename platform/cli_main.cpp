// platform/cli_main.cpp — minimal macOS CLI driver for MUSHclient.
//
// Usage: mushcli <host> <port>
//
// Connects to a MUD server, pipes incoming text to stdout, and forwards
// lines typed on stdin as commands.  ANSI/telnet/MXP processing is live
// (all the ported Phase_* methods run on each incoming byte), so the
// protocol state machine is exercised end-to-end.
//
// When compiled with HAVE_READLINE (CMake finds libreadline or libedit),
// the input line supports full editing (arrow keys, backspace, Ctrl-A/E,
// etc.) and command history (up/down arrows).  Incoming MUD text clears
// and restores the input line so output and input don't interleave.
//
// Building: added to CMakeLists.txt as the 'mushcli' target, linked
//           against the mushcore static library.

#ifndef _WIN32

#include "platform/doc_stub.h"
#include "worldsock.h"
#include "platform/posix_socket.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <string>

// POSIX
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#endif

// ── Signal handling ───────────────────────────────────────────────────────────
static volatile bool g_quit    = false;
static volatile bool g_winch   = false;
static void on_sigint(int)  { g_quit  = true; }
static void on_sigwinch(int){ g_winch = true; }

// ── Forward declaration of the doc pointer used by readline callback ──────────
static CMUSHclientDoc* g_doc = nullptr;

// ── send_line — evaluate aliases, then send to the MUD ────────────────────────
// EvaluateCommand handles alias matching (including plugin aliases), speedwalk
// expansion, and calls SendMsg() for every resulting line.  The socket write
// happens inside SendMsg; we do not touch m_outstanding_data here.
static void send_line(const char* line, int len)
{
    if (!g_doc || !g_doc->m_pSocket || len < 0) return;
    std::string cmd(line, (size_t)len);
    bool bOmitFromLog = false;
    g_doc->EvaluateCommand(CString(cmd.c_str()), /*bCountThem=*/true,
                           bOmitFromLog, /*bTest=*/false);
}

#ifdef HAVE_READLINE

// ── readline integration ──────────────────────────────────────────────────────

// Called by readline when the user presses Enter.
static void readline_line_handler(char* line)
{
    if (!line) {
        // EOF (Ctrl-D)
        g_quit = true;
        return;
    }
    int len = (int)strlen(line);
    if (len > 0)
        add_history(line);
    send_line(line, len);   // send even if empty — blank Enter is a valid MUD command
    free(line);
}

// Called from doc_portable.cpp via g_before_output before printing MUD text.
// Erases the current input line so MUD output doesn't interleave with it.
// Uses portable VT100 sequences: CR + erase-to-end-of-line.
static void before_output_rl()
{
    fwrite("\r\033[K", 1, 4, stdout);
    fflush(stdout);
}

// Called from doc_portable.cpp via g_after_output after printing MUD text.
// Tells readline we moved off its line and redraws the prompt + partial input.
static void after_output_rl()
{
    rl_on_new_line();
    rl_redisplay();
}

#else // !HAVE_READLINE

// ── fallback: non-blocking cooked-mode stdin ──────────────────────────────────

static void make_stdin_nonblocking()
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1)
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

#endif // HAVE_READLINE

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // Parse flags
    int argi = 1;
    const char* worldFile = nullptr;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-d") == 0 || strcmp(argv[argi], "--debug") == 0) {
            g_debug = true;
        } else if ((strcmp(argv[argi], "-w") == 0 || strcmp(argv[argi], "--world") == 0)
                   && argi + 1 < argc) {
            worldFile = argv[++argi];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            fprintf(stderr, "Usage: %s [-d] [-w <world.mcl>] [<host> <port>]\n", argv[0]);
            return 1;
        }
        argi++;
    }

    // Host/port can come from arguments or world file
    const char* host = nullptr;
    unsigned int port = 0;
    if (argc - argi >= 2) {
        host = argv[argi];
        port = (unsigned int)atoi(argv[argi + 1]);
    } else if (argc - argi == 1) {
        fprintf(stderr, "Usage: %s [-d] [-w <world.mcl>] [<host> <port>]\n", argv[0]);
        return 1;
    }
    // If no host/port on command line, they must come from the world file

    signal(SIGINT,   on_sigint);
    signal(SIGTERM,  on_sigint);
    signal(SIGWINCH, on_sigwinch);

    // ── Initialise global MXP tables (elements, entities, colours) ───────────
    // On Windows this happens in CMUSHclientApp::InitInstance() (MUSHclient.cpp).
    App.MXP_LoadElements();
    App.MXP_LoadEntities();
    App.MXP_LoadColours();

    // ── Initialise high-resolution performance counter ───────────────────────
    // QueryPerformanceFrequency() on POSIX returns 1 GHz (nanoseconds).
    // Setting m_iCounterFrequency enables script-time profiling and GetInfo(232).
    {
        LARGE_INTEGER li;
        if (QueryPerformanceFrequency(&li))
            App.m_iCounterFrequency = li.QuadPart;
    }

    // ── Set up the document (world state) ────────────────────────────────────
    CMUSHclientDoc doc;
    g_doc = &doc;
    doc.m_bConvertGAtoNewline = 1;   // treat IAC GA as newline
    doc.m_bNAWS               = TRUE; // negotiate window size with server
    doc.m_bUTF_8              = 1;    // advertise UTF-8 and pass through multibyte chars

    // Wire world.Save() Lua API: save to the loaded world file path.
    doc.m_fn_save = [&doc]() -> bool {
        if (doc.m_strWorldFilePath.IsEmpty()) return false;
        return doc.OnSaveDocument((const char*)doc.m_strWorldFilePath) != FALSE;
    };

    // ── Load world file if specified ──────────────────────────────────────────
    if (worldFile) {
        if (!doc.LoadWorldFromFile(worldFile)) {
            fprintf(stderr, "Error: could not load world file \"%s\"\n", worldFile);
            return 1;
        }
        // World file may supply host/port; command-line args override
        if (host)
            doc.m_mush_name = CString(host);
        if (port)
            doc.m_port = (unsigned short)port;
    } else {
        if (host) doc.m_mush_name = CString(host);
        if (port) doc.m_port = (unsigned short)port;
    }

    if (doc.m_mush_name.IsEmpty() || doc.m_port == 0) {
        fprintf(stderr, "Error: no host/port specified (use <host> <port> or "
                        "-w <world.mcl> with site/port set)\n");
        fprintf(stderr, "Usage: %s [-d] [-w <world.mcl>] [<host> <port>]\n", argv[0]);
        return 1;
    }

    host = (const char*)doc.m_mush_name;
    port = doc.m_port;

    // Seed m_nWrapColumn with the current terminal width
    {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            doc.m_nWrapColumn = ws.ws_col;
    }

    // ── Create and connect the socket ────────────────────────────────────────
    CWorldSocket* sock = new CWorldSocket(&doc);
    doc.m_pSocket = sock;

    if (!sock->Create()) {
        fprintf(stderr, "Error: could not create socket\n");
        return 1;
    }

    // Always resolve the MUD address and store it in m_sockAddr.
    // When using a proxy the TCP connection goes to the proxy server, but
    // ProcessProxyResponse3 / SendProxyConnectSequence need the resolved
    // MUD address to tell the proxy where to connect.
    {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[16];
        snprintf(port_str, sizeof port_str, "%u", port);
        if (::getaddrinfo(host, port_str, &hints, &res) == 0 && res) {
            doc.m_sockAddr = *reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
            ::freeaddrinfo(res);
        }
    }

    const char* connect_host = host;
    unsigned    connect_port = port;

    if (doc.m_iSocksProcessing != eProxyServerNone &&
        !doc.m_strProxyServerName.IsEmpty() &&
        doc.m_iProxyServerPort != 0) {
        connect_host = (const char*)doc.m_strProxyServerName;
        connect_port = doc.m_iProxyServerPort;
        fprintf(stderr, "[Connecting via %s proxy at %s port %u ...]\n",
                doc.m_iSocksProcessing == eProxyServerSocks4 ? "SOCKS4" : "SOCKS5",
                connect_host, connect_port);
    } else {
        fprintf(stderr, "[Connecting to %s port %u ...]\n", host, port);
    }

    doc.m_iConnectPhase = eConnectConnecting;

    if (!sock->Connect(connect_host, connect_port)) {
        if (sock->GetLastError() != WSAEWOULDBLOCK) {
            fprintf(stderr, "Error: connect failed immediately (%d)\n",
                    sock->GetLastError());
            return 1;
        }
    }

    // ── Set up input ──────────────────────────────────────────────────────────
#ifdef HAVE_READLINE
    // Install readline callback handler; "" = no persistent prompt
    // (the MUD sends its own prompts as output text).
    rl_callback_handler_install("", readline_line_handler);
    g_before_output = before_output_rl;
    g_after_output  = after_output_rl;
#else
    make_stdin_nonblocking();
    char  line_buf[4096];
    int   line_len = 0;
#endif

    // ── Event loop ────────────────────────────────────────────────────────────
    // CAsyncSocket::Poll() handles socket events (connect / receive / send).
    // We check stdin for user input and forward complete lines to the MUD.

    // Timer tracking: fire CheckTimerList every ~100 ms
    struct timespec ts_last_timer;
    clock_gettime(CLOCK_MONOTONIC, &ts_last_timer);

    // Script-file monitoring: track last check time for 2 s poll interval
    struct timespec ts_last_script_check = ts_last_timer;

    while (!g_quit) {

        // Poll sockets (10 ms timeout keeps CPU usage low)
        CAsyncSocket::Poll(10);

        // Fire timers every ~100 ms; also poll script file mtime every ~2 s
        {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            long ms = (ts_now.tv_sec  - ts_last_timer.tv_sec)  * 1000
                    + (ts_now.tv_nsec - ts_last_timer.tv_nsec) / 1000000;
            if (ms >= 100) {
                ts_last_timer = ts_now;
                if (doc.m_bEnableTimers && !doc.m_CurrentPlugin) {
                    // World timers
                    doc.CheckTimerList(doc.GetTimerMap());
                    // Plugin timers — mirrors CheckTimers() in timers.cpp
                    for (PluginListIterator pit = doc.m_PluginList.begin();
                         pit != doc.m_PluginList.end(); ++pit) {
                        doc.m_CurrentPlugin = *pit;
                        if (doc.m_CurrentPlugin->m_bEnabled)
                            doc.CheckTimerList(doc.GetTimerMap());
                    }
                    doc.m_CurrentPlugin = nullptr;
                    // Per-tick plugin callback
                    doc.SendToAllPluginCallbacks(ON_PLUGIN_TICK);
                }

                // Script file auto-reload check every 2 s
                long ms2 = (ts_now.tv_sec  - ts_last_script_check.tv_sec)  * 1000
                         + (ts_now.tv_nsec - ts_last_script_check.tv_nsec) / 1000000;
                if (ms2 >= 2000) {
                    ts_last_script_check = ts_now;
                    doc.OnScriptFileChanged(false);
                }
            }
        }

        // Check for disconnect
        if (doc.m_iConnectPhase == eConnectNotConnected) {
            fprintf(stderr, "\n[Disconnected]\n");
            break;
        }

        // Handle terminal resize — update wrap column and re-send NAWS
        if (g_winch) {
            g_winch = false;
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
                doc.m_nWrapColumn = ws.ws_col;
                if (doc.m_bNAWS && doc.m_iConnectPhase == eConnectConnected)
                    doc.SendWindowSizes(doc.m_nWrapColumn);
            }
        }

#ifdef HAVE_READLINE
        // Check if stdin has data; if so, let readline process one character.
        {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(STDIN_FILENO, &rd);
            struct timeval tv = {0, 0};
            if (::select(STDIN_FILENO + 1, &rd, nullptr, nullptr, &tv) > 0)
                rl_callback_read_char();
        }
#else
        // Basic fallback: non-blocking character-by-character read.
        char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n == 1) {
            if (ch == '\n' || ch == '\r') {
                if (line_len > 0) {
                    send_line(line_buf, line_len);
                    line_len = 0;
                }
            } else if (ch == 4 /* Ctrl-D */) {
                g_quit = true;
            } else {
                if (line_len < (int)sizeof(line_buf) - 1)
                    line_buf[line_len++] = ch;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            g_quit = true;  // stdin closed
        }
#endif
    }

    // ── Clean up ──────────────────────────────────────────────────────────────
#ifdef HAVE_READLINE
    rl_callback_handler_remove();
    // Emit a reset so the terminal is clean after exit
    printf("\033[0m\n");
#endif

    g_before_output = nullptr;
    g_after_output  = nullptr;

    // ── World-close script callback ───────────────────────────────────────────
    // Fire on_world_close before tearing down the world, mirroring the
    // Windows OnCloseDocument / SaveModified path.
    doc.FireWorldClose();

    // ── Auto-save on exit ─────────────────────────────────────────────────────
    // If the world was loaded from a file and save_world_automatically is set,
    // write it back now.  This mirrors doc.cpp SaveModified() behaviour.
    if (doc.m_bSaveWorldAutomatically && !doc.m_strWorldFilePath.IsEmpty()) {
        const char* savePath = (const char*)doc.m_strWorldFilePath;
        if (doc.OnSaveDocument(savePath))
            fprintf(stderr, "[auto-save] saved \"%s\"\n", savePath);
        else
            fprintf(stderr, "[auto-save] failed to save \"%s\"\n", savePath);
    }

    if (doc.m_pSocket) {
        delete doc.m_pSocket;
        doc.m_pSocket = nullptr;
    }

    return 0;
}

#endif // !_WIN32
