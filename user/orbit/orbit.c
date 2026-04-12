// user/orbit/orbit.c — orbit: Orion IRC client

#include "orion.h"

#define ORBIT_BUF    256
#define ORBIT_PATH   128
#define CHAT_DISP    32

static char g_planet[32];
static char g_uname[64];
static int  g_log_fd = -1;
static int  g_ctl_fd = -1;

static void ow(const char *s) { write(STDOUT_FILENO, s, (uint32_t)strlen(s)); }

// ── message ring buffer ───────────────────────────────────────────────────────

static char g_msgs[CHAT_DISP][ORBIT_BUF];
static int  g_msg_head  = 0;
static int  g_msg_count = 0;

static void chat_push(const char *line) {
    strncpy(g_msgs[g_msg_head], line, ORBIT_BUF - 1);
    g_msgs[g_msg_head][ORBIT_BUF - 1] = '\0';
    g_msg_head = (g_msg_head + 1) % CHAT_DISP;
    if (g_msg_count < CHAT_DISP) g_msg_count++;
}

// ── connected users ───────────────────────────────────────────────────────────

static char g_members[16][64];
static int  g_nmembers = 0;

static void refresh_users(const char *ctl_path) {
    int fd = open(ctl_path, O_RDONLY);
    if (fd < 0) return;
    char raw[512];
    int n = read(fd, raw, sizeof(raw) - 1);
    close(fd);
    if (n < 0) n = 0;
    raw[n] = '\0';

    g_nmembers = 0;
    int i = 0;
    while (i < n && g_nmembers < 16) {
        while (i < n && (raw[i] == '\n' || raw[i] == '\r')) i++;
        if (i >= n) break;
        int j = 0;
        while (i < n && raw[i] != '\n' && raw[i] != '\r' && j < 63)
            g_members[g_nmembers][j++] = raw[i++];
        g_members[g_nmembers][j] = '\0';
        if (j > 0) g_nmembers++;
    }
}

// ── full redraw ───────────────────────────────────────────────────────────────

static void full_redraw(void) {
    ow("\033[2J\033[H");

    ow("orbit - empty line to leave\n\n");

    ow("connected users:\n");
    for (int i = 0; i < g_nmembers; i++) {
        ow("* "); ow(g_members[i]); ow("\n");
    }
    ow("\n");

    int oldest = (g_msg_count < CHAT_DISP) ? 0 : g_msg_head;
    for (int i = 0; i < g_msg_count; i++) {
        int slot = (oldest + i) % CHAT_DISP;
        ow(g_msgs[slot]); ow("\n");
    }

    ow("\n");
    ow(g_uname); ow(": ");
}

// ── child: log tailer ─────────────────────────────────────────────────────────
//
//  The VFS advances the fd offset between calls, so each read() returns only
//  bytes written since the last read. No log_pos needed — just act on whatever
//  n bytes come back, the same way tail(1) works on a regular file.

static void tail_child(void) {
    char ctl_path[ORBIT_PATH];
    snprintf(ctl_path, sizeof(ctl_path), "/chat/%s/ctl", g_planet);

    char acc[ORBIT_BUF]; int acc_len = 0;

    refresh_users(ctl_path);
    full_redraw();

    while (1) {
        char buf[ORBIT_BUF];
        int n = read(g_log_fd, buf, sizeof(buf) - 1);

        if (n < 0) _exit(0);

        if (n == 0) {
            sleep(10);
            continue;
        }

        int changed = 0;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (acc_len > 0) {
                    acc[acc_len] = '\0';
                    if (strncmp(acc, "Satellite ", 10) == 0)
                        refresh_users(ctl_path);
                    chat_push(acc);
                    acc_len = 0;
                    changed = 1;
                }
            } else if (acc_len < ORBIT_BUF - 1) {
                acc[acc_len++] = c;
            }
        }

        if (changed) full_redraw();
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 2) { ow("usage: orbit <planet>\n"); return 1; }

    g_uname[0] = '\0';
    int ufd = open("/etc/username", O_RDONLY);
    if (ufd >= 0) {
        int n = read(ufd, g_uname, sizeof(g_uname) - 1);
        close(ufd);
        if (n > 0) {
            g_uname[n] = '\0';
            int l = n;
            while (l > 0 && (g_uname[l-1] == '\n' || g_uname[l-1] == '\r'))
                g_uname[--l] = '\0';
        }
    }
    if (!g_uname[0]) strncpy(g_uname, "anonymous", sizeof(g_uname) - 1);

    strncpy(g_planet, argv[1], sizeof(g_planet) - 1);

    char ctl_path[ORBIT_PATH], log_path[ORBIT_PATH];
    snprintf(ctl_path, sizeof(ctl_path), "/chat/%s/ctl", g_planet);
    snprintf(log_path, sizeof(log_path), "/chat/%s/log", g_planet);

    g_ctl_fd = open(ctl_path, O_WRONLY | O_CREAT);
    if (g_ctl_fd < 0) { ow("orbit: cannot open ctl\n"); return 1; }

    g_log_fd = open(log_path, O_RDONLY);
    if (g_log_fd < 0) { ow("orbit: cannot open log\n"); close(g_ctl_fd); return 1; }

    char cmd[ORBIT_BUF];
    snprintf(cmd, sizeof(cmd), "join %s\n", g_uname);
    write(g_ctl_fd, cmd, (uint32_t)strlen(cmd));

    int child = fork((uint32_t)tail_child);
    if (child < 0) { ow("orbit: fork failed\n"); return 1; }

    // parent: read stdin one char at a time, echo it, send complete lines
    char line[ORBIT_BUF];
    uint32_t len = 0;
    char c;

    while (1) {
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) break;

        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            if (len == 0) break;                        // empty line = leave
            snprintf(cmd, sizeof(cmd), "msg %s:%s\n", g_uname, line);
            write(g_ctl_fd, cmd, (uint32_t)strlen(cmd));
            len = 0;
            ow("\n");                                   // move cursor to new line
            // child will redraw when it picks up the log entry

        } else if ((c == '\b' || c == 127) && len > 0) {
            len--;
            ow("\b \b");

        } else if (c >= 0x20 && c <= 0x7E && len < sizeof(line) - 1) {
            line[len++] = c;
            write(STDOUT_FILENO, &c, 1);
        }
    }

    kill(child, SIGKILL);
    snprintf(cmd, sizeof(cmd), "leave %s\n", g_uname);
    write(g_ctl_fd, cmd, (uint32_t)strlen(cmd));
    close(g_ctl_fd);
    close(g_log_fd);

    ow("\033[2J\033[H");
    ow("orbit: left "); ow(g_planet); ow("\n");
    return 0;
}